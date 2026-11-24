#ifndef DM_WINDOWS_M4_GUEST_RESOURCES_H
#define DM_WINDOWS_M4_GUEST_RESOURCES_H

#include "m4_draw.h"
#include "m4_draw_state.h"

struct dm_m4_resource_chunk;
struct dm_m4_resource_writeback;
struct dm_m4_bindless_pool;
struct dm_m4_retained_attachment;

// Zero-initialize once per owner. This storage is not copyable after first use.
// Staging allocation is reused. Retained descriptor snapshots do not own the
// backing texture bytes; attachment snapshots are checked against live storage.
// Large tiled attachments may use the retained linear snapshot as mutable pixels.
struct dm_m4_guest_resources {
    struct dm_m4_resource_chunk *chunks;
    struct dm_m4_resource_writeback *writeback;
    struct dm_m4_bindless_pool *bindless[2]; // texture, sampler
    struct dm_m4_retained_attachment *attachments[1 + DM_M4_DRAW_TARGETS]; // depth/stencil, then color slots
    struct dm_m4_retained_attachment *stream_attachments[1 + DM_M4_DRAW_TARGETS]; // same slot order; linear only
    size_t attachment_bytes;
    uint64_t samples_passed; // Persistent logical queue counter, reset by the guest.
    int count_samples;
};

// Load the stages' reflected bindings and the bound color and depth/stencil
// attachments into info. The caller provides the active shaders and first_instance.
// Support and c[1] are bound by the draw backend. Neither support nor fixed-function
// state is changed here. Bindings and planes borrow the resolver's live memory and
// workspace storage: submit synchronously, before reusing either. On error, do not submit.
const char *dm_m4_guest_resources_prepare(struct dm_m4_guest_resources *workspace,
                                         const struct dm_m4_draw_state *state,
                                         const struct dm_m4_draw_resolver *resolve,
                                         struct dm_m4_draw_info *info);

// After a successful submission, commit only the named draw attachment planes
// (NULL for compute) and staged writable storage images to guest texture storage.
// The backend writes direct linear planes and storage buffers in place.
// Successful writeback also commits the exact snapshots and their backend
// content tokens, including unchanged planes. Do not call on submission failure.
// All strings returned on errors have static lifetime.
const char *dm_m4_guest_resources_writeback(struct dm_m4_guest_resources *workspace,
                                           const struct dm_m4_draw_attachment_writes *writes);

enum dm_m4_clear_type { DM_M4_CLEAR_FLOAT, DM_M4_CLEAR_INT, DM_M4_CLEAR_UINT, DM_M4_CLEAR_DEPTH_STENCIL };
struct dm_m4_clear {
    uint8_t type, index, mask, stencil_mask;
    uint32_t value[4]; // raw color components, or depth bits and stencil value
};
// Apply a recorded clear immediately to the live pool, using the view's base
// mip and all its layers. Color words are interpreted by the attachment format,
// including when the command's input type differs. No draw staging is needed.
const char *dm_m4_clear_attachment(const struct dm_m4_draw_state *state,
                                   const struct dm_m4_draw_resolver *resolve, const struct dm_m4_clear *clear);
const char *dm_m4_clear_texture(const struct dm_m4_draw_texture *texture, const struct dm_m4_draw_scissor *region,
                                const struct dm_m4_draw_resolver *resolve, const struct dm_m4_clear *clear);
void dm_m4_guest_resources_destroy(struct dm_m4_guest_resources *workspace);

#endif
