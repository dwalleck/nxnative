#define VK_NO_PROTOTYPES
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "m4_draw.h"
#include "m4_phase.h"
#include "m4_bytes_equal.h"

#include <assert.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../gfx/nvn/dmsh_abi.h"

#define DM_M4_DRAW_VK(X) \
    X(CreateCommandPool) X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandBuffer) \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(CreateFence) X(DestroyFence) X(ResetFences) \
    X(QueueSubmit) X(WaitForFences) X(DeviceWaitIdle) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) \
    X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) \
    X(CreateQueryPool) X(DestroyQueryPool) X(CmdResetQueryPool) X(CmdBeginQuery) X(CmdEndQuery) X(GetQueryPoolResults) \
    X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) X(CreateImage) X(DestroyImage) \
    X(GetImageMemoryRequirements) X(BindImageMemory) X(CreateImageView) X(DestroyImageView) \
    X(CreateSampler) X(DestroySampler) X(CreateDescriptorPool) X(DestroyDescriptorPool) \
    X(CreateShaderModule) X(DestroyShaderModule) \
    X(ResetDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreatePipelineLayout) \
    X(DestroyPipelineLayout) X(CmdPipelineBarrier) X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) \
    X(CmdBlitImage) \
    X(CmdBeginRenderingKHR) X(CmdEndRenderingKHR) X(CmdBindPipeline) X(CmdBindDescriptorSets) \
    X(CmdSetViewport) X(CmdSetScissor) X(CmdBindVertexBuffers) X(CmdBindIndexBuffer) X(CmdDrawIndexed) X(CmdDraw)

#define DM_M4_MESH_VK(X) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CmdPushConstants) X(CmdDrawMeshTasksEXT)

#define DM_M4_COMPUTE_VK(X) \
    X(CreateComputePipelines) X(DestroyPipeline) X(CmdDispatch) X(CreateBufferView) X(DestroyBufferView)

#define DM_M4_DRAW_CORE_FLAGS(X) \
    X(DEPTH_CLAMP, depthClamp) X(DEPTH_BIAS_CLAMP, depthBiasClamp) X(NON_SOLID, fillModeNonSolid) \
    X(WIDE_LINES, wideLines) X(LOGIC_OP, logicOp) X(DUAL_SRC_BLEND, dualSrcBlend) \
    X(ANISOTROPY, samplerAnisotropy) X(BC, textureCompressionBC) X(ASTC_LDR, textureCompressionASTC_LDR) \
    X(ETC2, textureCompressionETC2) X(ALPHA_TO_ONE, alphaToOne) X(DEPTH_BOUNDS, depthBounds) \
    X(SAMPLE_RATE_SHADING, sampleRateShading) X(VERTEX_STORAGE_WRITES, vertexPipelineStoresAndAtomics) \
    X(FRAGMENT_STORAGE_WRITES, fragmentStoresAndAtomics) X(FULL_UINT32_INDEX, fullDrawIndexUint32) \
    X(INDEPENDENT_BLEND, independentBlend) X(LARGE_POINTS, largePoints)

#define DM_M4_DRAW_CORE_CAPS(X) \
    X(10, shaderFloat64) X(11, shaderInt64) X(22, shaderInt16) X(25, shaderImageGatherExtended) \
    X(28, shaderUniformBufferArrayDynamicIndexing) X(29, shaderSampledImageArrayDynamicIndexing) \
    X(30, shaderStorageBufferArrayDynamicIndexing) X(31, shaderStorageImageArrayDynamicIndexing) \
    X(32, shaderClipDistance) X(33, shaderCullDistance) X(34, imageCubeArray) \
    X(35, sampleRateShading) X(45, imageCubeArray) X(49, shaderStorageImageExtendedFormats) \
    X(52, sampleRateShading) X(55, shaderStorageImageReadWithoutFormat) X(56, shaderStorageImageWriteWithoutFormat)

#define DM_M4_DRAW_VK12_CAPS(X) X(9, shaderFloat16)

static VkResult failure(const char **step, const char *where, VkResult result)
{
    if (step) *step = where;
    return result;
}

