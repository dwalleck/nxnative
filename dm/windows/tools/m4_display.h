#ifndef DM_WINDOWS_M4_DISPLAY_H
#define DM_WINDOWS_M4_DISPLAY_H

#include "m4_window.h"
#include "m4_replay.h"

// Each logical NVN window owns a Win32 message thread. Guest creation,
// presentation and finalization need not be called from that same thread.
struct dm_display;
struct dm_m4_overlay;
struct dm_m4_overlay_frame;
VkResult dm_display_open(struct dm_display **out, uint32_t width, uint32_t height);
VkResult dm_display_frame(struct dm_display *d, uint32_t width, uint32_t height,
                          VkFormat format, dm_m4_fill_frame fill, void *source,
                          const struct dm_m4_overlay_frame *overlay_frame);
struct dm_m4_overlay *dm_display_overlay(struct dm_display *d);
const char *dm_display_borrow_draw(struct dm_display *d, const struct dm_m4_replay_keys *keys,
                                   struct dm_m4_replay_session *out);
const char *dm_display_borrow_compute(struct dm_display *d, const uint8_t cs[8],
                                      struct dm_m4_compute_session *out);
void dm_display_return_draw(struct dm_display *d);
void dm_display_close(struct dm_display *d);

#endif
