// SDK 22.2.8's AV1 decoder interface, following dm/host/hle_codec_video.c.
// The guest owns the pool objects and event storage; the host owns the
// packets, decoded NV12 pictures, and the decoder worker.
#include "m2_host.h"
#include "../../host/hle_movie_av1.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODEC_RESULT(description) (0x6fu | ((description) << 9))
enum {
    VIDEO_INVALID = CODEC_RESULT(2004), VIDEO_BUSY = CODEC_RESULT(3603), VIDEO_NONE = CODEC_RESULT(3604),
    VIDEO_PREF = 0x56436d44, VIDEO_SYNC = 0x53436d44,
    VIDEO_EVENT_OFFSET = 0x100, VIDEO_SLOTS = 32, VIDEO_READY = 4, VIDEO_HOLD = 12,
    VIDEO_ALIGN = 0x4000,
};

// SDK+0x331c30 writes a 64-byte object through x8. Only our pool functions
// consume it; carry the original dimensions and format rather than GPU layout.
struct video_preference {
    uint16_t w, h;
    uint32_t format, tiling, magic;
    unsigned char reserved[48];
};
_Static_assert(sizeof(struct video_preference) == 64, "SDK VideoDecoderImagePreference");

struct video_sync { uint32_t magic, id, slot, generation; };
_Static_assert(sizeof(struct video_sync) == 16, "SDK NativeSyncHandle");
struct video_wait {
    struct video_wait *next;
    void *event;
    uint32_t handle;
    struct video_sync sync;
};

struct picture {
    struct picture *next;
    size_t capacity;
    mav1_picture image;
};
struct pool_image { struct picture *frame; bool eos; };
struct video_pool {
    uint16_t w, h;
    uint32_t count;
};
struct pool_object { struct video_pool *pool; uint32_t magic; };

static struct pool_image *pool_images(const struct video_pool *pool) {
    return (struct pool_image *)(pool + 1);
}

struct packet { unsigned char *bytes; size_t length; int64_t timestamp; bool eos; };
struct slot {
    struct video_pool *pool;
    uint32_t image, generation, completed;
    bool acquirable, waiting, filled, eos;
};
struct order { unsigned head, count; uint8_t index[VIDEO_SLOTS]; };
struct video_decoder {
    struct video_decoder *next;
    uint32_t id, epoch, ready_handle;
    uint64_t submissions, pictures;
    void *event;
    HANDLE thread;
    CONDITION_VARIABLE work;
    mav1 *decoder;
    bool busy, quit, reset, eos_pending;
    struct packet packets[VIDEO_HOLD];
    unsigned packet_head, packet_count;
    struct slot slots[VIDEO_SLOTS];
    struct order acquire, release;
    struct picture *held[VIDEO_HOLD], *recycled;
    unsigned held_head, held_count;
};

// The worker does not enter guest code. This lock protects publication of
// pictures, kernel event transitions, and decoder/pool lifetimes; dav1d and
// the borrowed-picture copy run without it.
static SRWLOCK video_lock = SRWLOCK_INIT;
static struct video_decoder *decoders;
static struct video_wait *waits;
static uint32_t next_id;

static struct dm_guest_result call_sdk(const char *name, uintptr_t a, uintptr_t b, uintptr_t c) {
    uintptr_t entry = dm_guest_sdk_symbol(name);
    const uint64_t args[8] = {a, b, c};
    struct dm_guest_result r;
    if (!entry || !dm_guest_call(entry, args, &r)) dm_fail("video: cannot call SDK %s", name);
    return r;
}
static uint32_t create_event(void *event, uint32_t *handle) {
    uint32_t r = (uint32_t)call_sdk("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb",
                                    (uintptr_t)event, 0, 1).x0; // Manual clear, guest kernel handles.
    if (!r) *handle = (uint32_t)call_sdk("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE",
                                         (uintptr_t)event, 0, 0).x0;
    return r;
}
static void destroy_event(void *event) {
    call_sdk("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE", (uintptr_t)event, 0, 0);
}