static const VkSubgroupFeatureFlagBits subgroup_features[] = {
    VK_SUBGROUP_FEATURE_BASIC_BIT, VK_SUBGROUP_FEATURE_VOTE_BIT, VK_SUBGROUP_FEATURE_ARITHMETIC_BIT,
    VK_SUBGROUP_FEATURE_BALLOT_BIT, VK_SUBGROUP_FEATURE_SHUFFLE_BIT, VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT,
    VK_SUBGROUP_FEATURE_CLUSTERED_BIT, VK_SUBGROUP_FEATURE_QUAD_BIT,
};

static VkResult enable_capability(struct dm_m4_draw_requirements *out, VkShaderStageFlagBits stage,
                                  uint32_t cap, const char **step)
{
    switch (cap) {
    // Core capabilities with no additional feature bit (SPIR-V enums).
    case 0: case 1: case 40: case 43: case 44: case 46: case 47: case 50: case 51:
        break;
    case 5301: // ShaderNonUniform; the descriptor class supplies the feature below.
        break;
    case 5307: // SampledImageArrayNonUniformIndexing (includes separate samplers).
        out->vk12.descriptorIndexing = VK_TRUE;
        out->vk12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        out->vk12.descriptorBindingPartiallyBound = VK_TRUE;
        if (stage == VK_SHADER_STAGE_FRAGMENT_BIT) out->core.fragmentStoresAndAtomics = VK_TRUE;
        else if (stage != VK_SHADER_STAGE_COMPUTE_BIT) out->core.vertexPipelineStoresAndAtomics = VK_TRUE;
        break;
#define CAP_CASE(number, field) case number: out->core.field = VK_TRUE; break;
    DM_M4_DRAW_CORE_CAPS(CAP_CASE)
#undef CAP_CASE
#define CAP_CASE(number, field) case number: out->vk12.field = VK_TRUE; break;
    DM_M4_DRAW_VK12_CAPS(CAP_CASE)
#undef CAP_CASE
    case 61: case 62: case 63: case 64: case 65: case 66: case 67: case 68:
        out->subgroup_stages[cap - 61] |= stage;
        break;
    case 4427: out->vk11.shaderDrawParameters = VK_TRUE; break;
    case 70: // ShaderViewportIndex, used by the classic NVN VPRS lowering.
        if (stage != VK_SHADER_STAGE_VERTEX_BIT)
            return failure(step, "SPIR-V viewport index outside classic vertex stage", VK_ERROR_FEATURE_NOT_PRESENT);
        out->core.multiViewport = VK_TRUE;
        out->vk12.shaderOutputViewportIndex = VK_TRUE;
        break;
    case 5283: // MeshShadingEXT, also used by per-primitive fragment inputs
        out->mesh.meshShader = VK_TRUE;
        break;
    case 5291: // ShadingRateNV (FragmentSizeNV / InvocationsPerPixelNV)
        out->rate.shadingRateImage = VK_TRUE;
        break;
    case 5347: // PhysicalStorageBufferAddresses
        out->vk12.bufferDeviceAddress = VK_TRUE;
        break;
    case 5378:
        if (stage != VK_SHADER_STAGE_FRAGMENT_BIT)
            return failure(step, "SPIR-V pixel interlock outside fragment stage", VK_ERROR_INVALID_SHADER_NV);
        out->interlock.fragmentShaderPixelInterlock = VK_TRUE;
        break;
    default: return failure(step, "unsupported SPIR-V capability in staged replay", VK_ERROR_FEATURE_NOT_PRESENT);
    }
    return VK_SUCCESS;
}

