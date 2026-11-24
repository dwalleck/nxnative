#ifndef DM_WINDOWS_M4_DRAW_H
#define DM_WINDOWS_M4_DRAW_H

#include "m4_pipeline.h"
#include "m4_shader.h"
#include "m4_gmem.h"
#include "../../gfx/nvn/nvn_tex.h"

struct dm_m4_draw;

// Required by fixed-function state, separately from SPIR-V capabilities.
enum dm_m4_draw_feature {
    DM_M4_DRAW_NEGATIVE_ONE_TO_ONE = 1u << 0,
    DM_M4_DRAW_DEPTH_CLAMP = 1u << 1,
    DM_M4_DRAW_DEPTH_BIAS_CLAMP = 1u << 2,
    DM_M4_DRAW_NON_SOLID = 1u << 3,
    DM_M4_DRAW_WIDE_LINES = 1u << 4,
    DM_M4_DRAW_LOGIC_OP = 1u << 5,
    DM_M4_DRAW_DUAL_SRC_BLEND = 1u << 6,
    DM_M4_DRAW_ANISOTROPY = 1u << 7,
    DM_M4_DRAW_BC = 1u << 8,
    DM_M4_DRAW_ASTC_LDR = 1u << 9,
    DM_M4_DRAW_ETC2 = 1u << 10,
    DM_M4_DRAW_ALPHA_TO_ONE = 1u << 11,
    DM_M4_DRAW_DEPTH_BOUNDS = 1u << 12,
    DM_M4_DRAW_SAMPLE_RATE_SHADING = 1u << 13,
    DM_M4_DRAW_VERTEX_STORAGE_WRITES = 1u << 14,
    DM_M4_DRAW_FRAGMENT_STORAGE_WRITES = 1u << 15,
    DM_M4_DRAW_VERTEX_DIVISOR = 1u << 16,
    DM_M4_DRAW_FULL_UINT32_INDEX = 1u << 17,
    DM_M4_DRAW_CONSERVATIVE_RASTER = 1u << 18,
    DM_M4_DRAW_INDEPENDENT_BLEND = 1u << 19,
    DM_M4_DRAW_LARGE_POINTS = 1u << 20,
    DM_M4_DRAW_SHADING_RATE_IMAGE = 1u << 21,
};

struct dm_m4_draw_capabilities {
    VkShaderStageFlagBits stage;
    uint32_t count;
    const uint32_t *values;
};

// Do not copy or move this storage after init: its pointers are self-referential.
// Use the union of capabilities of all shaders to be replayed on this device.
struct dm_m4_draw_requirements {
    struct dm_m4_device_requirements requirements;
    const char *extensions[7];
    VkPhysicalDeviceFeatures core;
    VkPhysicalDeviceVulkan11Features vk11;
    VkPhysicalDeviceVulkan12Features vk12;
    VkPhysicalDeviceDynamicRenderingFeaturesKHR rendering;
    VkPhysicalDeviceDepthClipControlFeaturesEXT clip;
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock;
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT divisor;
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh;
    VkPhysicalDeviceShadingRateImageFeaturesNV rate;
    uint32_t draw_features;
    VkBool32 compute;
    VkShaderStageFlags subgroup_stages[8];
};

// On failure *step is a static diagnostic when supplied. Caps may contain
// repeated entries for a stage (e.g. multiple shader binaries in a library).
VkResult dm_m4_draw_requirements_init(struct dm_m4_draw_requirements *out, uint32_t draw_features,
                                     const struct dm_m4_draw_capabilities *caps, uint32_t count,
                                     const char **step);

struct dm_m4_draw_span {
    const void *data;
    size_t size;
};

// Top-down byte rows, or top-down block rows for compressed sampled images.
// Zero row_pitch denotes tight rows. size bounds all accessed bytes.
struct dm_m4_draw_pixels {
    const void *data;
    size_t size, row_pitch;
};

enum { DM_M4_RATE_PALETTE_SIZE = 16, DM_M4_RATE_MAX_PALETTES = 16 };
struct dm_m4_rate_image {
    struct dm_m4_draw_pixels texels; // R8_UINT palette indices; all zero for an unbound image.
    uint32_t width, height;
    VkShadingRatePaletteEntryNV palette[DM_M4_RATE_PALETTE_SIZE];
    // Slots [1, viewport_count) for a lowered VPRS vertex shader; NULL for one viewport.
    const VkShadingRatePaletteEntryNV (*additional_palettes)[DM_M4_RATE_PALETTE_SIZE];
};

