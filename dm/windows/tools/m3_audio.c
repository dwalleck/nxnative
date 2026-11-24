// Run the renderer shipped in the guest SDK. Windows consumes its DeviceSink PCM
// on a five-millisecond clock, without opening an audio playback device.
#include "m2_host.h"
#include "m3_audio.h"
#include <stdio.h>
#include <string.h>

#define AUDIO_ARG(v) ((uint64_t)(uintptr_t)(v))
#define AUDIO_ARGS(...) ((const uint64_t[8]){__VA_ARGS__})
#define AUDIO_UPDATE "_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm"
#define AUDIO_RENDER "_ZN2nn5audio6server17AudioRenderSystem29ExecuteAudioRendererRenderingEv"
#define AUDIO_CREATE_EVENT "_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb"
#define AUDIO_DESTROY_EVENT "_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE"

enum {
    AUDIO_FAIL = 0x2a2, AUDIO_INVALID_MODE = 0x40299,
    PARAM_DEVICE = 0x2c, PARAM_MODE = 0x30, PARAM_REVISION = 0x34, PARAM_SIZE = 0x38,
    RENDERER_COUNT = 4, QUEUE_DEPTH = 4, FRAME_SAMPLES = 240, CHANNELS_MAX = 6,
    WORKER_STACK = 0x10000, THREAD_EVENT = 0x200, SDK_EVENT_SIZE = 0x30,
    CLEAR_MANUAL = 0, CLEAR_AUTO = 1,
};

static uint32_t read_u32(const void *p) { uint32_t v; memcpy(&v, p, sizeof(v)); return v; }
static uint16_t read_u16(const void *p) { uint16_t v; memcpy(&v, p, sizeof(v)); return v; }
static uintptr_t read_ptr(const void *p) { uintptr_t v; memcpy(&v, p, sizeof(v)); return v; }
static void write_u32(void *p, uint32_t v) { memcpy(p, &v, sizeof(v)); }
static size_t align_up(size_t n, size_t alignment) { return (n + alignment - 1) & ~(alignment - 1); }

// Same service representation as dm/host/hle_audren.c, not the SDK's larger
// local CPU command representation. Keep the sound heap's DSP budget intact.
size_t dm_audio_service_work_size(const void *parameter, uint32_t revision) {
    if ((revision & 0xffffffu) != 0x564552u || revision < 0x31564552u || revision > 0x3f564552u) return 0;
    const unsigned char *p = parameter;
    unsigned r = (revision >> 24) - 0x30u;
    size_t n[11];
    for (size_t i = 0; i < ARRAYSIZE(n); ++i) {
        if (i == 8) { n[i] = p[0x20]; continue; } // voice-drop is a byte; adjacent padding is unspecified
        uint32_t v = read_u32(p + i * 4);
        if (v > 0x100000u) return 0;
        n[i] = v;
    }
    size_t mix = n[2], sub = n[3], voices = n[4], sinks = n[5], effects = n[6], perf = n[7];
    size_t splitters = n[9], dest = n[10], mixes = sub + 1;
    size_t size = align_up(((sinks + sub) * 0x3c0 + n[1] * 4) * (mix + 6), 64);
    size += align_up(mix * 4, 64) + sub * 0x400 + mixes * 0x940 + align_up(mixes * 8, 16);
    size += voices * 0x1d0 + align_up(voices * (r >= 15 ? 0x238u : 0x220u), 16) + align_up(voices * 8, 16);
    if (r >= 2) {
        size_t graph = mixes * mixes * 4 + mixes * 12 + 2 * align_up(mixes, 64) / 8 + align_up(mixes * mixes, 64) / 8;
        size += align_up(graph, 16);
        size += splitters * 0x20 + align_up(dest * (r >= 12 ? (r >= 15 ? 0x130u : 8u) : 0xe0u), 16);
        if (r < 5) size += align_up(dest * 4, 16);
    }
    size += (effects + voices * 4) * 0x20 + 0x50;
    if (r >= 12 && splitters && dest) size += align_up(dest * (r >= 14 ? 0xe0u : 0x80u), 64);
    size = align_up(size + (r >= 9 ? effects * 0x80 : 0), 64);
    size += (sinks + sub) * 0x280 + effects * (r >= 9 ? 0x530u : 0x4b0u) + sinks * 0x170 + voices * 0x100 + 0x40;
    if (perf) {
        size_t frame = (sub + voices + sinks + effects + 1) * (r >= 5 ? 0x18u : 0x10u) + (r >= 5 ? 0x990u : 0x658u);
        size += align_up(frame * (perf + 1), 64) + 0xc0;
    }
    size_t commands = 0x18000;
    if (r >= 5) commands = effects * 0x540 + sub * 0x3f24 + sinks * 0x130 + dest * 0x360
        + voices * (splitters * 0x50 + (r >= 8 ? 0x284u : 0x250u)) + (voices + sub + effects + sinks + 0x65) * 0x28 + 0x2c4;
    return align_up(size + commands + 0x7e, DM_PAGE);
}