VkResult dm_m4_draw_requirements_init(struct dm_m4_draw_requirements *out, uint32_t features,
                                     const struct dm_m4_draw_capabilities *caps, uint32_t count,
                                     const char **step)
{
    if (step) *step = NULL;
    if (!out || (count && !caps))
        return failure(step, "draw requirements arguments", VK_ERROR_INITIALIZATION_FAILED);
    if (features & ~(uint32_t)(DM_M4_DRAW_SHADING_RATE_IMAGE * 2u - 1u))
        return failure(step, "unrecognized draw device feature", VK_ERROR_FEATURE_NOT_PRESENT);
    *out = (struct dm_m4_draw_requirements){0};
    out->draw_features = features;
    out->core.robustBufferAccess = VK_TRUE;
    out->core.occlusionQueryPrecise = VK_TRUE; // NVN SAMPLES_PASSED is a count, not just visible/not-visible.
#define ENABLE_FEATURE(flag, field) out->core.field = (features & DM_M4_DRAW_##flag) != 0;
    DM_M4_DRAW_CORE_FLAGS(ENABLE_FEATURE)
#undef ENABLE_FEATURE
    for (uint32_t i = 0; i < count; ++i) {
        const struct dm_m4_draw_capabilities *c = &caps[i];
        if ((c->stage != VK_SHADER_STAGE_VERTEX_BIT && c->stage != VK_SHADER_STAGE_FRAGMENT_BIT &&
             c->stage != VK_SHADER_STAGE_COMPUTE_BIT && c->stage != VK_SHADER_STAGE_MESH_BIT_EXT &&
             c->stage != VK_SHADER_STAGE_TASK_BIT_EXT) ||
            (c->count && !c->values))
            return failure(step, "draw shader capability list", VK_ERROR_INITIALIZATION_FAILED);
        if (c->stage == VK_SHADER_STAGE_COMPUTE_BIT) out->compute = VK_TRUE;
        if (c->stage == VK_SHADER_STAGE_MESH_BIT_EXT || c->stage == VK_SHADER_STAGE_TASK_BIT_EXT)
            out->mesh.meshShader = VK_TRUE;
        if (c->stage == VK_SHADER_STAGE_TASK_BIT_EXT) out->mesh.taskShader = VK_TRUE;
        for (uint32_t n = 0; n < c->count; ++n) {
            VkResult r = enable_capability(out, c->stage, c->values[n], step);
            if (r != VK_SUCCESS) return r;
        }
    }
    uint32_t n = 0;
    out->extensions[n++] = VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME;
    if (features & DM_M4_DRAW_CONSERVATIVE_RASTER) out->extensions[n++] = VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME;
    out->rendering = (VkPhysicalDeviceDynamicRenderingFeaturesKHR){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR, .dynamicRendering = VK_TRUE,
    };
    void *chain = &out->rendering;
    if (features & DM_M4_DRAW_NEGATIVE_ONE_TO_ONE) {
        out->extensions[n++] = VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME;
        out->clip = (VkPhysicalDeviceDepthClipControlFeaturesEXT){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT,
            .pNext = chain, .depthClipControl = VK_TRUE,
        };
        chain = &out->clip;
    }
    if (out->interlock.fragmentShaderPixelInterlock) {
        out->extensions[n++] = VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME;
        out->interlock.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT;
        out->interlock.pNext = chain;
        chain = &out->interlock;
    }
    if (features & DM_M4_DRAW_VERTEX_DIVISOR) {
        out->extensions[n++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
        out->divisor = (VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
            .pNext = chain, .vertexAttributeInstanceRateDivisor = VK_TRUE,
        };
        chain = &out->divisor;
    }
    if (out->mesh.meshShader) {
        out->extensions[n++] = VK_EXT_MESH_SHADER_EXTENSION_NAME;
        out->mesh.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
        out->mesh.pNext = chain;
        chain = &out->mesh;
    }
    if ((features & DM_M4_DRAW_SHADING_RATE_IMAGE) || out->rate.shadingRateImage) {
        out->extensions[n++] = VK_NV_SHADING_RATE_IMAGE_EXTENSION_NAME;
        out->rate = (VkPhysicalDeviceShadingRateImageFeaturesNV){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADING_RATE_IMAGE_FEATURES_NV,
            .pNext = chain, .shadingRateImage = VK_TRUE,
        };
        chain = &out->rate;
    }
    if (out->vk11.shaderDrawParameters) {
        out->vk11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        out->vk11.pNext = chain;
        chain = &out->vk11;
    }
    if (out->vk12.shaderFloat16 || out->vk12.bufferDeviceAddress || out->vk12.descriptorIndexing ||
        out->vk12.shaderOutputViewportIndex) {
        out->vk12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        out->vk12.pNext = chain;
        chain = &out->vk12;
    }
    out->requirements = (struct dm_m4_device_requirements){
        .extension_count = n, .extensions = out->extensions,
        .enabled_features = &out->core, .device_create_next = chain,
    };
    return VK_SUCCESS;
}

void dm_m4_draw_support_init(struct dm_m4_shader_support *support)
{
    *support = (struct dm_m4_shader_support){0};
    for (size_t i = 0; i < sizeof support->render_scale / sizeof *support->render_scale; ++i)
        support->render_scale[i][0] = 1.0f;
}

enum { DRAW_MIPS = 32, PIPELINE_CACHE_SIZE = 8, PIPELINE_KEY_WORDS = 512 };
enum { COMPUTE_PIPELINE_CACHE_SIZE = 32 };
enum { NUMERIC_VARIANT_CACHE_SIZE = 128 };
static const VkImageUsageFlags rate_usage = VK_IMAGE_USAGE_SHADING_RATE_IMAGE_BIT_NV | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

struct draw_transfer {
    VkDeviceSize offset;
    size_t row, pitch, rows;
};

struct draw_image {
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
    VkDeviceSize allocation_size;
    VkFormat format;
    VkFormatFeatureFlags features;
    VkImageAspectFlags aspects;
    uint32_t width, height, depth, levels, array_layers;
    VkBool32 cube;
    VkComponentMapping components;
    // Render targets only. A nonzero token covers just these stored planes;
    // the owning image ended its fenced submission in TRANSFER_SRC_OPTIMAL.
    uint64_t target_token;
    VkImageAspectFlags target_stored;
};

struct draw_sampler {
    VkSampler handle;
    VkSamplerCreateInfo state;
};

struct draw_sample_key {
    uintptr_t address; // encoded backing, or the submission-local eager source
    uint64_t generation;
    uint32_t id, kind;
    size_t layer_bytes, layer_stride;
    struct dm_m4_draw_sample_layout layout;
};

struct draw_sample {
    struct draw_image image;
    // Borrowed for a miss in the current submission only. Clear on staging.
    const struct dm_m4_draw_sample_upload *upload;
    const struct dm_m4_draw_pixels *pixels; // NULL for a direct-to-staging materializer
    uint8_t *snapshot;
    size_t snapshot_size;
    struct draw_sample_key key;
    uint32_t hash, bucket;
    int resident;
    struct draw_transfer levels[DRAW_MIPS];
};

struct draw_storage_image {
    struct draw_image image;
    const struct dm_m4_draw_storage_image *source;
    void *writeback;
    struct draw_transfer transfer;
};

struct draw_texel_view {
    VkBufferView handle;
    VkBuffer buffer;
    VkFormat format;
    VkDeviceSize offset, range;
};

struct draw_binding {
    const void *data;
    void *writeback;
    size_t size;
    VkDescriptorBufferInfo buffer;
    VkDescriptorImageInfo image;
    VkFormat texel_format;
    int import_texel;
    VkBufferView texel_view;
    int storage_texel;
    int storage;
    int gmem_alias; // one-based index of the global memory range backing this binding
    uint32_t first_descriptor, descriptor_count;
    uint32_t bindless_map_binding; // associated layout binding + 1, zero for ordinary buffers
};

struct pipeline_key {
    VkShaderModule primary, ps, task;
    VkDescriptorSetLayout primary_set, ps_set, task_set;
    uint32_t count, words[PIPELINE_KEY_WORDS];
};

struct draw_pipeline {
    struct pipeline_key key;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    uint64_t used;
};

struct compute_pipeline {
    VkShaderModule shader;
    VkDescriptorSetLayout set;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    uint64_t used;
};

struct numeric_variant {
    VkShaderModule base, module;
    VkDescriptorSetLayout set;
    uint64_t used;
    uint32_t count;
    // Followed by count naturally aligned dm_m4_sample_type records.
};

struct sampled_types { uint32_t first, count; };

struct gmem_range {
    struct dm_m4_gmem_span span;
    VkDeviceSize offset, descriptor_alignment;
    VkBuffer imported;
    VkDeviceAddress imported_address;
    int dirty;
    int stage_only;
};

struct gmem_import {
    uintptr_t begin, end;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceAddress address;
    int widened; // this entire backing was proved committed and private
};

struct gmem_candidate {
    uintptr_t begin, end;
    size_t live_bytes;
    uint32_t first, last;
};

struct dm_m4_draw {
    const struct dm_m4_device *device;
#define DRAW_MEMBER(name) PFN_vk##name name;
    DM_M4_DRAW_VK(DRAW_MEMBER)
    DM_M4_COMPUTE_VK(DRAW_MEMBER)
    DM_M4_MESH_VK(DRAW_MEMBER)
#undef DRAW_MEMBER
    PFN_vkGetBufferDeviceAddress GetBufferDeviceAddress;
    PFN_vkGetMemoryHostPointerPropertiesEXT GetMemoryHostPointerPropertiesEXT;
    PFN_vkGetBufferMemoryRequirements2 GetBufferMemoryRequirements2;
    PFN_vkCmdBindShadingRateImageNV CmdBindShadingRateImageNV;
    PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties;
    PFN_vkGetPhysicalDeviceImageFormatProperties GetPhysicalDeviceImageFormatProperties;
    VkPhysicalDeviceLimits limits;
    VkPhysicalDeviceMemoryProperties memory;
    VkPhysicalDeviceFeatures features;
    VkPhysicalDeviceSubgroupProperties subgroup;
    VkPhysicalDeviceMeshShaderPropertiesEXT mesh_limits;
    VkExtent2D rate_max_extent;
    VkBool32 task_shader, mesh_shader;
    VkDescriptorSetLayout empty_set; // gap at set 1 on task + mesh draws without fragment
    uint32_t draw_features, max_vertex_divisor;
    VkBool32 pixel_interlock, shader_draw_parameters;
    VkBool32 shader_output_viewport_index;
    VkBool32 compute, shader_float16;
    VkBool32 sampled_nonuniform;
    VkCommandPool command_pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkQueryPool occlusion;
    VkResult queue_error;
    int in_flight;
    VkDescriptorPool descriptor_pool;
    uint32_t descriptor_counts[8];
    struct draw_binding *bindings;
    VkWriteDescriptorSet *writes;
    VkDescriptorImageInfo *array_infos;
    uint32_t array_info_count, array_info_capacity;
    uint32_t binding_capacity, write_capacity;
    VkBuffer buffer;
    VkBuffer texel_buffers[2]; // uniform and storage views of the same mapped allocation
    VkDeviceMemory buffer_memory;
    uint8_t *mapped;
    VkDeviceSize buffer_capacity;
    VkDeviceAddress buffer_address;
    struct gmem_range *gmem_ranges;
    uint32_t gmem_count, gmem_capacity;
    VkDeviceSize gmem_map_offset, gmem_log_offset;
    unsigned gmem_reported;
    VkDeviceSize gmem_reported_staged;
    int gmem_trace;
    int gmem_writes;
    int gmem_read_alias; // map has a host-visible compute fault word after its entries
    int gmem_read_protected; // this dispatch can fault on a separately staged writable view
    struct gmem_import *gmem_imports;
    uint32_t gmem_import_count, gmem_import_capacity;
    VkDeviceSize gmem_import_max_size;
    struct gmem_candidate *gmem_candidates;
    uint32_t gmem_candidate_capacity;
    uintptr_t gmem_private_begin, gmem_private_end;
    int coherent;
    struct draw_image colors[DM_M4_NVN_COLOR_TARGETS], depth;
    struct draw_image rate_image;
    struct draw_image blit_images[2];
    struct draw_sample *samples;
    uint32_t sample_count, sample_slots, sample_capacity;
    uint32_t *sample_lookup;
    uint32_t sample_lookup_capacity;
    VkDeviceSize sample_image_bytes;
    size_t sample_snapshot_bytes;
    struct draw_storage_image *storage_images;
    uint32_t storage_image_count, storage_image_capacity;
    struct draw_sampler *samplers;
    uint32_t sampler_count, sampler_capacity;
    struct draw_texel_view *texel_views;
    uint32_t texel_view_capacity;
    struct numeric_variant *numeric_variants[NUMERIC_VARIANT_CACHE_SIZE];
    struct dm_m4_sample_type *numeric_types;
    uint32_t numeric_variant_count, numeric_type_capacity;
    struct compute_pipeline compute_pipelines[COMPUTE_PIPELINE_CACHE_SIZE];
    struct draw_pipeline pipelines[PIPELINE_CACHE_SIZE];
    uint64_t serial;
};

static int32_t sampled_format_sign(VkFormat format);
static VkResult append_sampled_type(struct dm_m4_draw *d, const struct dm_m4_shader *shader,
                                    uint32_t *count, uint32_t binding, int32_t sign, const char **step);
static VkResult sampled_module(struct dm_m4_draw *d, const struct dm_m4_shader *shader,
                               struct sampled_types types, VkShaderModule *out, const char **step);

#include "m4_draw_resources.inc"
#include "m4_draw_pipeline.inc"
#include "m4_compute_resources.inc"
#include "m4_gmem_import.inc"
#include "m4_compute_gmem.inc"
#include "m4_draw_submit.inc"
#include "m4_compute_submit.inc"
#include "m4_draw_numeric.inc"
#include "m4_texture_blit.inc"

VkResult dm_m4_draw_create(struct dm_m4_draw **out, const struct dm_m4_device *device,
                           const struct dm_m4_draw_requirements *requirements, const char **step)
{
    if (step) *step = NULL;
    if (!out) return failure(step, "draw context output", VK_ERROR_INITIALIZATION_FAILED);
    *out = NULL;
    if (!device || !requirements || !device->device || !device->queue || !device->physical ||
        !device->get_device_proc || !device->get_instance_proc || !requirements->rendering.dynamicRendering)
        return failure(step, "draw context device", VK_ERROR_INITIALIZATION_FAILED);
    struct dm_m4_draw *d = calloc(1, sizeof *d);
    if (!d) return failure(step, "allocate draw context", VK_ERROR_OUT_OF_HOST_MEMORY);
    d->device = device;
    d->features = requirements->core;
    d->features.fullDrawIndexUint32 |= device->full_draw_index_uint32;
    d->draw_features = requirements->draw_features;
    d->pixel_interlock = requirements->interlock.fragmentShaderPixelInterlock;
    d->shader_draw_parameters = requirements->vk11.shaderDrawParameters;
    d->shader_output_viewport_index = requirements->vk12.shaderOutputViewportIndex;
    d->compute = requirements->compute;
    d->shader_float16 = requirements->vk12.shaderFloat16;
    d->sampled_nonuniform = requirements->vk12.shaderSampledImageArrayNonUniformIndexing &&
                            requirements->vk12.descriptorBindingPartiallyBound;
    d->mesh_shader = requirements->mesh.meshShader;
    d->task_shader = requirements->mesh.taskShader;
    const VkDevice vk = device->device;
    VkResult r;
    const char *where = "load staged Vulkan draw dispatch";
#define DRAW_LOAD(name) d->name = (PFN_vk##name)device->get_device_proc(vk, "vk" #name); \
    if (!d->name) { r = VK_ERROR_INITIALIZATION_FAILED; goto fail; }
    DM_M4_DRAW_VK(DRAW_LOAD)
    if (d->compute) {
        DM_M4_COMPUTE_VK(DRAW_LOAD)
        where = "check staged compute queue family";
        r = check_compute_queue(device);
        if (r != VK_SUCCESS) goto fail;
    }
    if (d->mesh_shader) {
        where = "load VK_EXT_mesh_shader draw dispatch";
        DM_M4_MESH_VK(DRAW_LOAD)
    }
    if (requirements->rate.shadingRateImage) {
        where = "load VK_NV_shading_rate_image draw dispatch";
        DRAW_LOAD(CmdBindShadingRateImageNV)
    }
    if (requirements->vk12.bufferDeviceAddress) {
        where = "load Vulkan buffer device address";
        DRAW_LOAD(GetBufferDeviceAddress)
        d->gmem_trace = GetEnvironmentVariableW(L"DM_VK_GMEM_STATS", NULL, 0) != 0;
        if (device->host_import_alignment) {
            where = "load Vulkan external host memory import";
            DRAW_LOAD(GetMemoryHostPointerPropertiesEXT)
            DRAW_LOAD(GetBufferMemoryRequirements2)
        }
    }
#undef DRAW_LOAD
    PFN_vkGetPhysicalDeviceProperties2 get_properties = (PFN_vkGetPhysicalDeviceProperties2)
        device->get_instance_proc(device->instance, "vkGetPhysicalDeviceProperties2");
    PFN_vkGetPhysicalDeviceMemoryProperties get_memory = (PFN_vkGetPhysicalDeviceMemoryProperties)
        device->get_instance_proc(device->instance, "vkGetPhysicalDeviceMemoryProperties");
    d->GetPhysicalDeviceFormatProperties = (PFN_vkGetPhysicalDeviceFormatProperties)
        device->get_instance_proc(device->instance, "vkGetPhysicalDeviceFormatProperties");
    d->GetPhysicalDeviceImageFormatProperties = (PFN_vkGetPhysicalDeviceImageFormatProperties)
        device->get_instance_proc(device->instance, "vkGetPhysicalDeviceImageFormatProperties");
    if (!get_properties || !get_memory || !d->GetPhysicalDeviceFormatProperties ||
        !d->GetPhysicalDeviceImageFormatProperties) {
        r = VK_ERROR_INITIALIZATION_FAILED;
        goto fail;
    }
    VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT divisor = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT,
    };
    d->subgroup = (VkPhysicalDeviceSubgroupProperties){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
        .pNext = (d->draw_features & DM_M4_DRAW_VERTEX_DIVISOR) ? &divisor : NULL,
    };
    if (d->mesh_shader) {
        d->mesh_limits.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT;
        d->mesh_limits.pNext = d->subgroup.pNext;
        d->subgroup.pNext = &d->mesh_limits;
    }
    VkPhysicalDeviceShadingRateImagePropertiesNV rate = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADING_RATE_IMAGE_PROPERTIES_NV,
        .pNext = d->subgroup.pNext,
    };
    if (d->CmdBindShadingRateImageNV) d->subgroup.pNext = &rate;
    VkPhysicalDeviceMaintenance3Properties import_limits = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES,
        .pNext = d->subgroup.pNext,
    };
    if (d->GetMemoryHostPointerPropertiesEXT) d->subgroup.pNext = &import_limits;
    VkPhysicalDeviceProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &d->subgroup,
    };
    get_properties(device->physical, &props);
    d->subgroup.pNext = NULL;
    d->mesh_limits.pNext = NULL;
    d->max_vertex_divisor = divisor.maxVertexAttribDivisor;
    d->limits = props.properties.limits;
    d->gmem_import_max_size = import_limits.maxMemoryAllocationSize;
    where = "check Vulkan 1.2 draw device";
    if (props.properties.apiVersion < VK_API_VERSION_1_2) {
        r = VK_ERROR_INCOMPATIBLE_DRIVER;
        goto fail;
    }
    if (d->CmdBindShadingRateImageNV) {
        where = "check NV shading-rate image tile, palette and coarse sample limits";
        // Replay uses all 16 palette slots and supports 4x4 fragments at one sample per pixel.
        if (!rate.shadingRateTexelSize.width || !rate.shadingRateTexelSize.height ||
            rate.shadingRatePaletteSize < DM_M4_RATE_PALETTE_SIZE || rate.shadingRateMaxCoarseSamples < 16) {
            r = VK_ERROR_FEATURE_NOT_PRESENT;
            goto fail;
        }
        where = "query transferable NV shading-rate image format";
        VkImageFormatProperties image_props;
        r = d->GetPhysicalDeviceImageFormatProperties(device->physical, VK_FORMAT_R8_UINT, VK_IMAGE_TYPE_2D,
                                                       VK_IMAGE_TILING_OPTIMAL, rate_usage, 0, &image_props);
        if (r != VK_SUCCESS) goto fail;
        if (!(image_props.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) {
            r = VK_ERROR_FORMAT_NOT_SUPPORTED;
            goto fail;
        }
        d->rate_max_extent = (VkExtent2D){image_props.maxExtent.width, image_props.maxExtent.height};
    }
    where = "check staged shader subgroup operations and compute warp width";
    for (size_t i = 0; i < sizeof subgroup_features / sizeof *subgroup_features; ++i) {
        VkShaderStageFlags stages = requirements->subgroup_stages[i];
        if (stages && !supports_subgroup(d, subgroup_features[i], stages)) {
            r = VK_ERROR_FEATURE_NOT_PRESENT;
            goto fail;
        }
    }
    get_memory(device->physical, &d->memory);
    const VkCommandPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT | VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        .queueFamilyIndex = device->queue_family,
    };
    where = "create draw command pool";
    r = d->CreateCommandPool(vk, &pool, NULL, &d->command_pool);
    if (r != VK_SUCCESS) goto fail;
    const VkCommandBufferAllocateInfo cmd = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = d->command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    where = "allocate draw command buffer";
    r = d->AllocateCommandBuffers(vk, &cmd, &d->cmd);
    if (r != VK_SUCCESS) goto fail;
    const VkFenceCreateInfo fence = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    where = "create draw fence";
    r = d->CreateFence(vk, &fence, NULL, &d->fence);
    if (r != VK_SUCCESS) goto fail;
    if (d->task_shader) {
        const VkDescriptorSetLayoutCreateInfo empty = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        where = "create empty fragment set for depth-only task and mesh draws";
        r = d->CreateDescriptorSetLayout(vk, &empty, NULL, &d->empty_set);
        if (r != VK_SUCCESS) goto fail;
    }
    *out = d;
    return VK_SUCCESS;
