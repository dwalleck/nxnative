#ifndef DM_WINDOWS_M4_DRAW_STATE_H
#define DM_WINDOWS_M4_DRAW_STATE_H

#include "../../gfx/nvn/include/nvn.h"
#include "../../gfx/nvn/nvn_tex.h"
#include "../../gfx/nvn/dmsh_abi.h"
#include "m4_gmem.h"

// NVN2 stage indices 6 and 7 are task and mesh. Tables use guest slot
// numbers; nothing is remapped to a Vulkan binding or attachment number here.
enum {
    DM_M4_NV_TASK_STAGE = 6,
    DM_M4_NV_MESH_STAGE = 7,
    DM_M4_DRAW_STAGES = 8,
    DM_M4_DRAW_ATTRIBS = 32,
    DM_M4_DRAW_STREAMS = 16,
    DM_M4_DRAW_UBOS = 16,
    DM_M4_DRAW_SSBOS = 16,
    DM_M4_DRAW_TEXTURES = 128,
    DM_M4_DRAW_IMAGES = 32,
    DM_M4_DRAW_TARGETS = 8,
    DM_M4_DRAW_VIEWPORTS = 16,
    DM_M4_RATE_TABLES = 16,
    DM_M4_RATE_ENTRIES = 16,
    DM_M4_RATE_1X1 = 5,
    DM_M4_RATE_4X4 = 11,
    DM_M4_RATE_PALETTE_PRIMITIVE = 0,
    DM_M4_RATE_PALETTE_VIEWPORT = 1,
    DM_M4_RATE_INDEX_CLASSIC = NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_GEOMETRY_BIT |
                               NVN_SHADER_STAGE_TESS_CONTROL_BIT | NVN_SHADER_STAGE_TESS_EVALUATION_BIT,
    DM_M4_RATE_INDEX_STAGES = DM_M4_RATE_INDEX_CLASSIC | (1u << DM_M4_NV_MESH_STAGE),
};

struct dm_m4_texture_view {
    uint8_t set, target, base_level, levels;
    uint16_t format, base_layer, layers;
    uint8_t swizzle[4], depth_stencil_mode;
};
enum {
    DM_M4_VIEW_LEVELS = 1, DM_M4_VIEW_LAYERS = 2, DM_M4_VIEW_FORMAT = 4,
    DM_M4_VIEW_TARGET = 8, DM_M4_VIEW_SWIZZLE = 16, DM_M4_VIEW_DEPTH_STENCIL = 32,
};
struct dm_m4_draw_texture {
    const void *pool; // Opaque guest backing-pool identity; NULL means unbound.
    ptrdiff_t offset;
    nvn_texdesc desc;
    struct dm_m4_texture_view view;
    uint8_t swizzle[4], depth_stencil_mode;
};
struct dm_m4_draw_sampler {
    float lod_min, lod_max, lod_bias, aniso, lod_snap;
    uint32_t border[4], modes;
};
struct dm_m4_draw_pool {
    uintptr_t address;
    uint32_t count;
};
struct dm_m4_draw_viewport { float x, y, width, height; };
struct dm_m4_draw_scissor { int32_t x, y, width, height; };
struct dm_m4_draw_depth_range { float near_z, far_z; };
struct dm_m4_draw_polygon_offset { float factor, units, clamp; };
struct dm_m4_draw_stencil { uint8_t compare_mask, write_mask, reference; };

struct dm_m4_draw_state {
    uint32_t shader_mask;
    uint32_t point_size_outputs;
    uint32_t rate_index_outputs; // NVN stage bits: SPH exports a VPRS table index.
    uint32_t render_disabled; // Evaluated in queue order; zero is the initial render-enable state.
    uint8_t shader_keys[DM_M4_DRAW_STAGES][8]; // first 8 SHA-256 bytes, in digest order
    uint32_t attrib_count, stream_count;
    NVNvertexAttribState attribs[DM_M4_DRAW_ATTRIBS];
    NVNvertexStreamState streams[DM_M4_DRAW_STREAMS];
    NVNbufferRange vertex[DM_M4_DRAW_STREAMS];
    NVNbufferRange uniform[DM_M4_DRAW_STAGES][DM_M4_DRAW_UBOS];
    NVNbufferRange storage[DM_M4_DRAW_STAGES][DM_M4_DRAW_SSBOS];
    // Distinguish explicitly null uniform bindings from never-bound slots.
    uint16_t uniform_set[DM_M4_DRAW_STAGES];
    uint32_t textures[DM_M4_DRAW_STAGES][DM_M4_DRAW_TEXTURES]; // texture ID : 20, sampler ID : 12
    uint32_t images[DM_M4_DRAW_STAGES][DM_M4_DRAW_IMAGES]; // image ID in the texture pool
    uint8_t compute_push[DMSH_PUSH_BLOCKS * DMSH_PUSH_SIZE];
    struct dm_m4_draw_pool texture_pool, sampler_pool;