static uintptr_t sdk_symbol(const char *name) {
    uintptr_t p = dm_guest_sdk_symbol(name);
    if (!p) dm_fail("audren: unsupported SDK, missing %s", name);
    return p;
}

static struct dm_guest_result sdk_call(uintptr_t entry, const uint64_t args[8]) {
    struct dm_guest_result result;
    if (!dm_guest_call(entry, args, &result)) dm_fail("audren: cannot enter SDK at %p", (void *)entry);
    return result;
}

static struct {
    uintptr_t work_size, open, open_event, close, exec, update;
    uintptr_t generate, construct, time_max, setup, process, lock, unlock;
    uintptr_t create_thread, start_thread, wait_thread, destroy_thread;
    uintptr_t create_event, destroy_event, get_readable, attach_readable;
    size_t mutex_offset;
} sdk;
static INIT_ONCE audio_once = INIT_ONCE_STATIC_INIT;

// AArch64 image PLT stub: adrp x16, page; ldr x17, [x16, #offset]; ... ; br x17.
static uintptr_t plt_target(uintptr_t pc) {
    if (!dm_guest_range((const void *)pc, 8, 0)) return 0;
    uint32_t a = read_u32((const void *)pc), l = read_u32((const void *)(pc + 4));
    if ((a & 0x9f00001fu) != 0x90000010u || (l & 0xffc003ffu) != 0xf9400211u) return pc;
    uint32_t imm = (((a >> 5) & 0x7ffffu) << 2) | ((a >> 29) & 3u);
    int64_t delta = ((int64_t)imm - ((imm & 0x100000u) ? 0x200000 : 0)) * 4096;
    uintptr_t slot = (pc & ~(uintptr_t)0xfff) + (uintptr_t)delta + ((l >> 10) & 0xfffu) * 8;
    return dm_guest_range((const void *)slot, sizeof(uintptr_t), 0) ? read_ptr((const void *)slot) : 0;
}

static size_t render_system_offset(uintptr_t method) {
    if (!method || !dm_guest_range((const void *)method, 8, 0)) return 0;
    uint32_t add = read_u32((const void *)method);
    uint32_t branch = read_u32((const void *)(method + 4));
    // DFC service objects can embed AudioRendererImpl at an extra offset.
    // Read the actual virtual method's `add x0, x0, #imm; b render_system`.
    if ((add & 0xffc003ffu) != 0x91000000u || (branch & 0xfc000000u) != 0x14000000u) return 0;
    uintptr_t target = plt_target(method + 4 + (uintptr_t)((int32_t)(branch << 6) / 16));
    return target && target == dm_m3_import(AUDIO_RENDER) ? (add >> 10) & 0xfffu : 0;
}

static size_t read_update_lock(uintptr_t entry, uintptr_t lock) {
    if (!dm_guest_range((const void *)entry, 24 * 4, 0)) return 0;
    // Trace the first call's x0 through the `this` copy and add in UpdateImpl.
    int field[32] = {0};
    for (size_t i = 1; i < ARRAYSIZE(field); ++i) field[i] = -1;
    for (size_t i = 0; i < 24; ++i) {
        uintptr_t pc = entry + i * 4;
        uint32_t w = read_u32((const void *)pc);
        unsigned rd = w & 31u, rn = (w >> 5) & 31u, rm = (w >> 16) & 31u;
        if ((w & 0xffe0ffe0u) == 0xaa0003e0u && rd != 31) field[rd] = field[rm];
        if ((w & 0xffc00000u) == 0x91000000u && rd != 31)
            field[rd] = field[rn] == 0 ? (int)((w >> 10) & 0xfffu) : -1;
        if ((w & 0xfc000000u) == 0x94000000u) {
            int32_t delta = (int32_t)(w << 6) / 16;
            return field[0] > 0 && plt_target(pc + (uintptr_t)delta) == lock ? (size_t)field[0] : 0;
        }
    }
    return 0;
}