struct dm_m4_draw_mutable_pixels {
    void *data;
    size_t size, row_pitch;
};

struct dm_m4_draw_attachment {
    VkFormat format;
    uint32_t width, height;
    struct dm_m4_draw_mutable_pixels pixels;
    // Required for a combined depth/stencil format, empty otherwise. Plane
    // packing follows Vulkan buffer/image copies: depth is D16, X8_D24 or D32
    // and stencil is S8. Color pixel packing is exactly format.
    struct dm_m4_draw_mutable_pixels stencil;
    // Optional exact-content proof. A nonzero input token identifies all
    // supplied planes, in the same format and shape, after guest writeback.
    // Supply a fresh, never-recycled output token for each prepared snapshot.
    // Never use it as an input until this draw and its guest writeback succeed.
    uint64_t input_token, output_token;
};

// The upload descriptor, encoded spans and callback data are borrowed through
// synchronous graphics submission. A hit requires the same registered view
// and byte-exact backing. Supply exactly one materializer.
struct dm_m4_draw_sample_layout {
    nvn_texdesc desc;
    size_t offset, pitch;
    uint32_t base_level, base_layer;
    int format; // NVN view format, including any depth conversion
};

struct dm_m4_draw_sample_upload {
    uint64_t pool_generation;
    uint32_t id, kind, layers;
    struct dm_m4_draw_sample_layout layout;
    const uint8_t *encoded;
    size_t encoded_size, layer_bytes, layer_stride;
    void *context;
    const char *(*materialize)(void *context, const struct dm_m4_draw_pixels **planes);
    // Each mip is written once, in order, on a miss. The destination is tight:
    // all block rows and z slices of each layer, with no gaps between layers.
    const char *(*write_mip)(void *context, uint32_t mip, struct dm_m4_draw_mutable_pixels dst);
};

struct dm_m4_draw_sampled_image {
    VkFormat format;
    uint32_t width, height, level_count;
    // [layer * level_count + mip], mip 0 is largest. A volume mip has all
    // its z slices in one entry, each spanning block_rows * effective row pitch bytes.
    const struct dm_m4_draw_pixels *levels;
    VkComponentMapping components;
    VkSamplerCreateInfo sampler; // .sType must be set; .pNext is unsupported
    uint32_t array_layers; // nonzero: 2D array, even for one layer
    uint32_t depth; // nonzero: 3D, even for one slice; mutually exclusive with array_layers
    VkBool32 cube; // six faces in +X, -X, +Y, -Y, +Z, -Z order, not array_layers
    const struct dm_m4_draw_sample_upload *upload;
};

// Single-level 2D or 3D storage view with identity swizzle. Pixels hold all
// slices consecutively. For ImageStore, writeback must equal pixels.data.
struct dm_m4_draw_storage_image {
    VkFormat format;
    uint32_t width, height;
    struct dm_m4_draw_pixels pixels;
    void *writeback;
    uint32_t depth; // nonzero for a 3D view, even with only one z slice
};

struct dm_m4_draw_binding {
    struct dm_m4_draw_span buffer;
    // Required for a reflected writable storage binding; leave NULL otherwise.
    // Must equal buffer.data and be valid for buffer.size bytes.
    void *writeback;
    const struct dm_m4_draw_sampled_image *texture;
    const struct dm_m4_draw_storage_image *image;
    VkFormat texel_format; // UNDEFINED uses the reflected format
    const VkSamplerCreateInfo *sampler; // separate sampler or per-binding override of texture->sampler
    union {
        const struct dm_m4_draw_sampled_image *const *texture_array;
        const VkSamplerCreateInfo *sampler_array;
    };
    uint32_t descriptor_count; // live prefix of a reflected partially bound array
};

struct dm_m4_draw_stage {
    const struct dm_m4_shader *shader;
    // shader->binding_count entries in reflection order (NULL only for an
    // empty set). Support and c[1] entries are unused: supplied by the backend.
    const struct dm_m4_draw_binding *bindings;
};

struct dm_m4_draw_attachment_writes {
    uint32_t colors; // Bit per color slot.
    VkImageAspectFlags depth_stencil;
};

