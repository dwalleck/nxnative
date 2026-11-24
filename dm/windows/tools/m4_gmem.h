#ifndef DM_M4_GMEM_H
#define DM_M4_GMEM_H

#include <stddef.h>
#include <stdint.h>

struct dm_m4_gmem_span {
    uint64_t guest; // guest GPU virtual address, not a Vulkan device address
    // Endpoints of each guest range and CPU backing are 16-byte aligned.
    // Live ranges share one guest-to-CPU translation; overlaps are aliases.
    void *host;     // validated CPU backing of exactly size bytes
    size_t size;
    // The source also owns the surrounding private guest reservation.
    // Replay exclusion and forget_imports cover protection/backing changes
    // even in parts of that reservation outside the enumerated live spans.
    int stable_private_backing;
};

// A view that cannot share the live pool's Vulkan buffer bytes, such as a
// deswizzled image or a private padded constant buffer.
struct dm_m4_gmem_view {
    const void *data; // underlying guest bytes, before staging
    size_t size;
    int writable;
};

typedef int (*dm_m4_gmem_accept)(void *context, struct dm_m4_gmem_span span);
// Enumerate live pools for this NVN device; stop when accept returns 0.
// Keep these spans mapped and stable until the synchronous submission returns;
// the shader can read any range and a log entry covers the entire interval.
// After submission the renderer may retain a coherent Vulkan import. Before
// replacing/releasing backing or changing its mapping or permissions, exclude
// replay and call dm_m4_draw_forget_imports on each borrowing draw context.
// Pass the entire affected host-page interval, including non-live bytes
// when stable_private_backing is set.
typedef int (*dm_m4_gmem_visit)(const void *context, dm_m4_gmem_accept accept, void *userdata);

// Required by a checked physical-address shader. Keep each staged view's
// original guest span (including tiled storage) valid through the fence.
// Guarded compute raw reads may share a pool with a writable staged view;
// an executed access to its protected span fails before publishing outputs.
struct dm_m4_gmem_source {
    dm_m4_gmem_visit visit;
    const void *context;
    const struct dm_m4_gmem_view *views;
    uint32_t view_count;
};

#endif