static BOOL CALLBACK init_audio(PINIT_ONCE once, PVOID arg, PVOID *context) {
    (void)once; (void)arg; (void)context;
    sdk.work_size = sdk_symbol("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE");
    sdk.open = sdk_symbol("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleERKNS0_22AudioRendererParameterEPvm");
    sdk.open_event = sdk_symbol("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm");
    sdk.close = sdk_symbol("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE");
    sdk.exec = sdk_symbol("_ZN2nn5audio29ExecuteAudioRendererRenderingENS0_19AudioRendererHandleE");
    sdk.update = sdk_symbol(AUDIO_UPDATE);
    sdk.generate = sdk_symbol("_ZN2nn5audio6server17AudioRenderSystem15GenerateCommandENS_4SpanISt4byteEE");
    sdk.construct = sdk_symbol("_ZN2nn5audio3dsp20CommandListProcessorC1Ev");
    sdk.time_max = sdk_symbol("_ZN2nn5audio3dsp20CommandListProcessor17SetProcessTimeMaxEj");
    sdk.setup = sdk_symbol("_ZN2nn5audio3dsp20CommandListProcessor5SetupEPKvm");
    sdk.process = sdk_symbol("_ZN2nn5audio3dsp20CommandListProcessor7ProcessEv");
    sdk.lock = sdk_symbol("_ZN2nn2os12SdkMutexType4LockEv");
    sdk.unlock = sdk_symbol("_ZN2nn2os12SdkMutexType6UnlockEv");
    sdk.create_thread = sdk_symbol("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi");
    sdk.start_thread = sdk_symbol("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE");
    sdk.wait_thread = sdk_symbol("_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE");
    sdk.destroy_thread = sdk_symbol("_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE");
    sdk.create_event = sdk_symbol(AUDIO_CREATE_EVENT);
    sdk.destroy_event = sdk_symbol(AUDIO_DESTROY_EVENT);
    sdk.get_readable = sdk_symbol("_ZN2nn2os30GetReadableHandleOfSystemEventEPKNS0_15SystemEventTypeE");
    sdk.attach_readable = sdk_symbol("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE");
    sdk.mutex_offset = read_update_lock(sdk_symbol("_ZN2nn5audio6server17AudioRenderSystem10UpdateImplEPvmS3_mPKvm"), sdk.lock);
    if (!sdk.mutex_offset) dm_fail("audren: unsupported SDK UpdateImpl mutex");
    printf("[audren] SDK Span renderer, update mutex +%#zx; silent sink (no playback)\n", sdk.mutex_offset);
    fflush(stdout);
    return TRUE;
}

static void ensure_audio(void) {
    if (!InitOnceExecuteOnce(&audio_once, init_audio, NULL, NULL)) dm_fail("audren: SDK setup failed");
}

static int manual_parameter(unsigned char p[PARAM_SIZE], const void *original, int *substituted) {
    if (!dm_guest_range(original, PARAM_SIZE, 0)) dm_fail("audren: invalid renderer parameter %p", original);
    memcpy(p, original, PARAM_SIZE);
    uint32_t device = read_u32(p + PARAM_DEVICE), mode = read_u32(p + PARAM_MODE);
    *substituted = device == 0 && mode == 0;
    if (!*substituted && !(device == 1 && mode == 1)) {
        fprintf(stderr, "[RED] audren: unsupported renderer device %u / execution mode %u\n", device, mode);
        return 0;
    }
    write_u32(p + PARAM_DEVICE, 1);
    write_u32(p + PARAM_MODE, 1);
    return 1;
}

static size_t guest_work_size(const unsigned char *p, size_t backend, int substituted) {
    size_t service = substituted ? dm_audio_service_work_size(p, read_u32(p + PARAM_REVISION)) : 0;
    return service ? service : backend;
}

size_t audio_work_size(const void *param) {
    unsigned char p[PARAM_SIZE];
    int substituted;
    if (!manual_parameter(p, param, &substituted)) dm_fail("audren: cannot size unsupported renderer mode");
    ensure_audio();
    size_t backend = sdk_call(sdk.work_size, AUDIO_ARGS(AUDIO_ARG(p))).x0;
    size_t guest = guest_work_size(p, backend, substituted);
    printf("[audren] work buffer guest=%#zx SDK CPU/manual=%#zx revision=%#x\n", guest, backend, read_u32(p + PARAM_REVISION));
    fflush(stdout);
    return guest;
}

struct audio_frame {
    uint32_t frames, channels;
    int16_t pcm[FRAME_SAMPLES * CHANNELS_MAX];
};

struct renderer {
    uintptr_t obj, state, system;
    unsigned char *storage;
    size_t storage_size;
    int reserved, driven, private_event, closing;
    unsigned sample_rate, sample_count;
    SRWLOCK lock;
    CONDITION_VARIABLE space;
    HANDLE stop, timer;
    struct audio_frame queue[QUEUE_DEPTH];
    unsigned first, queued;
    uint64_t renders, updates, last_update_render, catchups;
    uint64_t sink_frames, sink_samples, nonsilent;
};
static struct renderer renderers[RENDERER_COUNT];
static SRWLOCK registry_lock = SRWLOCK_INIT;
static __declspec(thread) int opening;
static __declspec(thread) struct renderer *worker_renderer;
extern void m3_audio_worker(void *arg);

static struct renderer *reserve_renderer(void) {
    struct renderer *r = NULL;
    AcquireSRWLockExclusive(&registry_lock);
    for (size_t i = 0; i < ARRAYSIZE(renderers); ++i) {
        if (!renderers[i].reserved) {
            r = &renderers[i];
            memset(r, 0, sizeof(*r));
            r->reserved = 1;
            InitializeSRWLock(&r->lock);
            InitializeConditionVariable(&r->space);
            break;
        }
    }
    ReleaseSRWLockExclusive(&registry_lock);
    return r;
}

