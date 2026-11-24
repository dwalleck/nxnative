#include "m4_shader.h"
#include "../../gfx/nvn/dmsh_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// Little-endian disk ABI, written by win_nvn_shaders.py. The raw code is
// stamped so reflection from a different translator run cannot be paired with it.
struct reflection_header {
    char magic[8];
    uint8_t key[8];
    uint64_t spirv_hash;
    uint32_t spirv_size;
    uint32_t execution_model;
    uint32_t set;
    uint32_t binding_count;
    uint32_t c1_size;
    uint32_t capability_count;
    uint32_t frag_write_mask;
    uint32_t frag_out_map;
    uint32_t uses_frag_coord;
    uint32_t local[3];
    uint32_t shared;
    uint32_t flags;
};

struct reflection_binding {
    uint32_t binding, descriptor_type, count, source;
    uint32_t data[8];
};

_Static_assert(sizeof(struct reflection_header) == 80, "shader reflection header ABI");
_Static_assert(sizeof(struct reflection_binding) == 48, "shader binding ABI");
_Static_assert(sizeof(struct dm_m4_shader_support) == 1392, "translator support UBO ABI");
_Static_assert(VK_DESCRIPTOR_TYPE_SAMPLER == 0 && VK_DESCRIPTOR_TYPE_STORAGE_BUFFER == 7,
               "descriptor types indexed by the reflection layout");
enum {
    REFLECTION_CHECKED_GMEM = 1u,
    REFLECTION_UINT_SHIFT = 8, REFLECTION_SINT_SHIFT = 16, REFLECTION_INTEGER_OUTPUTS = 0xffff00u,
};

static int stage_info(VkShaderStageFlagBits stage, uint32_t *execution_model, uint32_t *set)
{
    switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT:   *execution_model = 0; *set = 0; return 1;
    case VK_SHADER_STAGE_FRAGMENT_BIT: *execution_model = 4; *set = 1; return 1;
    case VK_SHADER_STAGE_COMPUTE_BIT:  *execution_model = 5; *set = 0; return 1;
    case VK_SHADER_STAGE_TASK_BIT_EXT: *execution_model = DM_M4_SPV_TASK_EXT; *set = 2; return 1;
    case VK_SHADER_STAGE_MESH_BIT_EXT: *execution_model = DM_M4_SPV_MESH_EXT; *set = 0; return 1;
    default: return 0;
    }
}

static int file_extent(FILE *file, uint64_t size, int64_t offset)
{
    if (_fseeki64(file, 0, SEEK_END))
        return 0;
    const int64_t end = _ftelli64(file);
    return end >= 0 && (uint64_t)end == size && !_fseeki64(file, offset, SEEK_SET);
}

static VkResult open_library_files(const wchar_t *directory, const uint8_t key[8],
                                   FILE **reflection, FILE **spirv)
{
    static const wchar_t hex[] = L"0123456789abcdef";
    size_t length = wcslen(directory);
    const size_t suffix_length = 1 + 16 + sizeof(L".vkr") / sizeof(wchar_t);
    if (length > SIZE_MAX / sizeof(wchar_t) - suffix_length)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    wchar_t *path = malloc((length + suffix_length) * sizeof(*path));
    if (!path)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    memcpy(path, directory, length * sizeof(*path));
    if (path[length - 1] != L'/' && path[length - 1] != L'\\')
        path[length++] = L'\\';
    for (size_t i = 0; i < 8; ++i) {
        path[length++] = hex[key[i] >> 4];
        path[length++] = hex[key[i] & 15];
    }
    memcpy(path + length, L".vkr", sizeof(L".vkr"));
    if (_wfopen_s(reflection, path, L"rb") == 0) {
        memcpy(path + length, L".spv", sizeof(L".spv"));
        if (_wfopen_s(spirv, path, L"rb") == 0) {
            free(path);
            return VK_SUCCESS;
        }
        fclose(*reflection);
        *reflection = NULL;
    }
    free(path);
    return VK_ERROR_INITIALIZATION_FAILED;
}

static uint64_t spirv_hash(const uint32_t *words, size_t size)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    const uint8_t *bytes = (const uint8_t *)words;
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * UINT64_C(0x100000001b3);
    return hash;
}

static int uniform_slot(uint32_t slot)
{
    return slot >= 3 && slot < 3 + DMSH_CBSIZE_SLOTS;
}

static int table_entry(uint32_t offset, uint32_t base, uint32_t slots)
{
    return offset >= base && offset - base < DMSH_C0_ROW * slots &&
           (offset - base) % DMSH_C0_ROW == 0;
}