static void push(struct order *q, uint32_t index) {
    q->index[(q->head + q->count++) % VIDEO_SLOTS] = (uint8_t)index;
}
static uint32_t pop(struct order *q) {
    uint32_t index = q->index[q->head];
    q->head = (q->head + 1) % VIDEO_SLOTS;
    --q->count;
    return index;
}
static void remove_index(struct order *q, uint32_t unwanted) {
    for (unsigned n = q->count; n; --n) {
        uint32_t index = pop(q);
        if (index != unwanted) push(q, index);
    }
}

static struct video_pool *pool_of(const struct pool_object *object) {
    return object && object->magic == VIDEO_PREF ? object->pool : NULL;
}
static unsigned pending(const struct video_decoder *d) {
    return d->packet_count + d->held_count + (unsigned)d->busy;
}
static void signal_ready(struct video_decoder *d, unsigned before, bool took) {
    if (pending(d) >= VIDEO_READY) dm_clear_event(d->ready_handle);
    else if (took || before >= VIDEO_READY) dm_signal_event(d->ready_handle);
}
static bool sync_reached(const struct video_sync *sync) {
    // Generation 0 is the initially available buffer. Empty / foreign
    // release handles are not waits on this CPU-only path.
    if (sync->magic != VIDEO_SYNC || !sync->generation) return true;
    for (const struct video_decoder *d = decoders; d; d = d->next) {
        if (d->id != sync->id) continue;
        if (sync->slot >= VIDEO_SLOTS) return false;
        const struct slot *s = &d->slots[sync->slot];
        return !s->pool || s->completed >= sync->generation;
    }
    return true;
}
static void signal_output(uint32_t id, uint32_t index) {
    for (struct video_wait *w = waits; w; w = w->next) {
        if (w->sync.id != id || (index < VIDEO_SLOTS && w->sync.slot != index) || !sync_reached(&w->sync)) continue;
        w->sync = (struct video_sync){0};
        dm_signal_event(w->handle);
    }
}
static void recycle(struct video_decoder *d, struct picture *p) {
    if (!p) return;
    p->next = d->recycled;
    d->recycled = p;
}
static void fill_output(struct video_decoder *d) {
    unsigned had = d->held_count;
    while (d->release.count && (d->held_count || d->eos_pending)) {
        uint32_t index = pop(&d->release);
        struct slot *s = &d->slots[index];
        struct picture *p = NULL;
        if (d->held_count) {
            p = d->held[d->held_head];
            d->held_head = (d->held_head + 1) % VIDEO_HOLD;
            --d->held_count;
            if (!d->pictures++) {
                printf("[codec] AV1 decoder %u: first picture %dx%d (timestamp %lld)\n",
                       d->id, p->image.w, p->image.h, (long long)p->image.ts);
                fflush(stdout);
            }
        }
        s->eos = d->eos_pending && !d->held_count;
        if (s->eos) d->eos_pending = false;
        pool_images(s->pool)[s->image] = (struct pool_image){p, s->eos};
        s->waiting = false;
        s->filled = true;
        s->completed = s->generation;
        signal_output(d->id, index);
    }
    if (had == VIDEO_HOLD && d->held_count < VIDEO_HOLD) WakeConditionVariable(&d->work);
}
static void detach_slot(struct video_decoder *d, uint32_t index) {
    struct slot *s = &d->slots[index];
    remove_index(&d->acquire, index);
    remove_index(&d->release, index);
    // Preserve the generation across reuse of this index.
    *s = (struct slot){.generation = s->generation + 1, .completed = s->generation + 1};
    signal_output(d->id, index);
}