static struct renderer *find_renderer(uintptr_t key, int by_system) {
    struct renderer *r = NULL;
    AcquireSRWLockShared(&registry_lock);
    for (size_t i = 0; i < ARRAYSIZE(renderers); ++i) {
        struct renderer *candidate = &renderers[i];
        if (candidate->obj && key == (by_system ? candidate->system : candidate->obj)) { r = candidate; break; }
    }
    ReleaseSRWLockShared(&registry_lock);
    return r;
}

static void release_renderer(struct renderer *r) {
    if (r->timer) CloseHandle(r->timer);
    if (r->stop) CloseHandle(r->stop);
    if (r->storage) dm_service_free(r->storage, r->storage_size);
    AcquireSRWLockExclusive(&registry_lock);
    r->obj = 0;
    r->reserved = 0;
    ReleaseSRWLockExclusive(&registry_lock);
}

static uint32_t sdk_open(void *out, void *event, const void *p, void *work, size_t bytes) {
    if (event) return (uint32_t)sdk_call(sdk.open_event, AUDIO_ARGS(AUDIO_ARG(out), AUDIO_ARG(event), AUDIO_ARG(p), AUDIO_ARG(work), bytes)).x0;
    return (uint32_t)sdk_call(sdk.open, AUDIO_ARGS(AUDIO_ARG(out), AUDIO_ARG(p), AUDIO_ARG(work), bytes)).x0;
}

static uint32_t open_renderer(void *out, void *event, const void *param, void *work, size_t bytes) {
    ensure_audio();
    // The event overload opens through the no-event overload's PLT slot.
    if (opening) return sdk_open(out, event, param, work, bytes);
    unsigned char p[PARAM_SIZE];
    int driven;
    if (!manual_parameter(p, param, &driven)) return AUDIO_FAIL;
    uint32_t rate = read_u32(p), frames = read_u32(p + 4);
    if ((rate != 48000 && rate != 32000) || frames != rate / 200) {
        fprintf(stderr, "[RED] audren: unsupported frame format %u Hz, %u samples (expected 5 ms at 32/48 kHz)\n", rate, frames);
        return AUDIO_FAIL;
    }
    if (!dm_guest_range(out, 2 * sizeof(uintptr_t), 1) || (event && !dm_guest_range(event, SDK_EVENT_SIZE, 1)))
        dm_fail("audren: invalid renderer handle or event address");
    size_t backend = sdk_call(sdk.work_size, AUDIO_ARGS(AUDIO_ARG(p))).x0;
    size_t guest = guest_work_size(p, backend, driven);
    if (!backend || !work || bytes < guest || ((AUDIO_ARG(work) | bytes | backend) & (DM_PAGE - 1)) || !dm_guest_range(work, bytes, 1)) {
        fprintf(stderr, "[RED] audren: invalid work buffer %p+%#zx (guest requires %#zx, SDK requires %#zx)\n", work, bytes, guest, backend);
        return AUDIO_FAIL;
    }
    struct renderer *r = reserve_renderer();
    if (!r) { fprintf(stderr, "[RED] audren: too many open renderers\n"); return AUDIO_FAIL; }
    r->driven = driven;
    r->sample_rate = rate;
    r->sample_count = frames;
    size_t private_bytes = driven ? DM_PAGE + WORKER_STACK : 0;
    size_t extra = driven && bytes < backend ? backend : 0;
    if (extra > SIZE_MAX - private_bytes) dm_fail("audren: work buffer size overflow");
    r->storage_size = private_bytes + extra;
    if (r->storage_size) {
        r->storage = dm_service_alloc(r->storage_size);
        if (!r->storage) { release_renderer(r); fprintf(stderr, "[RED] audren: no guest backing for CPU renderer\n"); return AUDIO_FAIL; }
    }
    if (extra) { work = r->storage + private_bytes; bytes = backend; }
    if (driven) {
        r->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
        r->timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!r->stop || !r->timer) {
            fprintf(stderr, "[RED] audren: cannot create silent sink clock (%lu)\n", GetLastError());
            release_renderer(r);
            return AUDIO_FAIL;
        }
    }

    unsigned char *owned_event = driven && event ? r->storage + THREAD_EVENT : NULL;
    opening = 1;
    uint32_t result = sdk_open(out, owned_event ? owned_event : event, p, work, bytes);
    opening = 0;
    if (result) { release_renderer(r); fprintf(stderr, "[RED] audren: SDK OpenAudioRenderer returned %#x\n", result); return result; }

    uintptr_t obj = read_ptr(out), state = read_ptr((const unsigned char *)out + sizeof(uintptr_t));
    uintptr_t vtable = obj && dm_guest_range((const void *)obj, sizeof(uintptr_t), 0) ? read_ptr((const void *)obj) : 0;
    uintptr_t method = vtable && dm_guest_range((const void *)(vtable + 0x78), sizeof(uintptr_t), 0)
        ? read_ptr((const void *)(vtable + 0x78)) : 0;
    size_t system_offset = render_system_offset(method);
    if (!system_offset || !dm_guest_range((const void *)(obj + system_offset), sdk.mutex_offset + 8, 1))
        dm_fail("audren: unsupported DFC renderer object=%p vtable=%p method=%p", (void *)obj, (void *)vtable, (void *)method);
    int attached_event = 0;
    if (owned_event) {
        // The game owns only a readable copy, and can move/destroy its event
        // without changing the object that the SDK's manual execute signals.
        sdk_call(sdk.destroy_event, AUDIO_ARGS(AUDIO_ARG(owned_event)));
        result = (uint32_t)sdk_call(sdk.create_event, AUDIO_ARGS(AUDIO_ARG(owned_event), CLEAR_AUTO, 1)).x0;
        if (result) goto failed_open;
        r->private_event = 1;
        uint32_t readable = (uint32_t)sdk_call(sdk.get_readable, AUDIO_ARGS(AUDIO_ARG(owned_event))).x0;
        uint32_t copy = dm_dup_readable_event(readable);
        if (!copy) { result = AUDIO_FAIL; goto failed_open; }
        sdk_call(sdk.attach_readable, AUDIO_ARGS(AUDIO_ARG(event), copy, 1, CLEAR_AUTO));
        attached_event = 1;
    }
    if (driven) {
        result = (uint32_t)sdk_call(sdk.create_thread, AUDIO_ARGS(AUDIO_ARG(r->storage), AUDIO_ARG(m3_audio_worker),
                                      AUDIO_ARG(r), AUDIO_ARG(r->storage + DM_PAGE), WORKER_STACK, 9)).x0;
        if (result) goto failed_open;
    }
    AcquireSRWLockExclusive(&registry_lock);
    r->obj = obj;
    r->state = state;
    r->system = obj + system_offset;
    ReleaseSRWLockExclusive(&registry_lock);
    printf("[audren] opened SDK CPU/manual renderer %p (system +%#zx), %u Hz x %u, host work=%#zx%s\n",
           (void *)obj, system_offset, rate, frames, extra, driven ? "; 5 ms silent sink" : "; game paced");
    fflush(stdout);
    if (driven) sdk_call(sdk.start_thread, AUDIO_ARGS(AUDIO_ARG(r->storage)));
    return 0;