static int storage_address(const uint32_t *data, uint32_t model)
{
    const uint32_t slot = data[0], cb_slot = data[1], offset = data[2], flags = data[3];
    if (cb_slot == DMSH_GMEM_MAP_CB_SLOT)
        return (offset == 0 && flags == 0) ||
               (model == 5 && offset == DMSH_GMEM_LOG_CB_OFFSET && flags == 1);
    if (flags > 3 || offset > UINT32_MAX / 4 - 1)
        return 0;
    if ((flags & 2) && (slot >= DMSH_SBLEN_SLOTS || cb_slot == 0))
        return 0;
    if (uniform_slot(cb_slot))
        return 1;
    if (cb_slot == DMSH_PUSH_BANK)
        return offset <= (DMSH_PUSH_BLOCKS * DMSH_PUSH_SIZE - 8) / 4;
    if (cb_slot != 0)
        return 0;
    const uint32_t base = model == 0 ? DMSH_C0_SSBO_VS : model == 4 ? DMSH_C0_SSBO_PS :
                          model == DM_M4_SPV_TASK_EXT ? DMSH_C0_SSBO_TS :
                          model == DM_M4_SPV_MESH_EXT ? DMSH_C0_SSBO_MS : DMSH_C0_SSBO_CS;
    return table_entry(offset * 4, base, DMSH_C0_SSBO_SLOTS) ||
           (model == 5 && table_entry(offset * 4, DMSH_C0_CS_UBO, DMSH_C0_CS_UBO_SLOTS));
}

static int image_cb_slot(uint32_t slot)
{
    return slot == 2 || uniform_slot(slot) || slot == DMSH_PUSH_BANK;
}