uint32_t video_initialize(void) { return 0; }
uint32_t video_finalize(void) { return 0; }
void video_preference(uint16_t w, uint16_t h, uint32_t format, uint32_t tiling, struct video_preference *out) {
    *out = (struct video_preference){.w = w, .h = h, .format = format, .tiling = tiling, .magic = VIDEO_PREF};
}
uint32_t video_pool_size(size_t *size, size_t *align, const struct video_preference *pref, uint32_t count) {
    if (pref->magic != VIDEO_PREF || !pref->w || !pref->h) return VIDEO_INVALID;
    // Match the canonical decoder pool sizing, including row and page alignment.
    size_t stride = ((size_t)pref->w * 4 + 63) & ~(size_t)63;
    size_t image = (stride * pref->h + VIDEO_ALIGN - 1) & ~(size_t)(VIDEO_ALIGN - 1);
    if (!count) count = 1;
    if (count > SIZE_MAX / image) return VIDEO_INVALID;
    *size = image * count;
    *align = VIDEO_ALIGN;
    return 0;
}
uint32_t video_pool_initialize(struct pool_object *object, void *memory, size_t size,
                               const struct video_preference *pref, uint32_t count) {
    size_t required, align;
    if (video_pool_size(&required, &align, pref, count) || !memory || size < required) return VIDEO_INVALID;
    if (!count) count = 1;
    struct video_pool *pool = calloc(1, sizeof(*pool) + (size_t)count * sizeof(struct pool_image));
    if (!pool) return VIDEO_INVALID;
    pool->w = pref->w;
    pool->h = pref->h;
    pool->count = count;
    AcquireSRWLockExclusive(&video_lock);
    *object = (struct pool_object){pool, VIDEO_PREF};
    ReleaseSRWLockExclusive(&video_lock);
    return 0;
}
void video_pool_finalize(struct pool_object *object) {
    AcquireSRWLockExclusive(&video_lock);
    struct video_pool *pool = pool_of(object);
    if (pool) {
        *object = (struct pool_object){0};
        for (struct video_decoder *d = decoders; d; d = d->next)
            for (uint32_t i = 0; i < VIDEO_SLOTS; ++i)
                if (d->slots[i].pool == pool) detach_slot(d, i);
    }
    ReleaseSRWLockExclusive(&video_lock);
    if (!pool) return;
    for (uint32_t i = 0; i < pool->count; ++i) free(pool_images(pool)[i].frame);
    free(pool);
}

static bool plane_fits(size_t stride, size_t length, unsigned width, unsigned height) {
    return width <= stride && width <= length && (!height || height == 1 || stride <= (length - width) / (height - 1));
}
static void copy_plane(unsigned char *dst, size_t stride, const unsigned char *src, size_t pitch,
                       unsigned width, unsigned height) {
    if (stride == width && pitch == width) {
        memcpy(dst, src, (size_t)width * height);
        return;
    }
    for (unsigned y = 0; y < height; ++y) memcpy(dst + y * stride, src + y * pitch, width);
}
static unsigned overlap(unsigned origin, unsigned length, unsigned limit) {
    if (origin >= limit) return 0;
    return (length < limit - origin) ? length : limit - origin;
}
static void read_plane(unsigned char *dst, size_t stride, const unsigned char *src, size_t pitch,
                       unsigned x, unsigned y, unsigned w, unsigned h, unsigned srcw, unsigned srch, int padding) {
    unsigned cw = overlap(x, w, srcw), ch = overlap(y, h, srch);
    if (cw && ch) copy_plane(dst, stride, src + y * pitch + x, pitch, cw, ch);
    if (cw < w) for (unsigned row = 0; row < ch; ++row) memset(dst + row * stride + cw, padding, w - cw);
    for (unsigned row = ch; row < h; ++row) memset(dst + row * stride, padding, w);
}
static const mav1_picture *image_of(const struct pool_object *object, uint32_t index, unsigned x, unsigned y,
                                    unsigned w, unsigned h, uint32_t *result) {
    const struct video_pool *pool = pool_of(object);
    if (!pool || index >= pool->count || !w || !h || x + w > pool->w || y + h > pool->h) {
        *result = VIDEO_INVALID;
        return NULL;
    }
    const struct pool_image *image = &pool_images(pool)[index];
    // The SDK's CPU acquire calls ReadBuffer even on a standalone EOS
    // marker. It has no pixels; succeed without touching the destination.
    *result = image->frame || image->eos ? 0 : VIDEO_NONE;
    return image->frame ? &image->frame->image : NULL;
}
uint32_t video_read_nv12(const struct pool_object *pool, uint32_t index, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         unsigned char *luma, size_t ystride, size_t ybytes,
                         unsigned char *chroma, size_t uvstride, size_t uvbytes) {
    unsigned uvw = ((unsigned)w + 1) & ~1u, uvh = ((unsigned)h + 1) / 2;
    if (!luma || !chroma || ((x | y) & 1) || !plane_fits(ystride, ybytes, w, h) ||
        !plane_fits(uvstride, uvbytes, uvw, uvh)) return VIDEO_INVALID;
    AcquireSRWLockShared(&video_lock);
    uint32_t result;
    const mav1_picture *p = image_of(pool, index, x, y, w, h, &result);
    if (p) {
        read_plane(luma, ystride, p->y, p->ystride, x, y, w, h, (unsigned)p->w, (unsigned)p->h, p->full_range ? 0 : 16);
        read_plane(chroma, uvstride, p->uv, p->uvstride, x, y / 2u, uvw, uvh,
                   ((unsigned)p->w + 1) & ~1u, ((unsigned)p->h + 1) / 2, 128);
    }
    ReleaseSRWLockShared(&video_lock);
    return result;
}