failed_open:
    sdk_call(sdk.close, AUDIO_ARGS(obj, state));
    if (attached_event) sdk_call(sdk.destroy_event, AUDIO_ARGS(AUDIO_ARG(event)));
    if (r->private_event) sdk_call(sdk.destroy_event, AUDIO_ARGS(AUDIO_ARG(owned_event)));
    memset(out, 0, 2 * sizeof(uintptr_t));
    release_renderer(r);
    fprintf(stderr, "[RED] audren: cannot initialize renderer event or guest worker (%#x)\n", result);
    return result;
}

uint32_t audio_open(void *out, const void *p, void *work, size_t bytes) {
    return open_renderer(out, NULL, p, work, bytes);
}

uint32_t audio_open_event(void *out, void *event, const void *p, void *work, size_t bytes) {
    if (!event) dm_fail("audren: null event for event overload");
    return open_renderer(out, event, p, work, bytes);
}

void audio_close(uintptr_t obj, uintptr_t state) {
    struct renderer *r = find_renderer(obj, 0);
    if (!r) dm_fail("audren: CloseAudioRenderer of unknown handle %p", (void *)obj);
    AcquireSRWLockExclusive(&r->lock);
    r->closing = 1;
    WakeAllConditionVariable(&r->space);
    ReleaseSRWLockExclusive(&r->lock);
    if (r->driven) {
        if (!SetEvent(r->stop)) dm_fail("audren: cannot stop renderer clock (%lu)", GetLastError());
        sdk_call(sdk.wait_thread, AUDIO_ARGS(AUDIO_ARG(r->storage)));
        sdk_call(sdk.destroy_thread, AUDIO_ARGS(AUDIO_ARG(r->storage)));
    }
    sdk_call(sdk.close, AUDIO_ARGS(obj, state));
    if (r->private_event) sdk_call(sdk.destroy_event, AUDIO_ARGS(AUDIO_ARG(r->storage + THREAD_EVENT)));
    printf("[audren] closed renderer %p: renders=%llu updates=%llu catch-ups=%llu sink blocks=%llu nonzero=%llu scalar samples=%llu\n",
           (void *)obj, (unsigned long long)r->renders, (unsigned long long)r->updates, (unsigned long long)r->catchups,
           (unsigned long long)r->sink_frames, (unsigned long long)r->nonsilent, (unsigned long long)r->sink_samples);
    fflush(stdout);
    release_renderer(r);
}

