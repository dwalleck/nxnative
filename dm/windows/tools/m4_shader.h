#ifndef DM_WINDOWS_M4_SHADER_H
#define DM_WINDOWS_M4_SHADER_H

#include "m4_window.h"

#define DM_M4_SHADER_ENTRY "main"

enum { DM_M4_SPV_TASK_EXT = 5364, DM_M4_SPV_MESH_EXT = 5365 };
// Windows translates NVN's fixed point size into the PointSize built-in's
// fallback. These IDs are internal to the in-memory shader module and PSO.
#define DM_M4_POINT_SIZE_SPEC_ID UINT32_C(0x4e564e50)
enum { DM_M4_POINT_SIZE_EXTRA_WORDS = 8 };
// Offline VPRS lowering uses a VkBool32 specialization, defaulting to viewport 0.
#define DM_M4_VPRS_VIEWPORT_SPEC_ID UINT32_C(0x4e565250)

// The translator's std140 support buffer, including the padding in each vec4.
struct dm_m4_shader_support {
    int32_t fragment_alpha_test[4];
    int32_t fragment_is_bgra[8][4];
    float viewport_inverse[4];
    float viewport_size[4];
    int32_t fragment_render_scale_count[4];
    float render_scale[73][4];
    int32_t tfe_offset[4];
    int32_t tfe_vertex_count[4];
};

enum dm_m4_shader_binding_source {
    DM_M4_SHADER_SUPPORT, // struct dm_m4_shader_support
    DM_M4_SHADER_CBUF,
    DM_M4_SHADER_SBUF,
    DM_M4_SHADER_TEXTURE,
    DM_M4_SHADER_IMAGE,
    DM_M4_SHADER_GMEM_MAP, // decoded SBUF sentinel, not an on-disk source
    DM_M4_SHADER_GMEM_LOG,
    DM_M4_SHADER_BINDLESS_MAP, // decoded SBUF cb_slot 254
};

struct dm_m4_shader_binding {
    uint32_t binding;
    VkDescriptorType descriptor_type;
    uint32_t count;
    enum dm_m4_shader_binding_source source;
    union {
        struct {
            // c[0]/c[2] are driver data, c[1] is shader->c1, c[3..16]
            // are guest uniform slots 0..13; see dmsh_abi.h for c[24..27].
            uint32_t slot;
            uint32_t fixed_size; // bytes read at fixed offsets; zero-pad short UBOs
        } cbuf;
        struct {
            uint32_t slot; // index into c[27] when flags & 2
            uint32_t cb_slot;
            uint32_t cb_offset; // word offset of the address in c[cb_slot]
            uint32_t flags; // bit 0: writes; bit 1: storage length table bounds
            uint32_t record_stride, record_words; // proven handle-bearing dwords in each material record
        } sbuf;
        struct {
            // Ryujinx TextureHandle packing: -1 names driver c[2]; otherwise
            // low 16 bits are texture cbuf and high 16 bits are sampler cbuf + 1
            // (zero means use the texture's cbuf). Handle low/middle 14 bits
            // are word offsets; the high nibble is the handle mode (4: pool ID).
            int32_t cbuf_slot;
            uint32_t handle;
            uint32_t type; // Ryujinx SamplerType (including array/MS/shadow bits)
            VkFormat format; // VK_FORMAT_UNDEFINED when the shader doesn't specify it
            uint32_t flags; // Ryujinx TextureUsageFlags
            int32_t tbx; // texel-bias table index, or -1
            VkBool32 separate;
            VkBool32 row_bias; // shader addresses the texel buffer as rows of a 2D image
            VkBool32 atomic_word; // direct r32ui/r32i storage variable used by OpImageTexelPointer
        } image; // shared by TEXTURE and IMAGE
    };
};

enum {
    DM_M4_BINDLESS_MAP_BANK = 254, DM_M4_BINDLESS_HEADER_WORDS = 3,
    DM_M4_BINDLESS_IMAGE_CAPACITY = 65536, DM_M4_BINDLESS_SAMPLER_CAPACITY = 4096,
};
static inline int dm_m4_shader_direct(const struct dm_m4_shader_binding *binding)
{
    return binding->source == DM_M4_SHADER_TEXTURE && binding->image.cbuf_slot == 0 &&
           binding->image.handle == UINT32_C(0x40000000);
}

struct dm_m4_shader {
    VkShaderModule module;
    VkDescriptorSetLayout set_layout;
    uint32_t *numeric_code; // owned, post-specialization code for fixed combined sampled variants
    size_t numeric_words;
    VkShaderStageFlagBits stage;
    uint32_t set; // VS/CS/MS: 0, PS: 1, TS: 2
    uint32_t binding_count;
    const struct dm_m4_shader_binding *bindings; // increasing Vulkan binding number
    uint32_t c1_size;
    const uint8_t *c1; // immutable NVN constant bank bytes
    uint32_t capability_count;
    const uint32_t *capabilities; // SPIR-V capability numbers
    uint32_t frag_write_mask; // NVN RGBA nibble per color attachment (PS only)
    uint32_t frag_out_map;
    uint8_t frag_uint_mask, frag_sint_mask; // bit per integer color output location
    VkBool32 uses_frag_coord;
    uint32_t local[3]; // workgroup size (CS, TS, MS)
    uint32_t shared; // workgroup shared bytes (CS, TS, MS)
    uint32_t mesh_vertices, mesh_primitives;
    uint8_t mesh_topology; // vertices per primitive (1, 2 or 3)
    uint8_t mesh_has_task; // MeshEXT reads a TaskPayloadWorkgroupEXT block
    uint8_t point_size_fallback; // module has DM_M4_POINT_SIZE_SPEC_ID
    uint8_t gmem_read_alias; // immutable checked map-only CS guard, verified at load
    uint8_t queries_images; // TXQ may observe descriptor geometry even when a TEX returns zero.
    VkBool32 unfiltered_image_ops; // changing filtering cannot affect this stage's image accesses
    uint32_t gmem; // bit 0: map; bit 1: write log
    int vprs_viewport; // immutable VS marker: NVRP selects the guest VPRS index
    int reads_viewport_index; // PS reads its native scalar ViewportIndex input
};

// Replace the translator's default PointSize store with a specialization.
// The writable span has DM_M4_POINT_SIZE_EXTRA_WORDS of capacity beyond words.
// Returns the new word count, words if no fallback exists, or 0 if malformed.
size_t dm_m4_shader_specialize_point_size(uint32_t *code, size_t words);

struct dm_m4_sample_type {
    uint32_t binding;
    int32_t sign; // +1 unsigned, -1 signed
};

// Returns newly allocated SPIR-V for the specified fixed combined sampled bindings.
VkResult dm_m4_shader_numeric_code(const uint32_t *src, size_t words,
                                   const struct dm_m4_sample_type *types, uint32_t count,
                                   uint32_t **out, size_t *out_words);

// Directory contains <key>.spv and <key>.vkr from win_nvn_shaders.py. Key
// bytes are in guest digest order. Pass an output not already in use; on
// failure it is zeroed. The caller enables the
// SPIR-V capabilities' Vulkan features/extensions before pipeline creation.
VkResult dm_m4_shader_load(const struct dm_m4_device *device, const wchar_t *directory,
                           const uint8_t key[8], VkShaderStageFlagBits expected_stage,
                           struct dm_m4_shader *out);

// All shader-owned handles and reflected storage are freed. Borrowed device
// must still be alive. Never destroy a set layout while a recording uses it.
void dm_m4_shader_destroy(const struct dm_m4_device *device, struct dm_m4_shader *shader);

#endif
