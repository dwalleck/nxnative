// Synchronous handoff of guest-backed frames to the thread owning the HWND.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <stdlib.h>
#include "m4_display.h"
#include "m4_overlay.h"
#include "m4_replay_device.h"

struct dm_display {
    HANDLE thread, request, complete;
    uint32_t width, height;
    VkFormat format;
    dm_m4_fill_frame fill;
    void *source;
    VkResult result;
    SRWLOCK gpu_lock;
    struct dm_replay_device *replay;
    struct dm_m4_overlay *overlay;
    const struct dm_m4_overlay_frame *overlay_frame;
};

static void observe_frame(void *arg, const void *pixels, VkExtent2D source, VkExtent2D surface, int bgra) {
    const struct dm_display *d = arg;
    dm_m4_overlay_present(d->overlay, d->overlay_frame, pixels, source, surface, bgra);
}

static unsigned WINAPI display_loop(void *arg) {
    struct dm_display *d = arg;
    struct dm_m4_window *w = NULL;
    const struct dm_m4_device_requirements *requirements;
    d->result = dm_replay_config(&d->replay, &requirements);
    if (d->result != VK_SUCCESS) return (unsigned)d->result;
    d->result = dm_m4_window_create(&w, L"nxnative", d->width, d->height, requirements);
    if (d->result == VK_SUCCESS) d->result = dm_replay_start(d->replay, dm_m4_window_get_device(w));
    if (d->result != VK_SUCCESS) goto done;
    SetEvent(d->complete);
    for (;;) {
        DWORD wake = MsgWaitForMultipleObjectsEx(1, &d->request, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wake == WAIT_OBJECT_0) {
            if (!d->fill) break;
            AcquireSRWLockExclusive(&d->gpu_lock);
            if (d->overlay_frame) {
                d->result = dm_m4_window_present_observe(w, d->width, d->height, d->format, d->fill, d->source,
                                                         observe_frame, d);
                if (d->result != VK_SUCCESS) dm_m4_overlay_discard(d->overlay);
            } else {
                d->result = dm_m4_window_present_fill(w, d->width, d->height, d->format, d->fill, d->source);
            }
            ReleaseSRWLockExclusive(&d->gpu_lock);
            if (d->result < 0) break;
            SetEvent(d->complete);
        } else if (wake != WAIT_OBJECT_0 + 1 || !dm_m4_window_poll(w)) {
            d->result = VK_ERROR_SURFACE_LOST_KHR;
            break;
        }
    }
done:
    AcquireSRWLockExclusive(&d->gpu_lock);
    dm_replay_destroy(d->replay);
    d->replay = NULL;
    dm_m4_window_destroy(w);
    ReleaseSRWLockExclusive(&d->gpu_lock);
    return (unsigned)d->result;
}

static VkResult await_reply(struct dm_display *d) {
    HANDLE events[] = {d->complete, d->thread};
    DWORD wake = WaitForMultipleObjects(2, events, FALSE, INFINITE);
    if (wake == WAIT_OBJECT_0) return d->result;
    DWORD result;
    return wake == WAIT_OBJECT_0 + 1 && GetExitCodeThread(d->thread, &result) ?
           (VkResult)(int32_t)result : VK_ERROR_INITIALIZATION_FAILED;
}

static void release_display(struct dm_display *d) {
    if (d->thread) CloseHandle(d->thread);
    if (d->request) CloseHandle(d->request);
    if (d->complete) CloseHandle(d->complete);
    dm_m4_overlay_close(d->overlay);
    free(d);
}

VkResult dm_display_open(struct dm_display **out, uint32_t width, uint32_t height) {
    *out = NULL;
    struct dm_display *d = calloc(1, sizeof(*d));
    if (!d) return VK_ERROR_OUT_OF_HOST_MEMORY;
    d->width = width;
    d->height = height;
    d->overlay = dm_m4_overlay_open();
    d->request = CreateEventW(NULL, FALSE, FALSE, NULL);
    d->complete = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (d->request && d->complete) d->thread = (HANDLE)_beginthreadex(NULL, 0, display_loop, d, 0, NULL);
    VkResult r = d->thread ? await_reply(d) : VK_ERROR_INITIALIZATION_FAILED;
    if (r != VK_SUCCESS) { release_display(d); return r; }
    *out = d;
    return VK_SUCCESS;
}

const char *dm_display_borrow_draw(struct dm_display *d, const struct dm_m4_replay_keys *keys,
                                   struct dm_m4_replay_session *out) {
    AcquireSRWLockExclusive(&d->gpu_lock);
    const char *error = dm_replay_shaders(d->replay, keys, out);
    if (error) ReleaseSRWLockExclusive(&d->gpu_lock);
    return error;
}
const char *dm_display_borrow_compute(struct dm_display *d, const uint8_t cs[8],
                                      struct dm_m4_compute_session *out) {
    AcquireSRWLockExclusive(&d->gpu_lock);
    const char *error = dm_replay_compute_shader(d->replay, cs, out);
    if (error) ReleaseSRWLockExclusive(&d->gpu_lock);
    return error;
}
void dm_display_return_draw(struct dm_display *d) { ReleaseSRWLockExclusive(&d->gpu_lock); }
struct dm_m4_overlay *dm_display_overlay(struct dm_display *d) { return d->overlay; }

VkResult dm_display_frame(struct dm_display *d, uint32_t width, uint32_t height,
                          VkFormat format, dm_m4_fill_frame fill, void *source,
                          const struct dm_m4_overlay_frame *overlay_frame) {
    // The caller serializes frames and close. The fill context and its guest
    // storage stay alive until the completion event or the thread's exit.
    d->width = width;
    d->height = height;
    d->format = format;
    d->fill = fill;
    d->source = source;
    d->overlay_frame = overlay_frame;
    SetEvent(d->request);
    return await_reply(d);
}

void dm_display_close(struct dm_display *d) {
    d->fill = NULL;
    SetEvent(d->request);
    WaitForSingleObject(d->thread, INFINITE);
    release_display(d);
}
