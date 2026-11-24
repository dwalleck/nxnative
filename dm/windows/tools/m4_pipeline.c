#define VK_NO_PROTOTYPES
#include "m4_pipeline.h"
#include "m4_window.h"

#include <string.h>

VkResult dm_m4_pipeline_create(const struct dm_m4_device *device,
                               const struct dm_m4_pipeline_info *info,
                               VkPipeline *out) {
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    const int mesh = info && info->ms;
    if (!device || !device->device || !device->get_device_proc || !info || !info->layout ||
        !info->rasterization || !info->viewport_count ||
        (mesh ? (info->vs || info->vertex_input || !info->ms_entry || (info->ts && !info->ts_entry) ||
                 (info->ps && !info->ps_entry)) :
                (!info->vs || !info->vs_entry || (info->ps && !info->ps_entry) || info->ts ||
                 !info->vertex_input || info->topology == VK_PRIMITIVE_TOPOLOGY_MAX_ENUM)))
        return VK_ERROR_INITIALIZATION_FAILED;
    if (info->depth_format != VK_FORMAT_UNDEFINED && info->stencil_format != VK_FORMAT_UNDEFINED &&
        info->depth_format != info->stencil_format)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    uint32_t color_count = info->blend ? info->blend->attachmentCount : 0;
    if ((color_count && (!info->color_formats || !info->blend->pAttachments)) ||
        (!info->rasterization->rasterizerDiscardEnable &&
         (!info->multisample || (!info->depth_stencil &&
          (info->depth_format != VK_FORMAT_UNDEFINED || info->stencil_format != VK_FORMAT_UNDEFINED)))))
        return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkCreateGraphicsPipelines create = (PFN_vkCreateGraphicsPipelines)
        device->get_device_proc(device->device, "vkCreateGraphicsPipelines");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkPipelineShaderStageCreateInfo stages[3];
    uint32_t stage_count = 0;
#define STAGE(kind, handle, entry) \
    stages[stage_count++] = (VkPipelineShaderStageCreateInfo){ \
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, \
        .stage = kind, .module = handle, .pName = entry}
    if (info->ts) { STAGE(VK_SHADER_STAGE_TASK_BIT_EXT, info->ts, info->ts_entry); }
    if (mesh) { STAGE(VK_SHADER_STAGE_MESH_BIT_EXT, info->ms, info->ms_entry); }
    else { STAGE(VK_SHADER_STAGE_VERTEX_BIT, info->vs, info->vs_entry); }
    stages[stage_count - 1].pSpecializationInfo = info->primary_specialization;
    if (info->ps) { STAGE(VK_SHADER_STAGE_FRAGMENT_BIT, info->ps, info->ps_entry); }
#undef STAGE
    const VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = info->topology, .primitiveRestartEnable = info->primitive_restart_enable,
    };
    const VkPipelineViewportDepthClipControlCreateInfoEXT clip = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT,
        .negativeOneToOne = VK_TRUE,
    };
    const VkPipelineViewportShadingRateImageStateCreateInfoNV rate = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_SHADING_RATE_IMAGE_STATE_CREATE_INFO_NV,
        .pNext = info->negative_one_to_one ? &clip : NULL, .shadingRateImageEnable = VK_TRUE,
        .viewportCount = info->viewport_count, .pShadingRatePalettes = info->shading_rate_palettes,
    };
    const VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = info->shading_rate_palettes ? (const void *)&rate : info->negative_one_to_one ? &clip : NULL,
        .viewportCount = info->viewport_count, .scissorCount = info->viewport_count,
    };
    static const VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    const VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = states,
    };
    const VkPipelineRenderingCreateInfoKHR rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = color_count, .pColorAttachmentFormats = info->color_formats,
        .depthAttachmentFormat = info->depth_format, .stencilAttachmentFormat = info->stencil_format,
    };
    const VkGraphicsPipelineCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering, .stageCount = stage_count, .pStages = stages,
        .pVertexInputState = info->vertex_input, .pInputAssemblyState = mesh ? NULL : &assembly,
        .pViewportState = &viewport, .pRasterizationState = info->rasterization,
        .pMultisampleState = info->multisample, .pDepthStencilState = info->depth_stencil,
        .pColorBlendState = info->blend, .pDynamicState = &dynamic,
        .layout = info->layout, .renderPass = VK_NULL_HANDLE, .basePipelineIndex = -1,
    };
    return create(device->device, VK_NULL_HANDLE, 1, &ci, NULL, out);
}