// Validate the entire SDK command span before letting either the processor or
// the sink read it. SDK 19/20 commands have a magic; SDK 21+ dropped it.
static const unsigned char *device_sink(const unsigned char *list, size_t len, int *compact) {
    enum { LIST_HEADER = 0x20, SINK_ID = 0x14 };
    const uint32_t magic = 0xcafebabeu;
    uint32_t count = read_u32(list + 4);
    size_t offset = LIST_HEADER;
    *compact = count && len - offset >= 4 && read_u32(list + offset) != magic;
    size_t min_header = *compact ? 0x10u : 8u, sink_size = *compact ? 0x140u : 0x148u;
    const unsigned char *sink = NULL;
    for (uint32_t i = 0; i < count; ++i) {
        if (len - offset < min_header) dm_fail("audren: truncated SDK command %u/%u", i, count);
        const unsigned char *c = list + offset;
        uint16_t size = read_u16(c + (*compact ? 2 : 6));
        if ((!*compact && read_u32(c) != magic) || size < min_header || size > 0x7fffu || size > len - offset)
            dm_fail("audren: unsupported/malformed SDK command %u/%u, size %#x", i, count, (unsigned)size);
        if (c[*compact ? 1 : 5] == SINK_ID && (c[*compact ? 0 : 4] & 1)) {
            if (size != sink_size || sink) dm_fail("audren: unsupported SDK DeviceSink layout/count");
            sink = c;
        }
        offset += size;
    }
    return sink;
}

static int16_t clamp_sample(int32_t s) {
    return (int16_t)(s > INT16_MAX ? INT16_MAX : s < INT16_MIN ? INT16_MIN : s);
}

static void capture_pcm(struct renderer *r, const unsigned char *list, const unsigned char *sink,
                        int compact, struct audio_frame *frame) {
    frame->frames = r->sample_count;
    frame->channels = 0; // a stopped renderer or a graph without a device sink has no device data
    if (!sink) return;
    unsigned channels = read_u32(sink + (compact ? 0x128 : 0x130));
    unsigned mix_count = read_u32(list + 0x10), samples = read_u32(list + 0x14), rate = read_u32(list + 0x18);
    const int32_t *mix = (const int32_t *)read_ptr(list + 8);
    if (!channels || channels > CHANNELS_MAX || samples != r->sample_count || rate != r->sample_rate || !mix || ((uintptr_t)mix & 3))
        dm_fail("audren: unsupported DeviceSink PCM format (%u channels, %u Hz x %u)", channels, rate, samples);
    const int32_t *src[CHANNELS_MAX];
    for (unsigned ch = 0; ch < channels; ++ch) {
        uint16_t index = read_u16(sink + (compact ? 0x12c : 0x134) + ch * 2);
        if (index >= mix_count) dm_fail("audren: invalid DeviceSink mix index %u / %u", (unsigned)index, mix_count);
        const int32_t *channel = mix + (size_t)index * samples;
        if (!dm_guest_range(channel, (size_t)samples * sizeof(*channel), 0)) dm_fail("audren: inaccessible DeviceSink mix buffer");
        src[ch] = channel;
    }
    for (unsigned i = 0; i < samples; ++i)
        for (unsigned ch = 0; ch < channels; ++ch)
            frame->pcm[(size_t)i * channels + ch] = clamp_sample(src[ch][i]);
    frame->channels = channels;
}

// Used for the worker's scheduled frames and update catch-up frames. Caller
// holds the renderer lock; SDK command generation also needs the SDK lock.
static uint32_t render_once(struct renderer *r, struct audio_frame *frame) {
    const unsigned char *sys = (const unsigned char *)r->system;
    if (read_u32(sys + 0x48) != 1 || read_u32(sys + 0x4c) != 1) return AUDIO_INVALID_MODE;
    uintptr_t mutex = r->system + sdk.mutex_offset;
    sdk_call(sdk.lock, AUDIO_ARGS(mutex));
    uintptr_t buf = read_ptr(sys + 0xf8);
    size_t capacity = read_ptr(sys + 0x100);
    struct dm_guest_result span = sdk_call(sdk.generate, AUDIO_ARGS(r->system, buf, capacity));
    sdk_call(sdk.unlock, AUDIO_ARGS(mutex));
    const unsigned char *list = (const unsigned char *)(uintptr_t)span.x0;
    size_t len = span.x1;
    if (len < 0x20 || span.x0 < buf || len > capacity || span.x0 - buf > capacity - len || !dm_guest_range(list, len, 0))
        dm_fail("audren: invalid SDK command span %p+%#zx in %p+%#zx", (const void *)list, len, (void *)buf, capacity);
    int compact;
    const unsigned char *sink = device_sink(list, len, &compact);
    _Alignas(16) uint64_t processor[16] = {0};
    sdk_call(sdk.construct, AUDIO_ARGS(AUDIO_ARG(processor)));
    sdk_call(sdk.time_max, AUDIO_ARGS(AUDIO_ARG(processor), UINT32_MAX));
    sdk_call(sdk.setup, AUDIO_ARGS(AUDIO_ARG(processor), AUDIO_ARG(list), len));
    sdk_call(sdk.process, AUDIO_ARGS(AUDIO_ARG(processor)));
    capture_pcm(r, list, sink, compact, frame);
    ++r->renders;
    return 0;
}