struct dm_m4_draw_info {
    struct dm_m4_draw_stage vs, ps;
    struct dm_m4_draw_stage ms, ts; // alternative to vs; task and fragment may be absent
    uint32_t first_task, task_count; // direct NVN task (or taskless mesh) workgroup range
    uint64_t *samples_passed; // Accumulate the precise occlusion result after a successful draw.
    // Optional output: planes with changed texels copied back; empty on errors or no draw.
    struct dm_m4_draw_attachment_writes *attachment_writes;
    struct dm_m4_gmem_source gmem; // used only by mesh/task shaders with checked raw memory
    struct dm_m4_shader_support support;
    float point_size; // nonzero: specialize NVN's fixed point size in the primary shader
    // layout, shader modules, entry points and attachment formats are populated
    // by submit. Single-sample rasterization; replicate viewport and scissor
    // when viewport_count > 1 for per-primitive shading-rate palette selection.
    struct dm_m4_pipeline_info pipeline;
    VkViewport viewport;
    VkRect2D scissor;
    VkExtent2D extent; // shared render area, including attachmentless draws
    const struct dm_m4_rate_image *rate_image; // NULL disables shading-rate image and palette.
    uint32_t color_count; // one past highest bound color slot, not number of images
    struct dm_m4_draw_attachment colors[DM_M4_NVN_COLOR_TARGETS], depth; // unbound formats are VK_FORMAT_UNDEFINED
    struct dm_m4_draw_span indices; // starts at the first index to draw
    VkIndexType index_type; // UINT8_EXT is expanded without a device extension
    uint32_t restart_index; // Guest marker, compared before base_vertex and without truncation.
    uint32_t index_count, instance_count, first_instance;
    uint32_t vertex_count; // Nonzero for a nonindexed draw; base_vertex is firstVertex.
    int32_t base_vertex;
    // Indexed by VkVertexInputBindingDescription.binding, not compact ordinal.
    struct dm_m4_draw_span vertices[DM_M4_NVN_VERTEX_SLOTS];
    // Maxwell fetches zero bytes past each bound stream. Never read those
    // bytes from guest memory, even if the surrounding pool is readable.
    size_t vertex_zero_tail[DM_M4_NVN_VERTEX_SLOTS];
};

struct dm_m4_compute_info {
    struct dm_m4_draw_stage cs;
    struct dm_m4_shader_support support;
    uint32_t groups[3];
    struct dm_m4_gmem_source gmem;
};

// Initialize the invariant defaults in the translator's std140 support buffer.
// The caller fills any guest-specific alpha-test, viewport, or scale state.
void dm_m4_draw_support_init(struct dm_m4_shader_support *support);

// Device is borrowed and must have been created with requirements. The module
// is single-threaded: synchronize host access to queue with all other users.
VkResult dm_m4_draw_create(struct dm_m4_draw **out, const struct dm_m4_device *device,
                          const struct dm_m4_draw_requirements *requirements, const char **step);

// Synchronously uploads geometry and any nonresident input planes, draws, and
// copies potentially modified planes and writable storage back after the fence.
// Attachment row padding is untouched. Set all Vk*CreateInfo sTypes. Keep the
// shader modules and descriptor set layouts alive until the draw context is
// destroyed; its pipeline cache uses the immutable shader handles as keys.
VkResult dm_m4_draw_submit(struct dm_m4_draw *draw, const struct dm_m4_draw_info *info, const char **step);

// Dispatch on the same queue, honoring the same shader lifetime and host
// serialization contract. Writable images and buffers are current on return.
VkResult dm_m4_compute_submit(struct dm_m4_draw *draw, const struct dm_m4_compute_info *info, const char **step);

// Tightly packed texels, one image after another. The source callback reads
// guest storage into Vulkan staging; the destination callback runs after the
// fence and writes only the requested region back to guest storage.
struct dm_m4_blit_plane {
    NVNformat format;
    uint32_t width, height, images;
    size_t bytes;
    const void *context;
    void (*transfer)(const void *context, int write, void *bytes);
};
VkResult dm_m4_texture_blit(struct dm_m4_draw *draw, const struct dm_m4_blit_plane planes[2], int volume,
                           uint32_t flags, const char **step);
// Call with replay excluded, before guest protection, backing or ownership changes.
void dm_m4_draw_forget_imports(struct dm_m4_draw *draw, const void *host, size_t size);
void dm_m4_draw_destroy(struct dm_m4_draw *draw);

#endif