void dm_m4_pipeline_destroy(const struct dm_m4_device *device, VkPipeline pipeline) {
    if (!pipeline) return;
    PFN_vkDestroyPipeline destroy = (PFN_vkDestroyPipeline)
        device->get_device_proc(device->device, "vkDestroyPipeline");
    destroy(device->device, pipeline, NULL);
}

VkPrimitiveTopology dm_m4_nvn_topology(NVNdrawPrimitive primitive) {
    switch (primitive) {
    case NVN_DRAW_PRIMITIVE_POINTS:         return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case NVN_DRAW_PRIMITIVE_LINES:          return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case NVN_DRAW_PRIMITIVE_LINE_STRIP:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLES:      return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN:   return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default:                              return VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
    }
}

VkIndexType dm_m4_nvn_index_type(NVNindexType type) {
    switch (type) {
    case NVN_INDEX_TYPE_UNSIGNED_BYTE:  return VK_INDEX_TYPE_UINT8_EXT;
    case NVN_INDEX_TYPE_UNSIGNED_SHORT: return VK_INDEX_TYPE_UINT16;
    case NVN_INDEX_TYPE_UNSIGNED_INT:   return VK_INDEX_TYPE_UINT32;
    default:                           return VK_INDEX_TYPE_MAX_ENUM;
    }
}

