// Two invocations copy an RGBA texel into the target and test texel-buffer/SSBO writes.
// A volume uses id.x as its z coordinate: the second write must land in slice one.
static VkResult compute_pixels_smoke(const struct dm_m4_device *device,
                                     const struct dm_m4_draw_requirements *requirements, int volume, const char **step)
{
#define OP(words, opcode) (((words) << 16u) | (opcode))
    uint32_t compute_copy_spv[] = {
    0x07230203, 0x00010000, 0, 52, 0,
    OP(2, 17), 1, OP(2, 17), 46, OP(2, 17), 47, // Shader, SampledBuffer, ImageBuffer
    OP(3, 14), 0, 1,                            // Logical GLSL450
    OP(6, 15), 5, 34, 0x6e69616d, 0, 11,        // GLCompute %34 "main" %11
    OP(6, 16), 34, 17, 1, 1, 1,                // LocalSize 1 1 1
    OP(4, 71), 11, 11, 28,                     // BuiltIn GlobalInvocationId
    OP(4, 71), 15, 34, 0, OP(4, 71), 15, 33, 1, // Sampled image set 0 binding 1
    OP(4, 71), 18, 34, 0, OP(4, 71), 18, 33, 2, // Storage image binding 2
    OP(4, 71), 21, 34, 0, OP(4, 71), 21, 33, 3, // Uniform texel buffer binding 3
    OP(4, 71), 24, 34, 0, OP(4, 71), 24, 33, 4, // Storage texel buffer binding 4
    OP(4, 71), 28, 34, 0, OP(4, 71), 28, 33, 5, // SSBO binding 5
    OP(4, 71), 25, 6, 4,                       // ArrayStride 4
    OP(3, 71), 26, 3,                           // BufferBlock
    OP(5, 72), 26, 0, 35, 0,                   // Member 0 Offset 0
    OP(2, 19), 1, OP(3, 33), 2, 1,              // Void and function type
    OP(4, 21), 3, 32, 0,                       // uint
    OP(4, 23), 4, 3, 3, OP(4, 23), 5, 3, 2, OP(4, 23), 6, 3, 4, // uvec3, uvec2, uvec4
    OP(3, 22), 7, 32, OP(4, 23), 8, 7, 2, OP(4, 23), 9, 7, 4,   // float, vec2, vec4
    OP(4, 32), 10, 1, 4,                       // Input uvec3 pointer
    OP(9, 25), 12, 7, 1, 0, 0, 0, 1, 0,        // Sampled 2D float image
    OP(3, 27), 13, 12, OP(4, 32), 14, 0, 13,   // Combined sampled 2D image
    OP(9, 25), 16, 7, volume ? 2u : 1u, 0, 0, 0, 2, 4, // Storage 3D or 2D rgba8
    OP(4, 32), 17, 0, 16,
    OP(9, 25), 19, 3, 5, 0, 0, 0, 1, 0,        // Uniform texel uint
    OP(4, 32), 20, 0, 19,
    OP(9, 25), 22, 3, 5, 0, 0, 0, 2, 33,       // Storage texel r32ui
    OP(4, 32), 23, 0, 22,
    OP(3, 29), 25, 3, OP(3, 30), 26, 25,       // Runtime uint array in SSBO
    OP(4, 32), 27, 2, 26, OP(4, 32), 29, 2, 3,
    OP(4, 43), 3, 30, 0, OP(4, 43), 7, 31, 0,
    OP(4, 43), 7, 32, 0x3f000000, OP(5, 44), 8, 33, 32, 32, // vec2(0.5)
    OP(4, 43), 3, 47, 2,
    OP(4, 59), 10, 11, 1, OP(4, 59), 14, 15, 0, OP(4, 59), 17, 18, 0,
    OP(4, 59), 20, 21, 0, OP(4, 59), 23, 24, 0, OP(4, 59), 27, 28, 2,
    OP(5, 54), 1, 34, 0, 2, OP(2, 248), 35,
    OP(4, 61), 4, 36, 11,                      // Load global id
    OP(8, 79), 4, 37, 36, 36, 0, 1, 0,         // Global id.xyx
    OP(5, 81), 3, 38, 36, 0,                   // Global id.x
    OP(4, 61), 13, 39, 15,
    OP(7, 88), 9, 40, 39, 33, 2, 31,           // Sample explicit LOD 0
    OP(4, 61), 16, 41, 18, OP(4, 99), 41, 37, 40,
    OP(4, 61), 19, 42, 21, OP(5, 95), 6, 43, 42, 38, // Fetch uniform texel
    OP(4, 61), 22, 44, 24, OP(4, 99), 44, 38, 43,
    OP(5, 81), 3, 45, 43, 0, OP(6, 65), 29, 46, 28, 30, 38,
    OP(3, 62), 46, 45,
    OP(4, 124), 6, 48, 40,                      // Bitcast sampled vec4 to uvec4.
    OP(5, 77), 3, 49, 48, 38,                   // id.x selects R or G.
    OP(5, 128), 3, 50, 38, 47,                  // write to ssbo[id.x + 2]
    OP(6, 65), 29, 51, 28, 30, 50, OP(3, 62), 51, 49,
    OP(1, 253), OP(1, 56),
    };
#undef OP

    static const uint32_t caps[] = {1, 46, 47};
    const struct dm_m4_shader_binding recipe[] = {
        {.binding = 1, .count = 1, .descriptor_type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .source = DM_M4_SHADER_TEXTURE, .image = {.type = 3, .tbx = -1}},
        {.binding = 2, .count = 1, .descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .source = DM_M4_SHADER_IMAGE, .image = {.type = volume ? 4u : 3u, .format = VK_FORMAT_R8G8B8A8_UNORM, .flags = 4, .tbx = -1}},
        {.binding = 3, .count = 1, .descriptor_type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
         .source = DM_M4_SHADER_TEXTURE, .image = {.type = 2, .tbx = -1}},
        {.binding = 4, .count = 1, .descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,
         .source = DM_M4_SHADER_IMAGE, .image = {.type = 2, .format = VK_FORMAT_R32_UINT, .flags = 4, .tbx = -1}},
        {.binding = 5, .count = 1, .descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .source = DM_M4_SHADER_SBUF, .sbuf = {.flags = 1}},
    };
    enum { COUNT = sizeof recipe / sizeof *recipe, PITCH = 12 };
    struct dm_m4_shader cs = {
        .stage = VK_SHADER_STAGE_COMPUTE_BIT, .bindings = recipe, .binding_count = COUNT,
        .local = {1, 1, 1}, .capability_count = sizeof caps / sizeof *caps, .capabilities = caps,
        .numeric_code = compute_copy_spv, .numeric_words = sizeof compute_copy_spv / sizeof *compute_copy_spv,
    };
    const VkDevice vk = device->device;
    PFN_vkCreateShaderModule make_shader = (PFN_vkCreateShaderModule)device->get_device_proc(vk, "vkCreateShaderModule");
    PFN_vkCreateDescriptorSetLayout make_set = (PFN_vkCreateDescriptorSetLayout)
        device->get_device_proc(vk, "vkCreateDescriptorSetLayout");
    *step = "load staged compute smoke dispatch";
    if (!make_shader || !make_set) return VK_ERROR_INITIALIZATION_FAILED;
    const VkShaderModuleCreateInfo module = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .pCode = compute_copy_spv, .codeSize = sizeof compute_copy_spv,
    };
    *step = "create staged compute smoke shader";
    VkResult r = make_shader(vk, &module, NULL, &cs.module);
    if (r != VK_SUCCESS) return r;
    VkDescriptorSetLayoutBinding bindings[COUNT];
    for (uint32_t i = 0; i < COUNT; ++i)
        bindings[i] = (VkDescriptorSetLayoutBinding){recipe[i].binding, recipe[i].descriptor_type, 1, cs.stage, NULL};
    const VkDescriptorSetLayoutCreateInfo set = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = COUNT, .pBindings = bindings,
    };
    *step = "create staged compute smoke set";
    r = make_set(vk, &set, NULL, &cs.set_layout);
    struct dm_m4_draw *draw = NULL;
    if (r == VK_SUCCESS) r = dm_m4_draw_create(&draw, device, requirements, step);
    if (r == VK_SUCCESS) {
        const uint8_t known[] = {27, 119, 198, 241};
        const uint32_t known_words[] = {UINT32_C(0x01020304), UINT32_C(0x76543210)};
        uint8_t rgba[2][4][PITCH];
        uint32_t words[2][2] = {{0}}, ssbo[4];
        memset(rgba, 0xdb, sizeof rgba);
        struct dm_m4_draw_pixels source = {known, sizeof known, 0};
        struct dm_m4_draw_sampled_image texture = {
            .format = VK_FORMAT_R8G8B8A8_UNORM, .width = 1, .height = 1, .level_count = 1, .levels = &source,
            .sampler = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO},
        };
        struct dm_m4_draw_storage_image target = {.format = VK_FORMAT_R8G8B8A8_UNORM, .width = 2, .height = 2,
            .depth = volume ? 2u : 0u};
        struct dm_m4_draw_binding input[COUNT] = {
            {.texture = &texture}, {.image = &target},
            {.buffer = {known_words, sizeof known_words}, .texel_format = VK_FORMAT_R32_UINT},
            {0}, {.buffer = {ssbo, sizeof ssbo}, .writeback = ssbo},
        };
        const struct dm_m4_compute_info info = {.cs = {&cs, input}, .groups = {2, 1, 1}};
        for (size_t pass = 0; pass < 2; ++pass) {
            target.pixels = (struct dm_m4_draw_pixels){rgba[pass], volume ? 4 * PITCH : 2 * PITCH, PITCH};
            target.writeback = rgba[pass];
            input[3] = (struct dm_m4_draw_binding){.buffer = {words[pass], sizeof words[pass]}, .writeback = words[pass]};
            memset(ssbo, 0, sizeof ssbo);
            r = dm_m4_compute_submit(draw, &info, step);
            if (r != VK_SUCCESS) break;
            *step = "verify compute image row padding and texel/SSBO readback across dispatches";
            if (memcmp(rgba[pass][0], known, sizeof known) || memcmp(rgba[pass][volume ? 2 : 0] + 4, known, sizeof known) ||
                memcmp(words[pass], known_words, sizeof known_words) || memcmp(ssbo, known_words, sizeof known_words)) {
                r = VK_ERROR_UNKNOWN;
                break;
            }
            const size_t other = (volume ? 2 * PITCH : 0) + sizeof known;
            for (size_t i = sizeof known; i < sizeof rgba[pass]; ++i)
                if ((i < other || i >= other + sizeof known) && ((const uint8_t *)rgba[pass])[i] != 0xdb) r = VK_ERROR_UNKNOWN;
            if (r != VK_SUCCESS) break;
            source = (struct dm_m4_draw_pixels){&rgba[pass][volume ? 2 : 0][volume ? 4 : 0],
                volume ? sizeof known : PITCH, PITCH};
            texture.width = volume ? 1 : 2;
            input[2].buffer.data = words[pass];
        }
        if (r == VK_SUCCESS && !volume) {
            // The same float-declared sampler sees unsigned, signed, then float
            // texels. The shader moves their bits; numeric conversion is wrong.
            const uint16_t pair[2] = {0x8023, 0xfffe};
            const float floats[2] = {0.25f, 0.5f};
            const VkFormat formats[] = {VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT,
                                        VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R16G16_UINT};
            const uint32_t expected[][2] = {{0x8023, 0xfffe}, {0xffff8023, 0xfffffffe},
                                            {0x3e800000, 0x3f000000}, {0x8023, 0xfffe}};
            texture.width = 1;
            input[2].buffer.data = known_words;
            for (unsigned i = 0; i < sizeof formats / sizeof *formats; ++i) {
                texture.format = formats[i];
                texture.sampler.borderColor = i == 2 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK :
                                                       VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
                source = i == 2 ? (struct dm_m4_draw_pixels){floats, sizeof floats, 0} :
                                  (struct dm_m4_draw_pixels){pair, sizeof pair, 0};
                ssbo[2] = ssbo[3] = 0xdeadbeef;
                r = dm_m4_compute_submit(draw, &info, step);
                if (r != VK_SUCCESS) break;
                if (memcmp(ssbo + 2, expected[i], sizeof expected[i])) {
                    *step = "fixed sampled integer and float views return raw register bits";
                    r = VK_ERROR_UNKNOWN;
                    break;
                }
            }
        }
    }
    dm_m4_draw_destroy(draw);
    cs.bindings = NULL;
    cs.numeric_code = NULL; // This fixture borrows its SPIR-V words.
    dm_m4_shader_destroy(device, &cs);
    return r;
}
