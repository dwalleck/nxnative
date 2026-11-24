#ifndef DM_WINDOWS_M4_PIPELINE_H
#define DM_WINDOWS_M4_PIPELINE_H

#include <vulkan/vulkan_core.h>
#include "../../gfx/nvn/include/nvn.h"

struct dm_m4_device;

struct dm_m4_pipeline_info {
    VkPipelineLayout layout;
    VkShaderModule vs, ps;
    const char *vs_entry, *ps_entry;
    VkShaderModule ms, ts; // mesh instead of vertex input, with optional task and fragment
    const char *ms_entry, *ts_entry;
    const VkSpecializationInfo *primary_specialization; // vertex or mesh stage only

    // Vertex bindings describe streams (including stride and input rate).
    // Attributes use shader locations and reference those binding numbers.
    const VkPipelineVertexInputStateCreateInfo *vertex_input;
    VkPrimitiveTopology topology;
    // If enabled, the index stream must use Vulkan's all-ones restart index.
    VkBool32 primitive_restart_enable;

    const VkPipelineRasterizationStateCreateInfo *rasterization;
    const VkPipelineMultisampleStateCreateInfo *multisample;
    const VkPipelineDepthStencilStateCreateInfo *depth_stencil;
    const VkPipelineColorBlendStateCreateInfo *blend;

    // One format per blend attachment, in fragment output location order.
    // Preserve unbound slots with VK_FORMAT_UNDEFINED; do not compact them.
    const VkFormat *color_formats;
    VkFormat depth_format, stencil_format;

    // Both viewport and scissor rectangles are dynamic, with this fixed count.
    uint32_t viewport_count;
    VkBool32 negative_one_to_one; // Clip-space Z is [-W, W] instead of [0, W].
    const VkShadingRatePaletteNV *shading_rate_palettes; // nonnull: enable NV shading rate, one palette per viewport
};

// Vulkan 1.2: enable VK_KHR_dynamic_rendering and dynamicRendering in
// VkPhysicalDeviceDynamicRenderingFeaturesKHR. For negative_one_to_one also
// enable VK_EXT_depth_clip_control and depthClipControl. A shading-rate palette also
// requires VK_NV_shading_rate_image and shadingRateImage. The device must enable
// any other features used by the shaders or requested fixed-function state.
// The device, modules, layout and Vulkan state structures (with sType set)
// are borrowed. State structures and their arrays are only read during create.
// blend can be NULL for no color targets; depth_stencil can be NULL if both
// formats are undefined; multisample can be NULL if rasterization is discarded.
//
// Use matching formats/count and sample count with vkCmdBeginRenderingKHR
// and set the viewport(s) and scissor(s) before drawing. The caller owns the
// resulting pipeline; on error *out is VK_NULL_HANDLE.
VkResult dm_m4_pipeline_create(const struct dm_m4_device *device,
                               const struct dm_m4_pipeline_info *info,
                               VkPipeline *out);

// Retire all GPU uses before destruction, and destroy before the device.
void dm_m4_pipeline_destroy(const struct dm_m4_device *device, VkPipeline pipeline);

// Decode snapshots, not live guest memory. An unrepresentable state returns
// VK_ERROR_FEATURE_NOT_PRESENT; enum helpers return *_MAX_ENUM or, for formats,
// VK_FORMAT_UNDEFINED.
VkPrimitiveTopology dm_m4_nvn_topology(NVNdrawPrimitive primitive);
// NVN unsigned-byte indices need VK_EXT_index_type_uint8 and indexTypeUint8
// on Vulkan 1.2, or expansion to UINT16 at the binding site.
VkIndexType dm_m4_nvn_index_type(NVNindexType type);
VkFormat dm_m4_nvn_vertex_format(NVNformat format);
VkFormat dm_m4_nvn_attachment_format(NVNformat format);
enum dm_m4_color_type { DM_M4_COLOR_FLOAT, DM_M4_COLOR_UINT, DM_M4_COLOR_SINT };
enum dm_m4_color_type dm_m4_attachment_color_type(VkFormat format);

enum { DM_M4_NVN_VERTEX_SLOTS = 32, DM_M4_NVN_COLOR_TARGETS = 8 };

// The native state's pointers refer to its containing struct; keep it in place
// until pipeline creation returns. Divisors greater than one need
// VK_EXT_vertex_attribute_divisor and vertexAttributeInstanceRateDivisor.
struct dm_m4_nvn_vertex_input {
    VkPipelineVertexInputStateCreateInfo state;
    VkPipelineVertexInputDivisorStateCreateInfoEXT divisor_state;
    VkVertexInputAttributeDescription attributes[DM_M4_NVN_VERTEX_SLOTS];
    VkVertexInputBindingDescription bindings[DM_M4_NVN_VERTEX_SLOTS];
    VkVertexInputBindingDivisorDescriptionEXT divisors[DM_M4_NVN_VERTEX_SLOTS];
};
VkResult dm_m4_nvn_vertex_input(const NVNvertexAttribState *attributes, uint32_t attribute_count,
                                const NVNvertexStreamState *streams, uint32_t stream_count,
                                struct dm_m4_nvn_vertex_input *out);

// flip_winding accounts for a host/guest framebuffer orientation difference.
VkResult dm_m4_nvn_rasterization(const NVNpolygonState *polygon, NVNdrawPrimitive primitive,
                                 VkBool32 flip_winding, VkBool32 depth_clamp, float line_width,
                                 float depth_bias_factor, float depth_bias_units, float depth_bias_clamp,
                                 VkPipelineRasterizationStateCreateInfo *out);

struct dm_m4_nvn_stencil_values {
    uint32_t compare_mask, write_mask, reference;
};
// faces[0] is front, faces[1] is back. Required when stencil testing is active.
VkResult dm_m4_nvn_depth_stencil(const NVNdepthStencilState *depth, VkBool32 have_depth,
                                 VkBool32 have_stencil, const struct dm_m4_nvn_stencil_values faces[2],
                                 VkPipelineDepthStencilStateCreateInfo *out);

// As with the vertex input adapter, don't copy this after translation.
struct dm_m4_nvn_blend {
    VkPipelineColorBlendStateCreateInfo state;
    VkPipelineColorBlendAttachmentState attachments[DM_M4_NVN_COLOR_TARGETS];
};
// Blend snapshots and channel/write masks are indexed by the uncompressed NVN
// target slot. shader_write_mask has one RGBA nibble per slot.
VkResult dm_m4_nvn_blend(const NVNcolorState *color, const NVNchannelMaskState *channel_mask,
                         const NVNblendState *target_blends, const VkFormat *formats, uint32_t count,
                         uint32_t shader_write_mask, const float constants[4], struct dm_m4_nvn_blend *out);

VkResult dm_m4_nvn_multisample(const NVNmultisampleState *multisample,
                               VkSampleCountFlagBits attachment_samples,
                               VkPipelineMultisampleStateCreateInfo *out);

#endif