static unsigned clamp_rgb(int v, unsigned max) {
    return v < 0 ? 0u : ((unsigned)v > max ? max : (unsigned)v);
}
static void convert_rgba(const mav1_picture *p, unsigned x, unsigned y, unsigned w, unsigned h,
                         unsigned char *dst, size_t stride, bool hdr) {
    // Y'CbCr matrix coefficients. The 4-byte HDR read uses RGB10A2 rather
    // than RGBA8. The dav1d adapter supplies normalized 8-bit NV12 samples.
    static const double matrix[][4] = {
        {1.402, 0.344136, 0.714136, 1.772}, // BT.601
        {1.5748, 0.187324, 0.468124, 1.8556}, // BT.709
        {1.4746, 0.164553, 0.571353, 1.8814}, // BT.2020
    };
    const double *m = matrix[p->matrix == 5 ? 2 : (p->matrix == 2 ? 1 : 0)];
    unsigned max = hdr ? 1023 : 255, shift = hdr ? 10 : 8;
    double yc = (double)max * 65536.0 / (p->full_range ? 255.0 : 219.0);
    double cc = (double)max * 65536.0 / (p->full_range ? 255.0 : 224.0);
    int cy = (int)yc, cr = (int)(m[0] * cc), cgu = (int)(m[1] * cc), cgv = (int)(m[2] * cc), cb = (int)(m[3] * cc);
    int black = p->full_range ? 0 : 16;
    unsigned cw = overlap(x, w, (unsigned)p->w), ch = overlap(y, h, (unsigned)p->h);
    uint32_t alpha = UINT32_MAX << (3 * shift);
    for (unsigned row = 0; row < h; ++row) {
        unsigned char *out = dst + row * stride;
        unsigned col = 0;
        if (row < ch && cw) {
            const unsigned char *luma = p->y + (y + row) * p->ystride + x;
            const unsigned char *uv = p->uv + ((y + row) / 2) * p->uvstride;
            for (; col < cw; ++col) {
                unsigned chroma = (x + col) & ~1u;
                int l = (luma[col] - black) * cy + 32768, u = uv[chroma] - 128, v = uv[chroma + 1] - 128;
                unsigned r = clamp_rgb((l + cr * v) >> 16, max);
                unsigned g = clamp_rgb((l - cgu * u - cgv * v) >> 16, max);
                unsigned b = clamp_rgb((l + cb * u) >> 16, max);
                uint32_t packed = alpha | (b << (2 * shift)) | (g << shift) | r;
                memcpy(out + (size_t)col * 4, &packed, sizeof(packed));
            }
        }
        for (; col < w; ++col) memcpy(out + (size_t)col * 4, &alpha, sizeof(alpha));
    }
}
static uint32_t read_rgba(const struct pool_object *pool, uint32_t index, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          unsigned char *dst, size_t stride, size_t bytes, bool hdr) {
    if (!dst || !plane_fits(stride, bytes, (unsigned)w * 4, h)) return VIDEO_INVALID;
    AcquireSRWLockShared(&video_lock);
    uint32_t result;
    const mav1_picture *p = image_of(pool, index, x, y, w, h, &result);
    if (p) convert_rgba(p, x, y, w, h, dst, stride, hdr);
    ReleaseSRWLockShared(&video_lock);
    return result;
}
uint32_t video_read_rgba(const struct pool_object *pool, uint32_t index, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                         unsigned char *dst, size_t stride, size_t bytes) {
    return read_rgba(pool, index, x, y, w, h, dst, stride, bytes, false);
}
uint32_t video_read_rgba_hdr(const struct pool_object *pool, uint32_t index, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                             unsigned char *dst, size_t stride, size_t bytes) {
    return read_rgba(pool, index, x, y, w, h, dst, stride, bytes, true);
}

