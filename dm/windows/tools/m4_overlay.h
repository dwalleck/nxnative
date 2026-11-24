#ifndef DM_WINDOWS_M4_OVERLAY_H
#define DM_WINDOWS_M4_OVERLAY_H

#include "m4_draw.h"
#include "m4_replay.h"

struct dm_m4_overlay;

// Optional per-display observer. The owner serializes draws, presents and close.
struct dm_m4_overlay *dm_m4_overlay_open(void);
void dm_m4_overlay_close(struct dm_m4_overlay *capture);

enum dm_m4_overlay_role { DM_M4_OVERLAY_NONE, DM_M4_OVERLAY_PANE, DM_M4_OVERLAY_BLIT };
enum dm_m4_overlay_role dm_m4_overlay_select(struct dm_m4_overlay *capture,
                                              const struct dm_m4_replay_keys *keys);
// Target storage is the resolved guest address of the base 2D color plane, or
// NULL if that view cannot be verified. Call end only if begin returns true.
int dm_m4_overlay_draw_begin(struct dm_m4_overlay *capture, enum dm_m4_overlay_role role,
                             const struct dm_m4_draw_info *info, const void *target_storage);
void dm_m4_overlay_draw_end(struct dm_m4_overlay *capture, enum dm_m4_overlay_role role,
                             const struct dm_m4_draw_info *info, int success);

struct dm_m4_overlay_frame {
    uint64_t guest_frame;
    const void *storage; // Base address before crop; never dereferenced here.
    uint32_t x, y;
    int ambiguous; // Multiple logical windows on the same NVN device.
};
// Observe only after WSI has accepted the frame, while upload pixels are valid.
void dm_m4_overlay_present(struct dm_m4_overlay *capture, const struct dm_m4_overlay_frame *frame,
                           const void *pixels, VkExtent2D source, VkExtent2D surface, int bgra);
// Retire the candidate on every guest present attempt, including skipped/failed
// WSI presents, or if display registration makes ownership ambiguous.
void dm_m4_overlay_discard(struct dm_m4_overlay *capture);

#endif