    uint32_t color_count;
    struct dm_m4_draw_texture colors[DM_M4_DRAW_TARGETS], depth;
    struct dm_m4_draw_texture rate_image; // Record-time snapshot; NULL pool means unbound.
    uint32_t rate_palette[DM_M4_RATE_TABLES][DM_M4_RATE_ENTRIES]; // Raw NVN rates; read only enabled tables.
    uint32_t rate_palette_mask;
    uint8_t rate_palette_mode; // NVN selects a table by primitive VPRS index or viewport index.
    uint16_t viewport_set, scissor_set;
    uint8_t viewport_origin[DM_M4_DRAW_VIEWPORTS]; // NVNwindowOriginMode, at viewport recording
    struct dm_m4_draw_viewport viewports[DM_M4_DRAW_VIEWPORTS];
    struct dm_m4_draw_scissor scissors[DM_M4_DRAW_VIEWPORTS];
    struct dm_m4_draw_depth_range depth_ranges[DM_M4_DRAW_VIEWPORTS];
    struct dm_m4_draw_depth_range depth_bounds;
    NVNcolorState color;
    NVNblendState blend[DM_M4_DRAW_TARGETS];
    float blend_color[4];
    NVNchannelMaskState channel_mask;
    NVNdepthStencilState depth_stencil;
    NVNpolygonState polygon;
    NVNmultisampleState multisample;
    struct dm_m4_draw_polygon_offset polygon_offset;
    float line_width, point_size;
    uint32_t sample_mask; // low 16 bits from NVN, stored as a Vulkan mask word
    uint32_t restart_index;
    uint8_t restart_enable;
    struct dm_m4_draw_stencil stencil[2]; // front, back
    NVNboolean depth_clamp, depth_bounds_test, rasterizer_discard, conservative_raster;
    uint8_t geometry_mode; // 0: classic stages; 1 and 2 select the mesh pipeline family.
};

struct dm_m4_raster_modes {
    NVNwindowOriginMode origin; // fallback if no viewport is set
    NVNwindowOriginMode queue_origin; // winding uses the queue's initialization origin
    NVNdepthMode depth_mode;
};

struct dm_m4_indexed_draw {
    NVNdrawPrimitive primitive;
    NVNindexType index_type;
    uint32_t count, instances, first_instance;
    int arrays; // Nonindexed: base_vertex is the first vertex, not an index bias.
    int32_t base_vertex;
    NVNbufferAddress indices;
    const void *guest_device; // opaque identity
    struct dm_m4_raster_modes raster;
};

struct dm_m4_mesh_draw {
    uint32_t first, count;
    const void *guest_device;
    struct dm_m4_raster_modes raster;
};

struct dm_m4_dispatch {
    const void *guest_device;
    uint32_t groups[3];
};

struct dm_m4_draw_resolver {
    const void *context;
    // Borrowed storage; NULL on invalid/unmapped range. Ask for write access
    // before passing storage to a backend that can modify guest bytes.
    void *(*buffer)(const void *context, NVNbufferAddress address, size_t size, int write);
    void *(*texture_storage)(const void *context, const struct dm_m4_draw_texture *texture,
                             nvn_texlayout *layout, int write);
    // Copy a populated entry from the indicated active pool. Resolve at draw
    // time, so registrations after command recording are visible, and IDs
    // from uniforms/bindless resources use the same lookup as bound handles.
    // Return 0 for absent or invalid entries; seeded sampler 0 is available
    // whenever a live sampler pool is bound.
    int (*texture)(const void *context, unsigned id, struct dm_m4_draw_texture *out);
    int (*sampler)(const void *context, unsigned id, struct dm_m4_draw_sampler *out);
    dm_m4_gmem_visit gmem_visit; // optional live shader address space
    // A nonzero serial changes when the bound table is replaced or modified.
    // Logical entries are consistent for the duration of this resolver borrow.
    uint64_t (*pool_generation)(const void *context, int sampler);
};

// Synchronous adapter. The state, resolver, and borrowed spans may not escape
// this call. Return NULL only after successful execution (including all writes
// needed by later commands), otherwise a diagnostic string with static lifetime.
struct dm_m4_guest_resources;
const char *dm_m4_execute_nvn_indexed(struct dm_m4_guest_resources *resources,
                                     const struct dm_m4_draw_state *state,
                                     const struct dm_m4_indexed_draw *draw,
                                     const struct dm_m4_draw_resolver *resolve);
const char *dm_m4_execute_nvn_mesh(struct dm_m4_guest_resources *resources,
                                  const struct dm_m4_draw_state *state,
                                  const struct dm_m4_mesh_draw *draw,
                                  const struct dm_m4_draw_resolver *resolve);
const char *dm_m4_execute_nvn_compute(struct dm_m4_guest_resources *resources,
                                      const struct dm_m4_draw_state *state,
                                      const struct dm_m4_dispatch *dispatch,
                                      const struct dm_m4_draw_resolver *resolve);

#endif