// Transfer ownership out of the ring; an empty slot must never retain a
// pointer that the worker or reset path is about to release.
static struct packet pop_packet(struct video_decoder *d) {
    struct packet *slot = &d->packets[d->packet_head];
    struct packet packet = *slot;
    slot->bytes = NULL;
    d->packet_head = (d->packet_head + 1) % VIDEO_HOLD;
    --d->packet_count;
    return packet;
}
static void discard_queued(struct video_decoder *d) {
    // pop_packet leaves inactive slots empty. Visit each slot exactly once,
    // including when the queued interval wraps through index zero.
    for (unsigned i = 0; i < VIDEO_HOLD; ++i) {
        free(d->packets[i].bytes);
        d->packets[i].bytes = NULL;
    }
    d->packet_count = d->packet_head = 0;
    for (unsigned i = 0; i < d->held_count; ++i) recycle(d, d->held[(d->held_head + i) % VIDEO_HOLD]);
    d->held_count = 0;
    d->eos_pending = false;
}
static bool wait_for_room(struct video_decoder *d, uint32_t epoch) {
    while (!d->quit && epoch == d->epoch && d->held_count == VIDEO_HOLD)
        SleepConditionVariableSRW(&d->work, &video_lock, INFINITE, 0);
    return !d->quit && epoch == d->epoch;
}
static struct picture *pop_recycled(struct video_decoder *d) {
    struct picture *p = d->recycled;
    if (p) d->recycled = p->next;
    return p;
}
static struct picture *retain_picture(struct video_decoder *d, const mav1_picture *p, uint32_t epoch) {
    AcquireSRWLockExclusive(&video_lock);
    struct picture *owned = wait_for_room(d, epoch) ? pop_recycled(d) : NULL;
    bool current = !d->quit && epoch == d->epoch;
    ReleaseSRWLockExclusive(&video_lock);
    if (!current) return NULL;