static void consume_pcm(struct renderer *r, const struct audio_frame *frame) {
    // Deliberately consume at the native sample rate and channel count. A
    // silent sink does not convert PCM for a device it never opens.
    size_t samples = (size_t)frame->frames * frame->channels;
    unsigned any = 0;
    for (size_t i = 0; i < samples; ++i) any |= (uint16_t)frame->pcm[i];
    r->sink_samples += samples;
    ++r->sink_frames;
    if (any) ++r->nonsilent;
    if (r->sink_frames == 1 || (r->sink_samples == samples && samples) || (any && r->nonsilent == 1) || r->sink_frames % 2000 == 0) {
        printf("[audren] silent sink %p: blocks=%llu scalar samples=%llu nonzero blocks=%llu updates=%llu catch-ups=%llu\n",
               (void *)r->obj, (unsigned long long)r->sink_frames, (unsigned long long)r->sink_samples,
               (unsigned long long)r->nonsilent, (unsigned long long)r->updates, (unsigned long long)r->catchups);
        fflush(stdout);
    }
}

uint32_t audio_render(uintptr_t system) {
    if (worker_renderer) {
        if (worker_renderer->system != system) dm_fail("audren: guest worker entered the wrong renderer");
        return render_once(worker_renderer, &worker_renderer->queue[worker_renderer->first]);
    }
    struct renderer *r = find_renderer(system, 1);
    if (!r) dm_fail("audren: ExecuteAudioRendererRendering of unknown system %p", (void *)system);
    if (r->driven) return AUDIO_INVALID_MODE; // the original DSP renderer is not externally executable
    AcquireSRWLockExclusive(&r->lock);
    uint32_t result = render_once(r, &r->queue[0]);
    if (!result) consume_pcm(r, &r->queue[0]);
    ReleaseSRWLockExclusive(&r->lock);
    return result;
}

uint32_t audio_update(uintptr_t sys, void *out, size_t out_n, void *perf, size_t perf_n, const void *in, size_t in_n) {
    ensure_audio();
    const uint64_t args[8] = {sys, AUDIO_ARG(out), out_n, AUDIO_ARG(perf), perf_n, AUDIO_ARG(in), in_n};
    struct renderer *r = find_renderer(sys, 1);
    if (!r) return (uint32_t)sdk_call(sdk.update, args).x0;
    AcquireSRWLockExclusive(&r->lock);
    // The next update can replace a just-initialized effect's state before
    // the DSP sees it. Process one real frame first, then consume it at the
    // normal sink deadline without generating an extra renderer event.
    if (r->driven && r->updates && r->last_update_render == r->renders) {
        while (!r->closing && r->queued == QUEUE_DEPTH) {
            if (!SleepConditionVariableSRW(&r->space, &r->lock, INFINITE, 0))
                dm_fail("audren: silent sink queue wait failed (%lu)", GetLastError());
        }
        if (!r->closing && r->last_update_render == r->renders) {
            unsigned at = (r->first + r->queued) % QUEUE_DEPTH;
            uint32_t result = render_once(r, &r->queue[at]);
            if (result) dm_fail("audren: SDK catch-up frame failed %#x", result);
            ++r->queued;
            ++r->catchups;
        }
    }
    uint32_t result = (uint32_t)sdk_call(sdk.update, args).x0;
    if (!result) {
        r->last_update_render = r->renders;
        if (++r->updates == 1) {
            printf("[audren] SDK update accepted for %p (%zu input bytes, %zu output bytes)\n", (void *)r->obj, in_n, out_n);
            fflush(stdout);
        }
    }
    ReleaseSRWLockExclusive(&r->lock);
    return result;
}

static int64_t audio_now(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

void audio_worker(void *arg) {
    struct renderer *r = arg;
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart < 2000) dm_fail("audren: no QPC clock");
    int64_t period = frequency.QuadPart / 200, min_spacing = 9 * frequency.QuadPart / 2000;
    HANDLE wake[] = {r->stop, r->timer};
    int64_t due = audio_now();
    for (;;) {
        int64_t now = audio_now();
        if (due > now) {
            LARGE_INTEGER relative;
            relative.QuadPart = -((due - now) * 10000000 + frequency.QuadPart - 1) / frequency.QuadPart;
            if (!SetWaitableTimer(r->timer, &relative, 0, NULL, NULL, FALSE))
                dm_fail("audren: cannot arm silent sink clock (%lu)", GetLastError());
            DWORD w = WaitForMultipleObjects(ARRAYSIZE(wake), wake, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0) break;
            if (w != WAIT_OBJECT_0 + 1) dm_fail("audren: silent sink clock wait failed (%lu)", GetLastError());
        }
        AcquireSRWLockExclusive(&r->lock);
        if (r->closing) { ReleaseSRWLockExclusive(&r->lock); break; }
        now = audio_now();
        if (!r->queued) {
            // SDK public execute runs through the intercepted system method
            // and signals the SDK's event after processing returns.
            uint64_t previous = r->renders;
            worker_renderer = r;
            uint32_t result = (uint32_t)sdk_call(sdk.exec, AUDIO_ARGS(r->obj, r->state)).x0;
            worker_renderer = NULL;
            if (result || r->renders == previous) dm_fail("audren: SDK manual execute failed or bypassed capture (%#x)", result);
            consume_pcm(r, &r->queue[r->first]);
        } else {
            consume_pcm(r, &r->queue[r->first]);
            r->first = (r->first + 1) % QUEUE_DEPTH;
            --r->queued;
            WakeConditionVariable(&r->space);
        }
        ReleaseSRWLockExclusive(&r->lock);
        due += period;
        if (due < now + min_spacing) due = now + period;
    }
}