fail:
    dm_m4_draw_destroy(d);
    return failure(step, where, r);
}

void dm_m4_draw_destroy(struct dm_m4_draw *d)
{
    if (!d) return;
    const VkDevice vk = d->device->device;
    if (d->gmem_import_count) gmem_wait_before_releasing(d);
    else if (d->in_flight) (void)d->DeviceWaitIdle(vk);
    if (d->command_pool) d->DestroyCommandPool(vk, d->command_pool, NULL);
    if (d->fence) d->DestroyFence(vk, d->fence, NULL);
    if (d->occlusion) d->DestroyQueryPool(vk, d->occlusion, NULL);
    if (d->descriptor_pool) d->DestroyDescriptorPool(vk, d->descriptor_pool, NULL);
    for (size_t i = 0; i < PIPELINE_CACHE_SIZE; ++i) destroy_pipeline(d, &d->pipelines[i]);
    if (d->empty_set) d->DestroyDescriptorSetLayout(vk, d->empty_set, NULL);
    if (d->compute) {
        for (size_t i = 0; i < COMPUTE_PIPELINE_CACHE_SIZE; ++i) destroy_compute_pipeline(d, &d->compute_pipelines[i]);
        for (uint32_t i = 0; i < d->storage_image_capacity; ++i) destroy_image(d, &d->storage_images[i].image);
    }
    for (uint32_t i = 0; i < d->numeric_variant_count; ++i)
        destroy_numeric_variant(d, d->numeric_variants[i]);
    free(d->numeric_types);
    for (uint32_t i = 0; i < d->sampler_capacity; ++i) destroy_sampler(d, &d->samplers[i]);
    for (uint32_t i = 0; i < d->sample_capacity; ++i) {
        free(d->samples[i].snapshot);
        destroy_image(d, &d->samples[i].image);
    }
    destroy_image(d, &d->depth);
    destroy_image(d, &d->rate_image);
    for (size_t i = 0; i < sizeof d->blit_images / sizeof *d->blit_images; ++i) destroy_image(d, &d->blit_images[i]);
    for (size_t i = 0; i < sizeof d->colors / sizeof *d->colors; ++i) destroy_image(d, &d->colors[i]);
    destroy_buffer(d);
    for (uint32_t i = 0; i < d->gmem_import_count; ++i)
        gmem_destroy_import(d, &d->gmem_imports[i]);
    free(d->gmem_imports);
    free(d->gmem_candidates);
    free(d->samples);
    free(d->sample_lookup);
    free(d->texel_views);
    free(d->gmem_ranges);
    free(d->samplers);
    free(d->storage_images);
    free(d->bindings);
    free(d->writes);
    free(d->array_infos);
    free(d);
}