    unsigned w = (unsigned)p->w, h = (unsigned)p->h, uvw = (w + 1) & ~1u, uvh = (h + 1) / 2;
    size_t ybytes = (size_t)w * h, bytes = ybytes + (size_t)uvw * uvh;
    if (!owned || owned->capacity < bytes) {
        struct picture *grown = realloc(owned, sizeof(*owned) + bytes);
        if (!grown) dm_fail("video: out of memory retaining AV1 picture %ux%u", w, h);
        owned = grown;
        owned->capacity = bytes;
    }
    owned->image = *p;
    unsigned char *storage = (unsigned char *)(owned + 1);
    owned->image.y = storage;
    owned->image.uv = storage + ybytes;
    owned->image.ystride = w;
    owned->image.uvstride = uvw;
    copy_plane(storage, w, p->y, p->ystride, w, h);
    copy_plane(storage + ybytes, uvw, p->uv, p->uvstride, uvw, uvh);
    return owned;
}
static bool publish(struct video_decoder *d, struct picture *p, uint32_t epoch, bool eos, bool packet_done) {
    AcquireSRWLockExclusive(&video_lock);
    bool current = wait_for_room(d, epoch);
    unsigned before = pending(d);
    if (packet_done) d->busy = false;
    if (current) {
        if (p) d->held[(d->held_head + d->held_count++) % VIDEO_HOLD] = p;
        if (eos) d->eos_pending = true;
        fill_output(d);
    } else recycle(d, p);
    if (packet_done) signal_ready(d, before, false);
    ReleaseSRWLockExclusive(&video_lock);
    return current;
}
static bool take_picture(struct video_decoder *d, const mav1_picture *p, uint32_t epoch, bool eos, struct picture **last) {
    if (*last) {
        struct picture *previous = *last;
        *last = NULL;
        if (!publish(d, previous, epoch, false, false)) return false;
    }
    struct picture *owned = retain_picture(d, p, epoch);
    if (!owned) return false;
    if (eos) { *last = owned; return true; }
    return publish(d, owned, epoch, false, false);
}
static int decode_packet(mav1 *decoder, struct packet packet, mav1_picture *picture) {
    if (!packet.bytes) return 0; // Empty EOS marker.
    int status = mav1_decode(decoder, packet.bytes, packet.length, packet.timestamp, picture);
    free(packet.bytes);
    return status;
}
static DWORD WINAPI decoder_worker(void *arg) {
    struct video_decoder *d = arg;
    for (;;) {
        AcquireSRWLockExclusive(&video_lock);
        while (!d->quit && (!d->packet_count || d->held_count == VIDEO_HOLD))
            SleepConditionVariableSRW(&d->work, &video_lock, INFINITE, 0);
        if (d->quit) { ReleaseSRWLockExclusive(&video_lock); return 0; }
        struct packet packet = pop_packet(d);
        uint32_t epoch = d->epoch;
        bool reset = d->reset;
        d->reset = false;
        d->busy = true;
        ReleaseSRWLockExclusive(&video_lock);

        if (reset) mav1_reset(d->decoder);
        mav1_picture picture;
        int status = decode_packet(d->decoder, packet, &picture);
        struct picture *last = NULL;
        bool current = true;
        while (status == 1 && (current = take_picture(d, &picture, epoch, packet.eos, &last)))
            status = mav1_next(d->decoder, 0, &picture);
        AcquireSRWLockShared(&video_lock);
        bool drain = current && epoch == d->epoch && !d->quit && (packet.eos || !d->packet_count);
        ReleaseSRWLockShared(&video_lock);
        while (drain && mav1_next(d->decoder, 1, &picture) == 1)
            drain = take_picture(d, &picture, epoch, packet.eos, &last);
        publish(d, last, epoch, packet.eos, true);
    }
}