uintptr_t audio_upsampler_allocate(void *manager) {
    // The SDK has no upsampler manager in CPU mode. Return the SDK's normal
    // unavailable result; the silent sink consumes the unconverted 32 kHz.
    if (!manager) return 0;
    return (uintptr_t)sdk_call(sdk_symbol("_ZN2nn5audio6server16UpsamplerManager8AllocateEv"), AUDIO_ARGS(AUDIO_ARG(manager))).x0;
}

// A stable logical device for the renderer, not an open playback route.
static const char device_name[0x100] = "AudioBuiltInSpeakerOutput";
static SRWLOCK device_lock = SRWLOCK_INIT;
static float device_volume = 1.0f;
static int auto_tune;
static void *device_notification;
static int notification_changing;

void audio_active_device(char *name) {
    if (!dm_guest_range(name, sizeof(device_name), 1)) dm_fail("audren: invalid output device name buffer");
    memcpy(name, device_name, sizeof(device_name));
}
int audio_list_devices(char *names, int capacity) {
    if (!names || capacity <= 0) return 0;
    audio_active_device(names);
    return 1;
}
int audio_device_channels(void) { return 2; }

static void check_device(const char *name) {
    if (!dm_guest_range(name, sizeof(device_name), 0)) dm_fail("audren: invalid logical output device %p", name);
    // The default route is also addressed by an empty AudioDeviceName.
    if (*name && memcmp(name, device_name, strlen(device_name) + 1)) dm_fail("audren: unknown logical output device '%.255s'", name);
}
void audio_set_device_volume(const char *name, float volume) {
    check_device(name);
    AcquireSRWLockExclusive(&device_lock);
    device_volume = volume;
    ReleaseSRWLockExclusive(&device_lock);
}
float audio_get_device_volume(const char *name) {
    check_device(name);
    AcquireSRWLockShared(&device_lock);
    float v = device_volume;
    ReleaseSRWLockShared(&device_lock);
    return v;
}
void audio_set_auto_tune(unsigned enabled) {
    AcquireSRWLockExclusive(&device_lock);
    auto_tune = (int)(enabled & 1u);
    ReleaseSRWLockExclusive(&device_lock);
}
int audio_get_auto_tune(void) {
    AcquireSRWLockShared(&device_lock);
    int enabled = auto_tune;
    ReleaseSRWLockShared(&device_lock);
    return enabled;
}
uint32_t audio_acquire_device_notification(void *event) {
    if (!dm_guest_range(event, SDK_EVENT_SIZE, 1)) dm_fail("audren: invalid output notification event");
    AcquireSRWLockExclusive(&device_lock);
    if (device_notification || notification_changing) { ReleaseSRWLockExclusive(&device_lock); return 0x499; }
    notification_changing = 1;
    ReleaseSRWLockExclusive(&device_lock);
    uint32_t result = (uint32_t)sdk_call(sdk_symbol(AUDIO_CREATE_EVENT), AUDIO_ARGS(AUDIO_ARG(event), CLEAR_MANUAL, 1)).x0;
    // The silent route is fixed, so there is no device-change signal to emit.
    AcquireSRWLockExclusive(&device_lock);
    if (!result) device_notification = event;
    notification_changing = 0;
    ReleaseSRWLockExclusive(&device_lock);
    return result;
}
void audio_release_device_notification(void *event) {
    AcquireSRWLockExclusive(&device_lock);
    if (!device_notification) { ReleaseSRWLockExclusive(&device_lock); return; }
    if (device_notification != event || notification_changing) dm_fail("audren: invalid output notification release");
    notification_changing = 1;
    ReleaseSRWLockExclusive(&device_lock);
    sdk_call(sdk_symbol(AUDIO_DESTROY_EVENT), AUDIO_ARGS(AUDIO_ARG(event)));
    AcquireSRWLockExclusive(&device_lock);
    device_notification = NULL;
    notification_changing = 0;
    ReleaseSRWLockExclusive(&device_lock);
}
int audio_list_input_devices(void *names, int capacity) {
    // No capture route is open on the Windows host. Do not advertise an
    // input stream or touch any output slots when returning an empty list.
    (void)names; (void)capacity;
    return 0;
}