static int image_recipe(const struct reflection_binding *disk, struct dm_m4_shader_binding *binding)
{
    const uint32_t *d = disk->data;
    const uint32_t type = d[2] & 0xff, mode = d[1] >> 28;
    if (type > 5 || d[2] > 0x7ff || d[3] > VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 ||
        d[4] > 15 || d[6] > 1 || d[7] > 1 ||
        (d[5] != UINT32_MAX && (d[5] >= DMSH_TBIAS_SLOTS || disk->count > DMSH_TBIAS_SLOTS - d[5])) ||
        (d[7] && (type != 3 || d[5] == UINT32_MAX)))
        return 0;
    if (mode > 4 || (mode == 4 && (d[0] != 0 || d[1] != UINT32_C(0x40000000) || disk->count < 2)))
        return 0;
    if (mode != 4 && d[0] != UINT32_MAX) {
        const uint32_t sampler_slot = d[0] >> 16;
        if (!image_cb_slot(d[0] & 0xffff) || (sampler_slot && !image_cb_slot(sampler_slot - 1)))
            return 0;
    }
    VkDescriptorType descriptor_type;
    if (disk->source == DM_M4_SHADER_IMAGE) {
        if (type == 0 || d[6])
            return 0;
        descriptor_type = type == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    } else if (type == 0) {
        descriptor_type = VK_DESCRIPTOR_TYPE_SAMPLER;
    } else if (type == 2) {
        descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    } else {
        descriptor_type = d[6] ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    if (disk->descriptor_type != (uint32_t)descriptor_type)
        return 0;
    if (mode == 4) {
        const uint32_t capacity = descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLER ?
            DM_M4_BINDLESS_SAMPLER_CAPACITY : DM_M4_BINDLESS_IMAGE_CAPACITY;
        if (disk->source != DM_M4_SHADER_TEXTURE || disk->count != capacity ||
            (descriptor_type != VK_DESCRIPTOR_TYPE_SAMPLER && descriptor_type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) ||
            d[5] != UINT32_MAX || d[7])
            return 0;
    }
    binding->image.cbuf_slot = d[0] == UINT32_MAX ? -1 : (int32_t)d[0];
    binding->image.handle = d[1];
    binding->image.type = d[2];
    binding->image.format = (VkFormat)d[3];
    binding->image.flags = d[4];
    binding->image.tbx = d[5] == UINT32_MAX ? -1 : (int32_t)d[5];
    binding->image.separate = d[6];
    binding->image.row_bias = d[7];
    binding->image.atomic_word = VK_FALSE;
    return 1;
}

static int binding_recipe(const struct reflection_header *header, const struct reflection_binding *disk,
                          struct dm_m4_shader_binding *binding, VkDescriptorSetLayoutBinding *layout)
{
    // Compact layouts may use binding zero for an ordinary resource when
    // dead-variable elimination removed the stage's support UBO.
    if (!disk->count || disk->descriptor_type > VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
        disk->source > DM_M4_SHADER_IMAGE || (disk->source == DM_M4_SHADER_SUPPORT && disk->binding != 0))
        return 0;
    binding->binding = disk->binding;
    binding->descriptor_type = (VkDescriptorType)disk->descriptor_type;
    binding->count = disk->count;
    binding->source = (enum dm_m4_shader_binding_source)disk->source;
    const uint32_t *d = disk->data;
    switch (binding->source) {
    case DM_M4_SHADER_SUPPORT:
        if (disk->count != 1 || disk->descriptor_type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
            return 0;
        break;
    case DM_M4_SHADER_CBUF:
        if (disk->count != 1 || disk->descriptor_type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
            (d[0] > 2 && !uniform_slot(d[0]) && (d[0] < DMSH_PUSH_BANK || d[0] > DMSH_SBLEN_BANK)) ||
            (d[0] == 1 && (header->c1_size == 0 || header->c1_size < d[1])))
            return 0;
        binding->cbuf.slot = d[0];
        binding->cbuf.fixed_size = d[1];
        break;
    case DM_M4_SHADER_SBUF: {
        if (disk->count != 1 || disk->descriptor_type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
            return 0;
        if (d[1] == DM_M4_BINDLESS_MAP_BANK) {
            const uint32_t words = d[4] / 4u;
            if (d[2] == UINT32_MAX || d[3] != 1 || d[6] || d[7] ||
                (d[0] != UINT32_MAX ? (!words || words > 32 || d[4] % 4u || !d[5] ||
                                      (words < 32 && (d[5] >> words)))
                                   : (d[4] || d[5])))
                return 0;
            binding->source = DM_M4_SHADER_BINDLESS_MAP;
            binding->sbuf.record_stride = d[4];
            binding->sbuf.record_words = d[5];
        } else if (!storage_address(d, header->execution_model)) return 0;
        binding->sbuf.slot = d[0];
        binding->sbuf.cb_slot = d[1];
        binding->sbuf.cb_offset = d[2];
        binding->sbuf.flags = d[3];
        if (d[1] == DMSH_GMEM_MAP_CB_SLOT)
            binding->source = d[2] == 0 ? DM_M4_SHADER_GMEM_MAP : DM_M4_SHADER_GMEM_LOG;
        break;
    }
    case DM_M4_SHADER_TEXTURE:
    case DM_M4_SHADER_IMAGE:
        if (!image_recipe(disk, binding))
            return 0;
        break;
    default: return 0;
    }
    *layout = (VkDescriptorSetLayoutBinding){
        .binding = disk->binding,
        .descriptorType = binding->descriptor_type,
        .descriptorCount = disk->count,
    };
    return 1;
}

static int bindless_recipes(const struct dm_m4_shader_binding *bindings, uint32_t count)
{
    uint32_t maps = 0, arrays = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const struct dm_m4_shader_binding *map = &bindings[i];
        arrays += (uint32_t)dm_m4_shader_direct(map);
        if (map->source != DM_M4_SHADER_BINDLESS_MAP) continue;
        ++maps;
        int array = 0, material = map->sbuf.slot == UINT32_MAX;
        for (uint32_t j = 0; j < count; ++j) {
            const struct dm_m4_shader_binding *b = &bindings[j];
            if (b->binding == map->sbuf.cb_offset) array = dm_m4_shader_direct(b);
            if (b->binding == map->sbuf.slot)
                material = b->source == DM_M4_SHADER_SBUF && b->count == 1 && !b->sbuf.flags;
            if (j < i && b->source == DM_M4_SHADER_BINDLESS_MAP && b->sbuf.cb_offset == map->sbuf.cb_offset)
                return 0;
        }
        if (!array || !material) return 0;
    }
    return maps == arrays;
}

static int descriptor_limits(const VkPhysicalDeviceLimits *limits, const uint64_t count[8])
{
    const uint64_t samplers = count[VK_DESCRIPTOR_TYPE_SAMPLER] + count[VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER];
    const uint64_t sampled = count[VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER] + count[VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE] +
                             count[VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER];
    const uint64_t storage = count[VK_DESCRIPTOR_TYPE_STORAGE_IMAGE] + count[VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER];
    const uint64_t ubos = count[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER], sbos = count[VK_DESCRIPTOR_TYPE_STORAGE_BUFFER];
    return samplers <= limits->maxPerStageDescriptorSamplers && samplers <= limits->maxDescriptorSetSamplers &&
           sampled <= limits->maxPerStageDescriptorSampledImages && sampled <= limits->maxDescriptorSetSampledImages &&
           storage <= limits->maxPerStageDescriptorStorageImages && storage <= limits->maxDescriptorSetStorageImages &&
           ubos <= limits->maxPerStageDescriptorUniformBuffers && ubos <= limits->maxDescriptorSetUniformBuffers &&
           sbos <= limits->maxPerStageDescriptorStorageBuffers && sbos <= limits->maxDescriptorSetStorageBuffers &&
           sampled + storage + ubos + sbos <= limits->maxPerStageResources;
}

static int stage_limits(const VkPhysicalDeviceLimits *limits, const struct reflection_header *h)
{
    if (h->set >= limits->maxBoundDescriptorSets)
        return 0;
    if (h->execution_model != 5)
        return 1;
    uint32_t invocations = 1;
    for (size_t i = 0; i < 3; ++i) {
        if (!h->local[i] || h->local[i] > limits->maxComputeWorkGroupSize[i] ||
            invocations > limits->maxComputeWorkGroupInvocations / h->local[i])
            return 0;
        invocations *= h->local[i];
    }
    return h->shared <= limits->maxComputeSharedMemorySize;
}

static int image_property_string(const uint32_t *words, uint32_t count, const char *value)
{
    const size_t size = strlen(value) + 1;
    return count == (size + 3) / 4 && !memcmp(words, value, size);
}

static int unfiltered_image_op(const uint32_t *inst, uint32_t n, uint32_t op)
{
    if (op == 10) { // OpExtension: none of these add filtering operations or operands.
        static const char *const known[] = {
            "SPV_EXT_descriptor_indexing", "SPV_EXT_fragment_shader_interlock",
            "SPV_EXT_mesh_shader", "SPV_EXT_shader_viewport_index_layer",
            "SPV_EXT_demote_to_helper_invocation", "SPV_KHR_16bit_storage", "SPV_KHR_8bit_storage",
            "SPV_KHR_float_controls", "SPV_KHR_physical_storage_buffer",
            "SPV_KHR_shader_draw_parameters", "SPV_KHR_storage_buffer_storage_class",
            "SPV_KHR_terminate_invocation", "SPV_KHR_vulkan_memory_model", "SPV_NV_shading_rate",
        };
        for (size_t i = 0; i < sizeof known / sizeof *known; ++i)
            if (image_property_string(inst + 1, n - 1, known[i])) return 1;
        return 0;
    }
    if (op == 11) // OpExtInstImport: the GLSL standard set has no image operations.
        return n > 2 && image_property_string(inst + 2, n - 2, "GLSL.std.450");
    if ((op >= 87 && op <= 94) || (op >= 305 && op <= 312))
        return 0; // OpImage[ Sparse ]Sample*, including projected samples.
    // Assigned core opcodes through SPIR-V 1.6, plus the listed stage extensions.
    // Fetch, gather, query, storage operations and image atomics do not filter.
    // Do not assume a new opcode (e.g. an extension footprint) is safe.
    return op <= 8 || op == 12 || (op >= 14 && op <= 17) ||
           (op >= 19 && op <= 39) || (op >= 41 && op <= 46) || (op >= 48 && op <= 52) ||
           (op >= 54 && op <= 57) || (op >= 59 && op <= 75) || (op >= 77 && op <= 84) ||
           (op >= 86 && op <= 107) || (op >= 109 && op <= 124) || (op >= 126 && op <= 152) ||
           (op >= 154 && op <= 191) || (op >= 194 && op <= 205) || (op >= 207 && op <= 215) ||
           (op >= 218 && op <= 221) || (op >= 224 && op <= 225) || (op >= 227 && op <= 242) ||
           (op >= 245 && op <= 257) || (op >= 259 && op <= 271) || (op >= 274 && op <= 288) ||
           (op >= 291 && op <= 366) || (op >= 400 && op <= 403) ||
           op == 4416 || op == 5294 || op == 5295 || op == 5364 || op == 5365 || op == 5380 || op == 5381;
}

static int atomic_word_type(const uint32_t *code, size_t variable_at, uint32_t pointer, VkFormat format)
{
    for (size_t i = 5; i < variable_at; i += code[i] >> 16) {
        const uint32_t *ptr = code + i;
        if (ptr[0] != ((4u << 16) | 32u) || ptr[1] != pointer || ptr[2] != 0)
            continue; // OpTypePointer UniformConstant
        for (size_t j = 5; j < i; j += code[j] >> 16) {
            const uint32_t *image = code + j;
            if (image[0] == ((9u << 16) | 25u) && image[1] == ptr[3]) { // OpTypeImage
                const uint32_t word = format == VK_FORMAT_R32_UINT ? 33u : 24u; // R32ui or R32i
                return image[7] == 2 && image[8] == word; // storage, not sampled
            }
        }
        break;
    }
    return 0;
}

static void mark_atomic_word(struct dm_m4_shader_binding *bindings, uint32_t count, uint32_t set,
                             const uint32_t *code, size_t globals_end, uint32_t variable)
{
    uint32_t binding = UINT32_MAX, descriptor_set = UINT32_MAX;
    size_t variable_at = 0;
    for (size_t i = 5; i < globals_end; i += code[i] >> 16) {
        const uint32_t *inst = code + i, n = inst[0] >> 16, op = inst[0] & 0xffffu;
        if (op == 71 && n == 4 && inst[1] == variable) { // OpDecorate
            if (inst[2] == 33) binding = inst[3];
            if (inst[2] == 34) descriptor_set = inst[3];
        } else if (op == 59 && n == 4 && inst[2] == variable && inst[3] == 0) {
            variable_at = i; // OpVariable UniformConstant, not an access chain or copy.
        }
    }
    if (!variable_at || descriptor_set != set || binding == UINT32_MAX) return;
    for (uint32_t i = 0; i < count && bindings[i].binding <= binding; ++i) {
        struct dm_m4_shader_binding *b = &bindings[i];
        if (b->binding == binding && b->count == 1 && b->source == DM_M4_SHADER_IMAGE &&
            b->descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE &&
            (b->image.format == VK_FORMAT_R32_UINT || b->image.format == VK_FORMAT_R32_SINT) &&
            atomic_word_type(code, variable_at, code[variable_at + 1], b->image.format))
            b->image.atomic_word = VK_TRUE;
    }
}

static int shader_properties(struct dm_m4_shader *shader, uint32_t model, uint32_t set,
                             struct dm_m4_shader_binding *bindings, uint32_t binding_count,
                             const uint32_t *code, size_t words)
{
    enum {
        VERTEX = 0, FRAGMENT = 4, COMPUTE = 5, INPUT = 1,
        OP_CAPABILITY = 17, OP_FUNCTION = 54, OP_VARIABLE = 59, OP_IMAGE_TEXEL_POINTER = 60,
        OP_LOAD = 61, OP_DECORATE = 71,
        OP_MODULE_PROCESSED = 330, BUILTIN = 11, VIEWPORT_INDEX = 10, SHADER_VIEWPORT_INDEX = 70,
        OP_IMAGE_QUERY_FORMAT = 101, OP_IMAGE_QUERY_SAMPLES = 107,
    };
    static const char read_alias[] = "nxnative.gmem.read-alias.v1";
    static const char vprs_viewport[] = "nxnative.vprs.viewport.v1";
    uint32_t viewport = 0, viewport_input = 0, last_atomic_image = 0;
    size_t globals_end = 0;
    int viewport_cap = 0;
    const int queries = model == VERTEX || model == FRAGMENT || (model == COMPUTE && shader->gmem == 1u);
    shader->unfiltered_image_ops = code[1] >= 0x00010000u && code[1] <= 0x00010600u && !(code[1] & 255u);
    for (size_t i = 5; i < words;) {
        const uint32_t *inst = code + i, n = inst[0] >> 16, op = inst[0] & 0xffffu;
        if (!n || n > words - i) return 0;
        if (shader->unfiltered_image_ops && !unfiltered_image_op(inst, n, op))
            shader->unfiltered_image_ops = VK_FALSE;
        if (op == OP_FUNCTION && !globals_end)
            globals_end = i;
        if (op == OP_IMAGE_TEXEL_POINTER && n == 6 && globals_end && inst[3] != last_atomic_image) {
            mark_atomic_word(bindings, binding_count, set, code, globals_end, inst[3]);
            last_atomic_image = inst[3];
        }
        if (queries && op >= OP_IMAGE_QUERY_FORMAT && op <= OP_IMAGE_QUERY_SAMPLES)
            shader->queries_images = 1;
        if (model == VERTEX) {
            if (op == OP_CAPABILITY && n == 2 && inst[1] == SHADER_VIEWPORT_INDEX)
                viewport_cap = 1;
            if (op == OP_MODULE_PROCESSED && n == 1 + (sizeof vprs_viewport + 3) / 4 &&
                !memcmp(inst + 1, vprs_viewport, sizeof vprs_viewport))
                shader->vprs_viewport = 1;
        } else if (model == FRAGMENT) {
            // The translator declares this built-in as a standalone Input
            // scalar, accessed with a direct OpLoad.
            if (op == OP_DECORATE && n == 4 && inst[2] == BUILTIN && inst[3] == VIEWPORT_INDEX)
                viewport = inst[1];
            if (viewport && op == OP_VARIABLE && n >= 4 && inst[2] == viewport && inst[3] == INPUT)
                viewport_input = viewport;
            if (viewport_input && op == OP_LOAD && n >= 4 && inst[3] == viewport_input) {
                shader->reads_viewport_index = 1;
            }
        } else if (model == COMPUTE && shader->gmem == 1u && op == OP_MODULE_PROCESSED &&
                   n == 1 + (sizeof read_alias + 3) / 4 && !memcmp(inst + 1, read_alias, sizeof read_alias)) {
            shader->gmem_read_alias = 1;
        }
        i += n;
    }
    return !shader->vprs_viewport || viewport_cap;
}

static int mesh_outputs(struct dm_m4_shader *shader, const uint32_t *code, size_t words)
{
    enum { OP_EXECUTION_MODE = 16, OP_VARIABLE = 59, OUTPUT_VERTICES = 26, OUTPUT_POINTS = 27,
           OUTPUT_LINES_EXT = 5269, OUTPUT_PRIMITIVES_EXT = 5270, OUTPUT_TRIANGLES_EXT = 5298,
           TASK_PAYLOAD_WORKGROUP_EXT = 5402 };
    // Mesh libraries have exactly one entry point. Topology comes from its
    // execution modes; the task payload is present only in a taskful mesh.
    for (size_t i = 5; i < words;) {
        const uint32_t n = code[i] >> 16, op = code[i] & 0xffffu;
        if (!n || n > words - i) return 0;
        if (op == OP_EXECUTION_MODE && n >= 3) {
            switch (code[i + 2]) {
            case OUTPUT_POINTS: shader->mesh_topology = 1; break;
            case OUTPUT_LINES_EXT: shader->mesh_topology = 2; break;
            case OUTPUT_TRIANGLES_EXT: shader->mesh_topology = 3; break;
            case OUTPUT_VERTICES: if (n == 4) shader->mesh_vertices = code[i + 3]; break;
            case OUTPUT_PRIMITIVES_EXT: if (n == 4) shader->mesh_primitives = code[i + 3]; break;
            }
        } else if (op == OP_VARIABLE && n >= 4 && code[i + 3] == TASK_PAYLOAD_WORKGROUP_EXT) {
            shader->mesh_has_task = 1;
        }
        i += n;
    }
    return shader->mesh_topology && shader->mesh_vertices && shader->mesh_primitives;
}

size_t dm_m4_shader_specialize_point_size(uint32_t *code, size_t words)
{
    enum {
        OP_TYPE_FLOAT = 22, OP_TYPE_POINTER = 32, OP_TYPE_FIRST = 19, OP_TYPE_LAST = 39,
        OP_CONSTANT = 43, OP_SPEC_CONSTANT = 50, OP_FUNCTION = 54, OP_VARIABLE = 59,
        OP_STORE = 62, OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66,
        OP_DECORATE = 71, OP_MEMBER_DECORATE = 72, DEC_SPEC_ID = 1, DEC_BUILTIN = 11,
        BUILTIN_POINT_SIZE = 1, STORAGE_OUTPUT = 3,
    };
    if (words < 5 || !code[3] || code[3] >= 0x3fffffu) return 0;
    const uint32_t spec = code[3], one_bits = UINT32_C(0x3f800000);
    uint32_t point = 0, block = 0, member = 0, block_ptr = 0, block_out = 0, f32 = 0, one = 0;
    uint32_t member_indices[2] = {0};
    size_t types = 0, functions = 0, matches = 0;
    uint32_t previous_point_ptr = 0;
    for (size_t i = 5; i < words;) {
        const uint32_t *inst = code + i, n = inst[0] >> 16, op = inst[0] & 0xffffu;
        if (!n || n > words - i) return 0;
        if (op == OP_DECORATE && n == 4) {
            if (inst[2] == DEC_BUILTIN && inst[3] == BUILTIN_POINT_SIZE) point = inst[1];
            if (inst[2] == DEC_SPEC_ID && inst[3] == DM_M4_POINT_SIZE_SPEC_ID) return 0;
        } else if (op == OP_MEMBER_DECORATE && n == 5 && inst[3] == DEC_BUILTIN && inst[4] == BUILTIN_POINT_SIZE) {
            block = inst[1];
            member = inst[2];
        }
        if (!types && op >= OP_TYPE_FIRST && op <= OP_TYPE_LAST) {
            if (!point && !block) return words;
            types = i;
        }
        if (op == OP_TYPE_FLOAT && n == 3 && inst[2] == 32) f32 = inst[1];
        if (op == OP_TYPE_POINTER && n == 4 && block && inst[2] == STORAGE_OUTPUT && inst[3] == block)
            block_ptr = inst[1];
        if (op == OP_VARIABLE && n >= 4 && block_ptr && inst[1] == block_ptr && inst[3] == STORAGE_OUTPUT)
            block_out = inst[2];
        if (op == OP_CONSTANT && n == 4) {
            if (inst[1] == f32 && inst[3] == one_bits) one = inst[2];
            if (block && inst[1] != f32 && inst[3] == member) {
                member_indices[1] = member_indices[0];
                member_indices[0] = inst[2];
            }
        }
        if (op == OP_FUNCTION && !functions) {
            if (!types || !f32 || !one) return words;
            functions = i;
        }
        if (op == OP_STORE && n >= 3 && one && inst[2] == one &&
            (inst[1] == point || inst[1] == previous_point_ptr)) {
            code[i + 2] = spec;
            ++matches;
        }
        // The translator emits the access chain immediately before a default
        // store: [member] into gl_PerVertex, or [vertex] into a mesh array.
        previous_point_ptr = 0;
        if (n == 5 && (op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN) &&
            (inst[3] == point || (inst[3] == block_out &&
             (inst[4] == member_indices[0] || inst[4] == member_indices[1]))))
            previous_point_ptr = inst[2];
        i += n;
    }
    if (!matches) return words;
    if (!functions) return 0;
    const uint32_t decl[] = {(4u << 16) | OP_SPEC_CONSTANT, f32, spec, one_bits};
    const uint32_t decorate[] = {(4u << 16) | OP_DECORATE, spec, DEC_SPEC_ID, DM_M4_POINT_SIZE_SPEC_ID};
    memmove(code + functions + 8, code + functions, (words - functions) * sizeof(*code));
    memmove(code + types + 4, code + types, (functions - types) * sizeof(*code));
    memcpy(code + types, decorate, sizeof(decorate));
    memcpy(code + functions + 4, decl, sizeof(decl));
    code[3]++;
    return words + DM_M4_POINT_SIZE_EXTRA_WORDS;
}

VkResult dm_m4_shader_load(const struct dm_m4_device *device, const wchar_t *directory,
                           const uint8_t key[8], VkShaderStageFlagBits expected_stage,
                           struct dm_m4_shader *out)
{
    if (!out)
        return VK_ERROR_INITIALIZATION_FAILED;
    *out = (struct dm_m4_shader){0};
    uint32_t model, set;
    if (!device || !device->device || !device->physical || !device->get_device_proc ||
        !device->get_instance_proc || !directory || !*directory || !key)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!stage_info(expected_stage, &model, &set))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    PFN_vkGetPhysicalDeviceProperties get_properties = (PFN_vkGetPhysicalDeviceProperties)
        device->get_instance_proc(device->instance, "vkGetPhysicalDeviceProperties");
    PFN_vkGetDescriptorSetLayoutSupport layout_support = (PFN_vkGetDescriptorSetLayoutSupport)
        device->get_device_proc(device->device, "vkGetDescriptorSetLayoutSupport");
    PFN_vkCreateDescriptorSetLayout create_layout = (PFN_vkCreateDescriptorSetLayout)
        device->get_device_proc(device->device, "vkCreateDescriptorSetLayout");
    PFN_vkCreateShaderModule create_module = (PFN_vkCreateShaderModule)
        device->get_device_proc(device->device, "vkCreateShaderModule");
    if (!get_properties || !layout_support || !create_layout || !create_module)
        return VK_ERROR_INITIALIZATION_FAILED;

    FILE *reflection = NULL, *spirv = NULL;
    VkResult result = open_library_files(directory, key, &reflection, &spirv);
    if (result != VK_SUCCESS)
        return result;
    struct dm_m4_shader shader = {0};
    VkDescriptorSetLayoutBinding *layout = NULL;
    VkDescriptorBindingFlags *array_flags = NULL;
    uint32_t *code = NULL;
    struct reflection_header h;
    result = VK_ERROR_INVALID_SHADER_NV;
    if (fread(&h, sizeof(h), 1, reflection) != 1 || memcmp(h.magic, "DMVKREF1", 8) ||
        memcmp(h.key, key, sizeof(h.key)) || h.execution_model != model || h.set != set ||
        (h.flags & ~(uint32_t)(REFLECTION_CHECKED_GMEM | REFLECTION_INTEGER_OUTPUTS)) ||
        (model != 4 && (h.flags & REFLECTION_INTEGER_OUTPUTS)) ||
        ((h.flags >> REFLECTION_UINT_SHIFT) & (h.flags >> REFLECTION_SINT_SHIFT) & 0xffu) ||
        h.spirv_size < 20 || h.spirv_size % 4 || h.capability_count == 0 ||
        h.capability_count > h.spirv_size / 8 || h.binding_count > h.spirv_size / 16 ||
        h.c1_size % 4 || h.uses_frag_coord > 1 ||
        (model != 4 && (h.frag_write_mask || h.frag_out_map || h.uses_frag_coord)) ||
        ((model == DM_M4_SPV_TASK_EXT || model == DM_M4_SPV_MESH_EXT) ?
            (!h.local[0] || !h.local[1] || !h.local[2]) :
            (model != 5 && (h.local[0] || h.local[1] || h.local[2] || h.shared))))
        goto done;
    const size_t binding_bytes = (size_t)h.binding_count * sizeof(struct dm_m4_shader_binding);
    const size_t capability_bytes = (size_t)h.capability_count * sizeof(uint32_t);
    const size_t reflection_size = sizeof(h) + (size_t)h.binding_count * sizeof(struct reflection_binding) +
                                   capability_bytes + h.c1_size;
    if (!file_extent(reflection, reflection_size, sizeof(h)) ||
        !file_extent(spirv, h.spirv_size, 0))
        goto done;

    VkPhysicalDeviceProperties properties;
    get_properties(device->physical, &properties);
    if (!stage_limits(&properties.limits, &h) || h.binding_count > properties.limits.maxPerStageResources +
        (uint64_t)properties.limits.maxPerStageDescriptorSamplers) {
        result = VK_ERROR_FEATURE_NOT_PRESENT;
        goto done;
    }
    struct dm_m4_shader_binding *bindings = malloc(binding_bytes + capability_bytes + h.c1_size);
    shader.bindings = bindings;
    if (h.binding_count)
        layout = malloc((size_t)h.binding_count * sizeof(*layout));
    code = malloc((size_t)h.spirv_size + (model == 0 || model == DM_M4_SPV_MESH_EXT ?
                                        DM_M4_POINT_SIZE_EXTRA_WORDS * sizeof(*code) : 0));
    if (!bindings || (h.binding_count && !layout) || !code) {
        result = VK_ERROR_OUT_OF_HOST_MEMORY;
        goto done;
    }
    uint32_t *capabilities = (uint32_t *)((uint8_t *)bindings + binding_bytes);
    uint8_t *c1 = (uint8_t *)capabilities + capability_bytes;
    uint64_t descriptors[8] = {0};
    uint32_t needs_banks = 0, has_banks = 0;
    int direct = 0, retain_numeric = 0;
    for (uint32_t i = 0; i < h.binding_count; ++i) {
        struct reflection_binding disk;
        if (fread(&disk, sizeof(disk), 1, reflection) != 1 || (i && disk.binding <= bindings[i - 1].binding))
            goto done;
        if (!binding_recipe(&h, &disk, &bindings[i], &layout[i])) {
            result = VK_ERROR_FEATURE_NOT_PRESENT;
            goto done;
        }
        layout[i].stageFlags = expected_stage;
        descriptors[disk.descriptor_type] += disk.count;
        direct |= dm_m4_shader_direct(&bindings[i]);
        retain_numeric |= disk.descriptor_type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER &&
                          disk.count == 1 && !(bindings[i].image.type & 0x400u);
        if (bindings[i].source == DM_M4_SHADER_GMEM_MAP || bindings[i].source == DM_M4_SHADER_GMEM_LOG) {
            const uint32_t bit = bindings[i].source == DM_M4_SHADER_GMEM_MAP ? 1u : 2u;
            if (shader.gmem & bit) goto done;
            shader.gmem |= bit;
        }
        if (disk.source == DM_M4_SHADER_CBUF) {
            const uint32_t required_size = disk.data[0] == 1 ? h.c1_size : disk.data[1];
            if (required_size > properties.limits.maxUniformBufferRange) {
                result = VK_ERROR_FEATURE_NOT_PRESENT;
                goto done;
            }
            has_banks |= UINT32_C(1) << disk.data[0];
        } else if (disk.source == DM_M4_SHADER_SBUF && (disk.data[3] & 2)) {
            needs_banks |= UINT32_C(1) << DMSH_SBLEN_BANK;
        } else if ((disk.source == DM_M4_SHADER_TEXTURE || disk.source == DM_M4_SHADER_IMAGE) && disk.data[5] != UINT32_MAX) {
            needs_banks |= UINT32_C(1) << DMSH_TBIAS_BANK;
        }
    }
    if ((needs_banks & ~has_banks) || !descriptor_limits(&properties.limits, descriptors) ||
        !bindless_recipes(bindings, h.binding_count)) {
        result = VK_ERROR_FEATURE_NOT_PRESENT;
        goto done;
    }
    if (fread(capabilities, 1, capability_bytes, reflection) != capability_bytes ||
        fread(c1, 1, h.c1_size, reflection) != h.c1_size ||
        fread(code, 1, h.spirv_size, spirv) != h.spirv_size ||
        code[0] != 0x07230203 || spirv_hash(code, h.spirv_size) != h.spirv_hash)
        goto done;
    if (model == DM_M4_SPV_MESH_EXT && !mesh_outputs(&shader, code, h.spirv_size / 4))
        goto done;
    size_t module_words = h.spirv_size / sizeof(*code);
    if (!shader_properties(&shader, model, set, bindings, h.binding_count, code, module_words))
        goto done;
    if (model == 0 || model == DM_M4_SPV_MESH_EXT) {
        module_words = dm_m4_shader_specialize_point_size(code, module_words);
        if (!module_words) goto done;
        shader.point_size_fallback = module_words != h.spirv_size / sizeof(*code);
    }
    int physical = 0, nonuniform = 0;
    for (uint32_t i = 0; i < h.capability_count; ++i) {
        physical |= capabilities[i] == 5347; // PhysicalStorageBufferAddresses
        nonuniform |= capabilities[i] == 5307; // SampledImageArrayNonUniformIndexing
    }
    if (direct && !nonuniform) goto done;
    if (physical != !!(shader.gmem & 1u) || shader.gmem == 2u ||
        physical != !!(h.flags & REFLECTION_CHECKED_GMEM))
        goto done;
    if (direct) {
        array_flags = calloc(h.binding_count, sizeof *array_flags);
        if (!array_flags) { result = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
        for (uint32_t i = 0; i < h.binding_count; ++i)
            if (dm_m4_shader_direct(&bindings[i])) array_flags[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    }
    const VkDescriptorSetLayoutBindingFlagsCreateInfo indexing = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = h.binding_count, .pBindingFlags = array_flags,
    };
    const VkDescriptorSetLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = direct ? &indexing : NULL,
        .bindingCount = h.binding_count,
        .pBindings = layout,
    };
    VkDescriptorSetLayoutSupport supported = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_SUPPORT};
    layout_support(device->device, &layout_info, &supported);
    if (!supported.supported) {
        result = VK_ERROR_FEATURE_NOT_PRESENT;
        goto done;
    }
    VkDescriptorSetLayout set_layout;
    result = create_layout(device->device, &layout_info, NULL, &set_layout);
    if (result != VK_SUCCESS)
        goto done;
    shader.set_layout = set_layout;
    const VkShaderModuleCreateInfo module_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = module_words * sizeof(*code),
        .pCode = code,
    };
    VkShaderModule module;
    result = create_module(device->device, &module_info, NULL, &module);
    if (result == VK_SUCCESS) {
        shader.module = module;
        shader.stage = expected_stage;
        shader.set = set;
        shader.binding_count = h.binding_count;
        shader.c1_size = h.c1_size;
        shader.c1 = h.c1_size ? c1 : NULL;
        shader.capability_count = h.capability_count;
        shader.capabilities = capabilities;
        shader.frag_write_mask = h.frag_write_mask;
        shader.frag_out_map = h.frag_out_map;
        shader.frag_uint_mask = (uint8_t)(h.flags >> REFLECTION_UINT_SHIFT);
        shader.frag_sint_mask = (uint8_t)(h.flags >> REFLECTION_SINT_SHIFT);
        shader.uses_frag_coord = h.uses_frag_coord;
        memcpy(shader.local, h.local, sizeof(shader.local));
        shader.shared = h.shared;
        if (retain_numeric) {
            shader.numeric_code = code;
            shader.numeric_words = module_words;
            code = NULL;
        }
        *out = shader;
    }
done:
    if (result != VK_SUCCESS)
        dm_m4_shader_destroy(device, &shader);
    free(code);
    free(layout);
    free(array_flags);
    fclose(spirv);
    fclose(reflection);
    return result;
}

void dm_m4_shader_destroy(const struct dm_m4_device *device, struct dm_m4_shader *shader)
{
    if (!shader)
        return;
    if (shader->module) {
        PFN_vkDestroyShaderModule destroy = (PFN_vkDestroyShaderModule)
            device->get_device_proc(device->device, "vkDestroyShaderModule");
        destroy(device->device, shader->module, NULL);
    }
    if (shader->set_layout) {
        PFN_vkDestroyDescriptorSetLayout destroy = (PFN_vkDestroyDescriptorSetLayout)
            device->get_device_proc(device->device, "vkDestroyDescriptorSetLayout");
        destroy(device->device, shader->set_layout, NULL);
    }
    free((void *)shader->bindings);
    free(shader->numeric_code);
    *shader = (struct dm_m4_shader){0};
}

#include "m4_shader_numeric.inc"