static uint32_t initialize_decoder(struct video_decoder **object, uint16_t w, uint16_t h, uint32_t input, uint32_t output,
                                   uint32_t mode) {
    struct video_decoder *d = calloc(1, sizeof(*d));
    if (!d) return VIDEO_INVALID;
    d->decoder = mav1_new(NULL);
    if (!d->decoder) { free(d); return VIDEO_INVALID; }
    d->event = (unsigned char *)object + VIDEO_EVENT_OFFSET;
    uint32_t result = create_event(d->event, &d->ready_handle);
    if (!result) {
        InitializeConditionVariable(&d->work);
        d->thread = CreateThread(NULL, 0, decoder_worker, d, 0, NULL);
        if (d->thread) {
            AcquireSRWLockExclusive(&video_lock);
            d->id = ++next_id;
            d->next = decoders;
            decoders = d;
            *object = d;
            dm_signal_event(d->ready_handle);
            ReleaseSRWLockExclusive(&video_lock);
            printf("[codec] AV1 decoder %u: %ux%u, input %u, output %u, mode %u (%s)\n",
                   d->id, w, h, input, output, mode, mav1_backend(d->decoder));
            fflush(stdout);
            return 0;
        }
        result = VIDEO_INVALID;
        destroy_event(d->event);
    }
    mav1_free(d->decoder);
    free(d);
    return result;
}
uint32_t video_av1_initialize_transfer(struct video_decoder **object, void *memory, size_t size, size_t bitstream,
                                       uint16_t w, uint16_t h, uint32_t references, uint32_t input, uint32_t output, uint32_t mode) {
    (void)memory; (void)size; (void)bitstream; (void)references;
    return initialize_decoder(object, w, h, input, output, mode);
}
uint32_t video_av1_initialize_remote(struct video_decoder **object, size_t bitstream,
                                     uint16_t w, uint16_t h, uint32_t references, uint32_t input, uint32_t output, uint32_t mode) {
    (void)bitstream; (void)references;
    return initialize_decoder(object, w, h, input, output, mode);
}
uint32_t video_av1_finalize(struct video_decoder **object) {
    AcquireSRWLockExclusive(&video_lock);
    struct video_decoder *d = *object;
    if (d) {
        *object = NULL;
        struct video_decoder **link = &decoders;
        while (*link != d) link = &(*link)->next;
        *link = d->next;
        d->quit = true;
        signal_output(d->id, VIDEO_SLOTS);
        WakeConditionVariable(&d->work);
    }
    ReleaseSRWLockExclusive(&video_lock);
    if (!d) return 0;
    WaitForSingleObject(d->thread, INFINITE);
    CloseHandle(d->thread);
    destroy_event(d->event);
    discard_queued(d);
    while (d->recycled) free(pop_recycled(d));
    mav1_free(d->decoder);
    free(d);
    return 0;
}
uint32_t video_av1_register(struct video_decoder **object, uint32_t index, struct pool_object *pool_obj, uint32_t image) {
    AcquireSRWLockExclusive(&video_lock);
    struct video_decoder *d = *object;
    struct video_pool *pool = pool_of(pool_obj);
    bool ok = d && index < VIDEO_SLOTS && !d->slots[index].pool && pool && image < pool->count;
    if (ok) {
        struct slot *s = &d->slots[index];
        s->pool = pool;
        s->image = image;
        s->acquirable = true;
        push(&d->acquire, index);
    }
    ReleaseSRWLockExclusive(&video_lock);
    return ok ? 0 : VIDEO_INVALID;
}
uint32_t video_av1_start(struct video_decoder **object) {
    AcquireSRWLockExclusive(&video_lock);
    struct video_decoder *d = *object;
    if (d) signal_ready(d, 0, true);
    ReleaseSRWLockExclusive(&video_lock);
    return d ? 0 : VIDEO_INVALID;
}
uint32_t video_av1_reset(struct video_decoder **object) {
    AcquireSRWLockExclusive(&video_lock);
    struct video_decoder *d = *object;
    if (d) {
        discard_queued(d);
        ++d->epoch;
        d->reset = true;
        dm_signal_event(d->ready_handle);
        WakeConditionVariable(&d->work);
    }
    ReleaseSRWLockExclusive(&video_lock);
    return 0;
}
void *video_av1_submission_event(struct video_decoder **object) {
    AcquireSRWLockShared(&video_lock);
    void *event = *object ? (*object)->event : NULL;
    ReleaseSRWLockShared(&video_lock);
    return event;
}
uint32_t video_av1_submit(uint64_t *out, struct video_decoder **object, const void *data, size_t bytes, int64_t timestamp, bool eos) {
    AcquireSRWLockExclusive(&video_lock);
    struct video_decoder *d = *object;
    uint32_t r = VIDEO_INVALID;
    if (d && (!bytes || data)) {
        unsigned char *copy = NULL;
        if (pending(d) >= VIDEO_HOLD || (bytes && !(copy = malloc(bytes)))) {
            dm_clear_event(d->ready_handle);
            r = VIDEO_BUSY;
        } else {
            if (bytes) memcpy(copy, data, bytes);
            d->packets[(d->packet_head + d->packet_count++) % VIDEO_HOLD] = (struct packet){copy, bytes, timestamp, eos};
            *out = ++d->submissions;
            signal_ready(d, 0, true);
            WakeConditionVariable(&d->work);
            r = 0;
        }
    }
    ReleaseSRWLockExclusive(&video_lock);
    return r;
}
uint32_t video_av1_acquire(uint32_t *index, struct video_sync *sync, struct video_decoder **object) {
    AcquireSRWLockExclusive(&video_lock);
    struct video_decoder *d = *object;
    uint32_t r = VIDEO_INVALID;
    if (d) {
        r = VIDEO_NONE;
        if (d->acquire.count) {
            uint32_t i = pop(&d->acquire);
            struct slot *s = &d->slots[i];
            s->acquirable = false;
            *index = i;
            *sync = (struct video_sync){VIDEO_SYNC, d->id, i, s->generation};
            r = 0;
        }
    }
    ReleaseSRWLockExclusive(&video_lock);
    return r;
}
void video_av1_release(struct video_decoder **object, uint32_t index, struct video_sync sync) {
    (void)sync; // The CPU reader has no GPU retirement dependency.
    AcquireSRWLockExclusive(&video_lock);
    struct video_decoder *d = *object;
    if (d && index < VIDEO_SLOTS) {
        struct slot *s = &d->slots[index];
        if (s->pool && !s->acquirable) {
            struct pool_image *image = &pool_images(s->pool)[s->image];
            recycle(d, image->frame);
            *image = (struct pool_image){0};
            ++s->generation;
            s->filled = false;
            s->acquirable = true;
            push(&d->acquire, index);
            if (!s->waiting) {
                s->waiting = true;
                push(&d->release, index);
            }
            unsigned before = pending(d);
            fill_output(d);
            signal_ready(d, before, false);
        }
    }
    ReleaseSRWLockExclusive(&video_lock);
}
uint32_t video_av1_properties(uint16_t *w, uint16_t *h, uint16_t *cropw, uint16_t *croph, int64_t *ts, bool *eos,
                               struct video_decoder **object, uint32_t index) {
    AcquireSRWLockShared(&video_lock);
    struct video_decoder *d = *object;
    uint32_t r = VIDEO_INVALID;
    if (d && index < VIDEO_SLOTS && d->slots[index].pool) {
        const struct slot *s = &d->slots[index];
        r = VIDEO_NONE;
        if (s->filled) {
            const struct picture *p = pool_images(s->pool)[s->image].frame;
            *w = *cropw = p ? (uint16_t)p->image.w : 0;
            *h = *croph = p ? (uint16_t)p->image.h : 0;
            *ts = p ? p->image.ts : 0;
            *eos = s->eos;
            r = 0;
        }
    }
    ReleaseSRWLockShared(&video_lock);
    return r;
}