// Formats common to vertex fetch and images. A mapping describes the bits, not
// necessarily support on this device; the caller checks the format's usage bits.
#define VECTOR_FORMATS(N, S8, S16, S32) \
    [NVN_FORMAT_##N##8]    = VK_FORMAT_##S8##_UNORM, \
    [NVN_FORMAT_##N##8SN]  = VK_FORMAT_##S8##_SNORM, \
    [NVN_FORMAT_##N##8UI]  = VK_FORMAT_##S8##_UINT, \
    [NVN_FORMAT_##N##8I]   = VK_FORMAT_##S8##_SINT, \
    [NVN_FORMAT_##N##16F]  = VK_FORMAT_##S16##_SFLOAT, \
    [NVN_FORMAT_##N##16]   = VK_FORMAT_##S16##_UNORM, \
    [NVN_FORMAT_##N##16SN] = VK_FORMAT_##S16##_SNORM, \
    [NVN_FORMAT_##N##16UI] = VK_FORMAT_##S16##_UINT, \
    [NVN_FORMAT_##N##16I]  = VK_FORMAT_##S16##_SINT, \
    [NVN_FORMAT_##N##32F]  = VK_FORMAT_##S32##_SFLOAT, \
    [NVN_FORMAT_##N##32UI] = VK_FORMAT_##S32##_UINT, \
    [NVN_FORMAT_##N##32I]  = VK_FORMAT_##S32##_SINT
static VkFormat common_format(NVNformat format) {
    static const VkFormat formats[] = {
        VECTOR_FORMATS(R, R8, R16, R32),
        VECTOR_FORMATS(RG, R8G8, R16G16, R32G32),
        VECTOR_FORMATS(RGB, R8G8B8, R16G16B16, R32G32B32),
        VECTOR_FORMATS(RGBA, R8G8B8A8, R16G16B16A16, R32G32B32A32),
        [NVN_FORMAT_RGB565] = VK_FORMAT_B5G6R5_UNORM_PACK16,
        [NVN_FORMAT_BGR565] = VK_FORMAT_R5G6B5_UNORM_PACK16,
        [NVN_FORMAT_RGB10A2] = VK_FORMAT_A2B10G10R10_UNORM_PACK32,
        [NVN_FORMAT_RGB10A2UI] = VK_FORMAT_A2B10G10R10_UINT_PACK32,
        [NVN_FORMAT_RGB10A2SN] = VK_FORMAT_A2B10G10R10_SNORM_PACK32,
        [NVN_FORMAT_RGB10A2I] = VK_FORMAT_A2B10G10R10_SINT_PACK32,
        [NVN_FORMAT_R11G11B10F] = VK_FORMAT_B10G11R11_UFLOAT_PACK32,
        [NVN_FORMAT_RGB9E5F] = VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
        [NVN_FORMAT_BGRA8] = VK_FORMAT_B8G8R8A8_UNORM,
    };
    return (unsigned)format < sizeof formats / sizeof *formats ? formats[format] : VK_FORMAT_UNDEFINED;
}
#undef VECTOR_FORMATS

VkFormat dm_m4_nvn_vertex_format(NVNformat format) {
#define SCALED_FORMATS(N, S8, S16) \
    [NVN_FORMAT_##N##8_UI2F]  = VK_FORMAT_##S8##_USCALED, \
    [NVN_FORMAT_##N##8_I2F]   = VK_FORMAT_##S8##_SSCALED, \
    [NVN_FORMAT_##N##16_UI2F] = VK_FORMAT_##S16##_USCALED, \
    [NVN_FORMAT_##N##16_I2F]  = VK_FORMAT_##S16##_SSCALED
    static const VkFormat formats[] = {
        SCALED_FORMATS(R, R8, R16),
        SCALED_FORMATS(RG, R8G8, R16G16),
        SCALED_FORMATS(RGB, R8G8B8, R16G16B16),
        SCALED_FORMATS(RGBA, R8G8B8A8, R16G16B16A16),
        [NVN_FORMAT_RGB10A2_UI2F] = VK_FORMAT_A2B10G10R10_USCALED_PACK32,
        [NVN_FORMAT_RGB10A2_I2F] = VK_FORMAT_A2B10G10R10_SSCALED_PACK32,
    };
#undef SCALED_FORMATS
    if ((unsigned)format < sizeof formats / sizeof *formats && formats[format] != VK_FORMAT_UNDEFINED)
        return formats[format];
    return common_format(format);
}

VkFormat dm_m4_nvn_attachment_format(NVNformat format) {
    switch (format) {
    case NVN_FORMAT_STENCIL8:          return VK_FORMAT_S8_UINT;
    case NVN_FORMAT_DEPTH16:           return VK_FORMAT_D16_UNORM;
    case NVN_FORMAT_DEPTH24:           return VK_FORMAT_X8_D24_UNORM_PACK32;
    case NVN_FORMAT_DEPTH32F:          return VK_FORMAT_D32_SFLOAT;
    case NVN_FORMAT_DEPTH24_STENCIL8:  return VK_FORMAT_D24_UNORM_S8_UINT;
    case NVN_FORMAT_DEPTH32F_STENCIL8: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case NVN_FORMAT_RGBA8_SRGB:        return VK_FORMAT_R8G8B8A8_SRGB;
    case NVN_FORMAT_BGRA8_SRGB:        return VK_FORMAT_B8G8R8A8_SRGB;
    default:                          return common_format(format);
    }
}

// Opaque guest values may have byte alignment. Read the retained words without
// aliasing their types or requiring alignment on the guest representation.
static uint32_t word_at(const void *value, unsigned index) {
    uint32_t word;
    memcpy(&word, (const uint8_t *)value + sizeof word * index, sizeof word);
    return word;
}

VkResult dm_m4_nvn_vertex_input(const NVNvertexAttribState *attributes, uint32_t attribute_count,
                                const NVNvertexStreamState *streams, uint32_t stream_count,
                                struct dm_m4_nvn_vertex_input *out) {
    if (!out || attribute_count > DM_M4_NVN_VERTEX_SLOTS || stream_count > DM_M4_NVN_VERTEX_SLOTS ||
        (attribute_count && !attributes) || (stream_count && !streams))
        return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t used = 0, attribute_descs = 0, binding_descs = 0, divisor_descs = 0;
    for (uint32_t i = 0; i < attribute_count; ++i) {
        const uint32_t word = word_at(&attributes[i], 0);
        NVNformat format = (NVNformat)(word & 0xffu);
        if (format == NVN_FORMAT_NONE) continue;
        uint32_t binding = word >> 27;
        if (binding >= stream_count) return VK_ERROR_INITIALIZATION_FAILED;
        VkFormat vk_format = dm_m4_nvn_vertex_format(format);
        if (vk_format == VK_FORMAT_UNDEFINED) return VK_ERROR_FORMAT_NOT_SUPPORTED;
        out->attributes[attribute_descs++] = (VkVertexInputAttributeDescription){
            .location = i, .binding = binding, .format = vk_format, .offset = (word >> 8) & 0x7ffffu,
        };
        used |= UINT32_C(1) << binding;
    }
    for (uint32_t i = 0; i < stream_count; ++i) {
        if (!(used & (UINT32_C(1) << i))) continue;
        uint32_t divisor = word_at(&streams[i], 1);
        out->bindings[binding_descs++] = (VkVertexInputBindingDescription){
            .binding = i, .stride = word_at(&streams[i], 0),
            .inputRate = divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX,
        };
        if (divisor > 1) out->divisors[divisor_descs++] = (VkVertexInputBindingDivisorDescriptionEXT){
            .binding = i, .divisor = divisor,
        };
    }
    if (divisor_descs) out->divisor_state = (VkPipelineVertexInputDivisorStateCreateInfoEXT){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
        .vertexBindingDivisorCount = divisor_descs, .pVertexBindingDivisors = out->divisors,
    };
    out->state = (VkPipelineVertexInputStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = divisor_descs ? &out->divisor_state : NULL,
        .vertexBindingDescriptionCount = binding_descs, .pVertexBindingDescriptions = out->bindings,
        .vertexAttributeDescriptionCount = attribute_descs, .pVertexAttributeDescriptions = out->attributes,
    };
    return VK_SUCCESS;
}

VkResult dm_m4_nvn_rasterization(const NVNpolygonState *polygon, NVNdrawPrimitive primitive,
                                 VkBool32 flip_winding, VkBool32 depth_clamp, float line_width,
                                 float depth_bias_factor, float depth_bias_units, float depth_bias_clamp,
                                 VkPipelineRasterizationStateCreateInfo *out) {
    if (!polygon || !out) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t word = word_at(polygon, 0), mode = (word >> 3) & 3u;
    static const VkPolygonMode modes[] = {VK_POLYGON_MODE_POINT, VK_POLYGON_MODE_LINE, VK_POLYGON_MODE_FILL};
    if (mode > NVN_POLYGON_MODE_FILL) return VK_ERROR_INITIALIZATION_FAILED;
    unsigned offset_mode = mode;
    switch (primitive) {
    case NVN_DRAW_PRIMITIVE_POINTS: offset_mode = NVN_POLYGON_MODE_POINT; break;
    case NVN_DRAW_PRIMITIVE_LINES:
    case NVN_DRAW_PRIMITIVE_LINE_STRIP: offset_mode = NVN_POLYGON_MODE_LINE; break;
    case NVN_DRAW_PRIMITIVE_TRIANGLES:
    case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP:
    case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: break;
    default: return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    VkBool32 bias = (word >> (15 + offset_mode)) & 1u;
    int ccw = ((word & (1u << 2)) != 0) != (flip_winding != VK_FALSE);
    *out = (VkPipelineRasterizationStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = depth_clamp, .polygonMode = modes[mode], .cullMode = word & 3u,
        .frontFace = ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = bias, .depthBiasConstantFactor = bias ? depth_bias_units : 0.0f,
        .depthBiasClamp = bias ? depth_bias_clamp : 0.0f,
        .depthBiasSlopeFactor = bias ? depth_bias_factor : 0.0f,
        .lineWidth = offset_mode == NVN_POLYGON_MODE_LINE ? line_width : 1.0f,
    };
    return VK_SUCCESS;
}

static int nvn_stencil_face(uint32_t word, const struct dm_m4_nvn_stencil_values *values,
                            VkStencilOpState *out) {
    uint32_t compare = word & 15u, fail = (word >> 4) & 15u;
    uint32_t pass = (word >> 8) & 15u, depth_fail = (word >> 12) & 15u;
    if (!compare || compare > NVN_STENCIL_FUNC_ALWAYS || !fail || fail > NVN_STENCIL_OP_DECR_WRAP ||
        !pass || pass > NVN_STENCIL_OP_DECR_WRAP || !depth_fail || depth_fail > NVN_STENCIL_OP_DECR_WRAP)
        return 0;
    // For these enums the NVN and Vulkan order is the same, starting at 1 and 0.
    *out = (VkStencilOpState){
        .failOp = (VkStencilOp)(fail - 1), .passOp = (VkStencilOp)(pass - 1),
        .depthFailOp = (VkStencilOp)(depth_fail - 1), .compareOp = (VkCompareOp)(compare - 1),
        .compareMask = values->compare_mask, .writeMask = values->write_mask, .reference = values->reference,
    };
    return 1;
}

VkResult dm_m4_nvn_depth_stencil(const NVNdepthStencilState *depth, VkBool32 have_depth,
                                 VkBool32 have_stencil, const struct dm_m4_nvn_stencil_values faces[2],
                                 VkPipelineDepthStencilStateCreateInfo *out) {
    if (!depth || !out) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t word = word_at(depth, 0), compare = (word >> 4) & 15u;
    VkBool32 test = have_depth && (word & 1u), stencil = have_stencil && (word & 4u);
    if (test && (!compare || compare > NVN_DEPTH_FUNC_ALWAYS)) return VK_ERROR_INITIALIZATION_FAILED;
    *out = (VkPipelineDepthStencilStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = test, .depthWriteEnable = test && (word & 2u),
        .depthCompareOp = test ? (VkCompareOp)(compare - 1) : VK_COMPARE_OP_ALWAYS,
        .stencilTestEnable = stencil, .maxDepthBounds = 1.0f,
    };
    if (stencil) {
        uint32_t ops = word_at(depth, 1);
        if (!faces || !nvn_stencil_face(ops, &faces[0], &out->front) ||
            !nvn_stencil_face(ops >> 16, &faces[1], &out->back)) return VK_ERROR_INITIALIZATION_FAILED;
    }
    return VK_SUCCESS;
}

static VkBlendFactor blend_factor(uint32_t factor) {
    switch (factor) {
    case NVN_BLEND_FUNC_ZERO:                         return VK_BLEND_FACTOR_ZERO;
    case NVN_BLEND_FUNC_ONE:                          return VK_BLEND_FACTOR_ONE;
    case NVN_BLEND_FUNC_SRC_COLOR:                    return VK_BLEND_FACTOR_SRC_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR:           return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA:                    return VK_BLEND_FACTOR_SRC_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA:           return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case NVN_BLEND_FUNC_DST_ALPHA:                    return VK_BLEND_FACTOR_DST_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA:           return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case NVN_BLEND_FUNC_DST_COLOR:                    return VK_BLEND_FACTOR_DST_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR:           return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE:            return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case NVN_BLEND_FUNC_SRC1_COLOR:                   return VK_BLEND_FACTOR_SRC1_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR:          return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case NVN_BLEND_FUNC_SRC1_ALPHA:                   return VK_BLEND_FACTOR_SRC1_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA:          return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case NVN_BLEND_FUNC_CONSTANT_COLOR:               return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR:     return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case NVN_BLEND_FUNC_CONSTANT_ALPHA:               return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA:     return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default:                                         return VK_BLEND_FACTOR_MAX_ENUM;
    }
}

enum dm_m4_color_type dm_m4_attachment_color_type(VkFormat format) {
#define INTEGER_FORMAT(S) \
    case VK_FORMAT_##S##_UINT: return DM_M4_COLOR_UINT; \
    case VK_FORMAT_##S##_SINT: return DM_M4_COLOR_SINT
    switch (format) {
    INTEGER_FORMAT(R8); INTEGER_FORMAT(R8G8); INTEGER_FORMAT(R8G8B8); INTEGER_FORMAT(R8G8B8A8);
    INTEGER_FORMAT(R16); INTEGER_FORMAT(R16G16); INTEGER_FORMAT(R16G16B16); INTEGER_FORMAT(R16G16B16A16);
    INTEGER_FORMAT(R32); INTEGER_FORMAT(R32G32); INTEGER_FORMAT(R32G32B32); INTEGER_FORMAT(R32G32B32A32);
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: return DM_M4_COLOR_UINT;
    case VK_FORMAT_A2B10G10R10_SINT_PACK32: return DM_M4_COLOR_SINT;
    default: return DM_M4_COLOR_FLOAT;
    }
#undef INTEGER_FORMAT
}

VkResult dm_m4_nvn_blend(const NVNcolorState *color, const NVNchannelMaskState *channel_mask,
                         const NVNblendState *target_blends, const VkFormat *formats, uint32_t count,
                         uint32_t shader_write_mask, const float constants[4], struct dm_m4_nvn_blend *out) {
    if (!color || !channel_mask || !out || count > DM_M4_NVN_COLOR_TARGETS || (count && !formats))
        return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t state = word_at(color, 0), logic = (state >> 16) & 0xffu;
    if (logic > NVN_LOGIC_OP_SET) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t writes = word_at(channel_mask, 0) & shader_write_mask;
    uint32_t written_colors = 0;
    for (uint32_t i = 0; i < count; ++i) {
        VkPipelineColorBlendAttachmentState *a = &out->attachments[i];
        *a = (VkPipelineColorBlendAttachmentState){0};
        if (formats[i] == VK_FORMAT_UNDEFINED) continue;
        a->colorWriteMask = (writes >> (4 * i)) & 15u;
        written_colors |= a->colorWriteMask;
        // Maxwell applies no blending to integer color attachments.
        if (!a->colorWriteMask || logic != NVN_LOGIC_OP_COPY || dm_m4_attachment_color_type(formats[i]) != DM_M4_COLOR_FLOAT ||
            !(state & (UINT32_C(1) << (8 + i)))) continue;
        if (!target_blends) return VK_ERROR_INITIALIZATION_FAILED;
        uint32_t mode = word_at(&target_blends[i], 0), factors = word_at(&target_blends[i], 1);
        if (((mode >> 8) & 63u) != NVN_BLEND_ADVANCED_MODE_NONE) return VK_ERROR_FEATURE_NOT_PRESENT;
        uint32_t rgb_op = (mode >> 24) & 7u, alpha_op = (mode >> 28) & 7u;
        if (!rgb_op || rgb_op > NVN_BLEND_EQUATION_MAX || !alpha_op || alpha_op > NVN_BLEND_EQUATION_MAX)
            return VK_ERROR_INITIALIZATION_FAILED;
        a->blendEnable = VK_TRUE;
        a->colorBlendOp = (VkBlendOp)(rgb_op - 1);
        a->alphaBlendOp = (VkBlendOp)(alpha_op - 1);
        a->srcColorBlendFactor = blend_factor(factors & 127u);
        a->dstColorBlendFactor = blend_factor((factors >> 8) & 127u);
        a->srcAlphaBlendFactor = blend_factor((factors >> 16) & 127u);
        a->dstAlphaBlendFactor = blend_factor((factors >> 24) & 127u);
        if (a->srcColorBlendFactor == VK_BLEND_FACTOR_MAX_ENUM || a->dstColorBlendFactor == VK_BLEND_FACTOR_MAX_ENUM ||
            a->srcAlphaBlendFactor == VK_BLEND_FACTOR_MAX_ENUM || a->dstAlphaBlendFactor == VK_BLEND_FACTOR_MAX_ENUM)
            return VK_ERROR_INITIALIZATION_FAILED;
    }
    out->state = (VkPipelineColorBlendStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = written_colors && logic != NVN_LOGIC_OP_COPY, .logicOp = (VkLogicOp)logic,
        .attachmentCount = count, .pAttachments = out->attachments,
    };
    if (constants) memcpy(out->state.blendConstants, constants, sizeof out->state.blendConstants);
    return VK_SUCCESS;
}

VkResult dm_m4_nvn_multisample(const NVNmultisampleState *multisample,
                               VkSampleCountFlagBits attachment_samples,
                               VkPipelineMultisampleStateCreateInfo *out) {
    uint32_t count = (uint32_t)attachment_samples;
    if (!multisample || !out || !count || count > VK_SAMPLE_COUNT_64_BIT || (count & (count - 1)))
        return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t word = word_at(multisample, 0);
    uint32_t samples = (word >> 8) & 31u, raster_samples = (word >> 15) & 31u;
    VkBool32 enabled = word & 1u;
    // No mixed samples, custom sample locations, coverage modulation, or
    // coverage-to-color in this adapter. Dither has no independent Vulkan knob.
    if ((!enabled && count > 1) || (samples && samples != count) ||
        (raster_samples && raster_samples != count) ||
        (word & ((1u << 3) | (1u << 5) | (3u << 13) | (1u << 20))))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    *out = (VkPipelineMultisampleStateCreateInfo){
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = attachment_samples, .alphaToCoverageEnable = enabled && (word & 2u),
    };
    return VK_SUCCESS;
}
