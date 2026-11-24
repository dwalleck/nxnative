// Only a live NVN window is associated with native resources. Never store a
// host pointer in the guest's writable NVNwindow object.
#include "m4_overlay.h"

struct display_binding {
    struct display_binding *next;
    const struct headless_window *owner;
    const struct nvn_device *device;
    struct dm_display *display;
};
static SRWLOCK display_lock = SRWLOCK_INIT;
static struct display_binding *displays;

static int open_display(const struct headless_window *owner) {
    if (!GetEnvironmentVariableW(L"DM_WINDOW", NULL, 0)) return 0;
    const nvn_texdesc *desc = &owner->state.textures[0]->desc;
    struct display_binding *entry = calloc(1, sizeof(*entry));
    if (!entry) dm_fail("nvnWindowInitialize: cannot allocate display binding");
    VkResult r = dm_display_open(&entry->display, desc->width, desc->height);
    if (r != VK_SUCCESS) dm_fail("nvnWindowInitialize: Vulkan window creation returned %d", (int)r);
    entry->owner = owner;
    entry->device = owner->state.device;
    AcquireSRWLockExclusive(&display_lock);
    entry->next = displays;
    displays = entry;
    // A device with several logical windows does not identify the draw's
    // display. Retire any candidate made before the ownership changed.
    if (dm_display_overlay(entry->display)) {
        for (struct display_binding *e = entry->next; e; e = e->next) {
            struct dm_m4_overlay *capture = dm_display_overlay(e->display);
            if (e->device == entry->device && capture) dm_m4_overlay_discard(capture);
        }
    }
    ReleaseSRWLockExclusive(&display_lock);
    return 1;
}
static void close_display(const struct headless_window *owner) {
    AcquireSRWLockExclusive(&display_lock);
    struct display_binding **p = &displays;
    while (*p && (*p)->owner != owner) p = &(*p)->next;
    struct display_binding *entry = *p;
    if (entry) *p = entry->next;
    // A VM change must not miss imports owned by a display being destroyed.
    if (entry) { dm_display_close(entry->display); free(entry); }
    ReleaseSRWLockExclusive(&display_lock);
}

void dm_guest_gpu_unmap_begin(const void *base, size_t size) {
    // All replay sessions and scanout hold display_lock until their fence.
    // Keep it excluded through the actual change to the host pages, not only
    // through cache eviction; no draw may re-import the old backing.
    AcquireSRWLockExclusive(&display_lock);
    for (const struct display_binding *e = displays; e; e = e->next) {
        struct dm_m4_replay_session session;
        const struct dm_m4_replay_keys none = {0};
        if (!dm_display_borrow_draw(e->display, &none, &session)) {
            dm_m4_draw_forget_imports(session.draw, base, size);
            dm_display_return_draw(e->display);
        }
    }
}
void dm_guest_gpu_unmap_end(void) { ReleaseSRWLockExclusive(&display_lock); }

// Hold the registry read lock across replay so finalization cannot release the
// display binding. Its GPU lock also serializes submits with scanout and close.
static __declspec(thread) struct dm_display *borrowed_display;
static const struct display_binding *lock_display(const void *device) {
    AcquireSRWLockShared(&display_lock);
    for (struct display_binding *e = displays; e; e = e->next)
        if (e->device == device) return e;
    ReleaseSRWLockShared(&display_lock);
    return NULL;
}
// Called only for opted-in displays, with the registry locked.
static int ambiguous_display(const struct display_binding *binding) {
    for (const struct display_binding *e = displays; e; e = e->next)
        if (e != binding && e->device == binding->device) return 1;
    return 0;
}
const char *dm_m4_replay_begin(const void *device, const struct dm_m4_replay_keys *keys,
                               struct dm_m4_replay_session *out) {
    const struct display_binding *binding = lock_display(device);
    if (!binding) return "no Vulkan window for submitted NVN device";
    struct dm_display *d = binding->display;
    const char *error = dm_display_borrow_draw(d, keys, out);
    if (!error) {
        borrowed_display = d;
        struct dm_m4_overlay *capture = dm_display_overlay(d);
        if (capture && !ambiguous_display(binding)) out->overlay = capture;
    } else ReleaseSRWLockShared(&display_lock);
    return error;
}
const char *dm_m4_replay_begin_compute(const void *device, const uint8_t cs[8],
                                       struct dm_m4_compute_session *out) {
    const struct display_binding *binding = lock_display(device);
    if (!binding) return "no Vulkan window for dispatched NVN device";
    struct dm_display *d = binding->display;
    const char *error = dm_display_borrow_compute(d, cs, out);
    if (!error) borrowed_display = d;
    else ReleaseSRWLockShared(&display_lock);
    return error;
}
void dm_m4_replay_end(void) {
    dm_display_return_draw(borrowed_display);
    borrowed_display = NULL;
    ReleaseSRWLockShared(&display_lock);
}