bool video_try_native_sync(const struct video_sync *sync) {
    AcquireSRWLockShared(&video_lock);
    bool reached = sync_reached(sync);
    ReleaseSRWLockShared(&video_lock);
    return reached;
}
static struct video_wait **find_wait(void *event) {
    struct video_wait **link = &waits;
    while (*link && (*link)->event != event) link = &(*link)->next;
    return link;
}
uint32_t video_create_sync_event(void *event) {
    struct video_wait *w = calloc(1, sizeof(*w));
    if (!w) return VIDEO_INVALID;
    uint32_t r = create_event(event, &w->handle);
    if (r) { free(w); return r; }
    w->event = event;
    AcquireSRWLockExclusive(&video_lock);
    w->next = waits;
    waits = w;
    ReleaseSRWLockExclusive(&video_lock);
    return 0;
}
void video_destroy_sync_event(void *event) {
    AcquireSRWLockExclusive(&video_lock);
    struct video_wait **link = find_wait(event), *w = *link;
    if (w) *link = w->next;
    ReleaseSRWLockExclusive(&video_lock);
    if (w) {
        destroy_event(event);
        free(w);
    }
}
void video_cancel_sync_event(void *event) {
    AcquireSRWLockExclusive(&video_lock);
    struct video_wait *w = *find_wait(event);
    if (w) {
        w->sync = (struct video_sync){0};
        dm_clear_event(w->handle);
    }
    ReleaseSRWLockExclusive(&video_lock);
}
uint32_t video_request_sync_event(void *event, const struct video_sync *sync) {
    AcquireSRWLockExclusive(&video_lock);
    struct video_wait *w = *find_wait(event);
    if (w) {
        dm_clear_event(w->handle);
        if (sync_reached(sync)) {
            w->sync = (struct video_sync){0};
            dm_signal_event(w->handle);
        } else w->sync = *sync;
    }
    ReleaseSRWLockExclusive(&video_lock);
    return w ? 0 : VIDEO_INVALID;
}
void video_initialize_sync_holder(void *holder, void *event) {
    call_sdk("_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_15SystemEventTypeE",
             (uintptr_t)holder, (uintptr_t)event, 0);
}