struct scanout_source { nvn_texdesc desc; const uint8_t *bytes; size_t linear_pitch; NVNrectangle crop; };
static VkResult fill_scanout(void *arg, void *pixels, size_t pitch) {
    const struct scanout_source *s = arg;
    unsigned width = (unsigned)s->crop.width, height = (unsigned)s->crop.height;
    if (!s->linear_pitch && width == s->desc.width && height == s->desc.height)
        return nvn_deswizzle(&s->desc, 0, 0, s->bytes, pixels, pitch) ? VK_ERROR_FORMAT_NOT_SUPPORTED : VK_SUCCESS;
    uint32_t start = (unsigned)s->crop.x * 4, end = start + width * 4;
    unsigned th, td;
    nvn_tex_level_tiles(&s->desc, 0, &th, &td);
    uint32_t stride = nvn_tex_level_gobs(&s->desc, 0) * 64;
    for (unsigned i = 0; i < height; ++i) {
        unsigned y = (unsigned)s->crop.y + i;
        uint8_t *dst = (uint8_t *)pixels + i * pitch;
        if (s->linear_pitch) memcpy(dst, s->bytes + y * s->linear_pitch + start, end - start);
        else for (unsigned x = start, n; x < end; x += n) {
            n = min(end - x, 16u - (x & 15));
            uint64_t at = nvn_bl_offset(x, y, 0, stride, s->desc.height, th, td, s->desc.gob_order);
            memcpy(dst + (x - start), s->bytes + at, n);
        }
    }
    return VK_SUCCESS;
}

// 0=headless, 1=queued to WSI, 2=window temporarily unavailable.
static int present_display(const struct headless_window *owner, const struct headless_texture *texture,
                           const NVNrectangle *crop) {
    AcquireSRWLockExclusive(&display_lock);
    const struct display_binding *entry = displays;
    while (entry && entry->owner != owner) entry = entry->next;
    if (!entry) { ReleaseSRWLockExclusive(&display_lock); return 0; }
    struct scanout_source src = {.desc = texture->desc};
    nvn_texlayout layout;
    if (src.desc.target != NVN_TEXTURE_TARGET_2D || src.desc.samples > 1 || !texture_layout(&src.desc, &layout))
        dm_fail("nvnQueuePresentTexture: unsupported scanout layout");
    VkFormat format;
    switch (src.desc.format) {
    case NVN_FORMAT_RGBA8: format = VK_FORMAT_R8G8B8A8_UNORM; break;
    case NVN_FORMAT_RGBA8_SRGB: format = VK_FORMAT_R8G8B8A8_SRGB; break;
    case NVN_FORMAT_BGRA8: format = VK_FORMAT_B8G8R8A8_UNORM; break;
    case NVN_FORMAT_BGRA8_SRGB: format = VK_FORMAT_B8G8R8A8_SRGB; break;
    default: dm_fail("nvnQueuePresentTexture: unsupported scanout format %#x", src.desc.format);
    }
    src.bytes = pool_storage(texture->pool, texture->offset, (size_t)layout.size, layout.align, 0);
    if (!src.bytes) dm_fail("nvnQueuePresentTexture: invalid scanout storage");
    if (src.desc.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) {
        src.linear_pitch = (size_t)layout.layer_size / src.desc.height;
        if (src.linear_pitch < src.desc.width * (size_t)4) dm_fail("nvnQueuePresentTexture: invalid scanout pitch");
    }
    src.crop = crop->width ? *crop : (NVNrectangle){0, 0, (int)src.desc.width, (int)src.desc.height};
    if ((unsigned)src.crop.x > src.desc.width || (unsigned)src.crop.width > src.desc.width - (unsigned)src.crop.x ||
        (unsigned)src.crop.y > src.desc.height || (unsigned)src.crop.height > src.desc.height - (unsigned)src.crop.y)
        dm_fail("nvnQueuePresentTexture: crop exceeds scanout texture");
    struct dm_m4_overlay_frame frame;
    const int capture = dm_display_overlay(entry->display) != NULL;
    if (capture) frame = (struct dm_m4_overlay_frame){
        .guest_frame = owner->presents + 1, .storage = src.bytes,
        .x = (uint32_t)src.crop.x, .y = (uint32_t)src.crop.y, .ambiguous = ambiguous_display(entry)};
    VkResult r = dm_display_frame(entry->display, (uint32_t)src.crop.width, (uint32_t)src.crop.height,
                                  format, fill_scanout, &src, capture ? &frame : NULL);
    ReleaseSRWLockExclusive(&display_lock);
    if (r != VK_SUCCESS && r != VK_NOT_READY) dm_fail("nvnQueuePresentTexture: Vulkan present returned %d", (int)r);
    return r == VK_SUCCESS ? 1 : 2;
}
