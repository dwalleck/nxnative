// Write gl_FragmentSizeNV to an integer render target. Fetching texels on the
// CPU or substituting a full-rate pipeline cannot pass this GPU readback.
#define OP(words, opcode) (((words) << 16u) | (opcode))
// Position = location 0. With the same specialization as a lowered NVN
// vertex shader, native ViewportIndex = VertexIndex / 6, otherwise zero.
static const uint32_t rate_vertex_spv[] = {
    0x07230203, 0x00010500, 0, 24, 0,
    OP(2, 17), 1, OP(2, 17), 70,                // Shader, ShaderViewportIndex
    OP(3, 14), 0, 1,
    OP(9, 15), 0, 17, 0x6e69616d, 0, 9, 10, 11, 12,
    OP(4, 71), 9, 30, 0,                        // Location 0
    OP(4, 71), 10, 11, 0,                       // Position
    OP(4, 71), 11, 11, 42,                      // VertexIndex
    OP(4, 71), 12, 11, 10,                      // ViewportIndex
    OP(4, 71), 16, 1, 0x4e565250,               // SpecId NVRP
    OP(2, 19), 1, OP(3, 33), 2, 1,              // void, function
    OP(3, 22), 3, 32, OP(4, 23), 4, 3, 4,       // float, vec4
    OP(4, 32), 5, 1, 4, OP(4, 32), 6, 3, 4,     // input/output vec4*
    OP(4, 21), 7, 32, 1, OP(4, 32), 8, 1, 7,    // int, input int*
    OP(4, 32), 13, 3, 7, OP(2, 20), 14,         // output int*, bool
    OP(4, 59), 5, 9, 1, OP(4, 59), 6, 10, 3,
    OP(4, 59), 8, 11, 1, OP(4, 59), 13, 12, 3,
    OP(4, 43), 7, 15, 0, OP(4, 43), 7, 23, 6,
    OP(3, 49), 14, 16,                          // OpSpecConstantFalse
    OP(5, 54), 1, 17, 0, 2, OP(2, 248), 18,
    OP(4, 61), 4, 19, 9, OP(3, 62), 10, 19,
    OP(4, 61), 7, 20, 11, OP(5, 135), 7, 21, 20, 23, // VertexIndex / 6
    OP(6, 169), 7, 22, 16, 21, 15, OP(3, 62), 12, 22,
    OP(1, 253), OP(1, 56),
};
static const uint32_t rate_spv[] = {
    0x07230203, 0x00010000, 0, 18, 0,
    OP(2, 17), 1,                               // OpCapability Shader
    OP(2, 17), 5291,                            // OpCapability ShadingRateNV
    OP(6, 10), 0x5f565053, 0x735f564e, 0x69646168, 0x725f676e, 0x00657461, // OpExtension "SPV_NV_shading_rate"
    OP(3, 14), 0, 1,                            // OpMemoryModel Logical GLSL450
    OP(7, 15), 4, 12, 0x6e69616d, 0, 10, 11,    // OpEntryPoint Fragment %12 "main" %10 %11
    OP(3, 16), 12, 7,                           // OpExecutionMode %12 OriginUpperLeft
    OP(4, 71), 10, 11, 5292,                    // OpDecorate %10 BuiltIn FragmentSizeNV
    OP(4, 71), 11, 30, 0,                       // OpDecorate %11 Location 0
    OP(2, 19), 1,                               // %1 = OpTypeVoid
    OP(4, 21), 2, 32, 1,                        // %2 = OpTypeInt 32 signed
    OP(4, 23), 3, 2, 2,                         // %3 = OpTypeVector %2 2
    OP(4, 23), 4, 2, 4,                         // %4 = OpTypeVector %2 4
    OP(4, 32), 5, 1, 3,                         // %5 = OpTypePointer Input %3
    OP(4, 32), 6, 3, 4,                         // %6 = OpTypePointer Output %4
    OP(3, 33), 7, 1,                            // %7 = OpTypeFunction %1
    OP(4, 43), 2, 8, 0,                         // %8 = OpConstant %2 0
    OP(4, 43), 2, 9, 1,                         // %9 = OpConstant %2 1
    OP(4, 59), 5, 10, 1,                        // %10 = OpVariable %5 Input
    OP(4, 59), 6, 11, 3,                        // %11 = OpVariable %6 Output
    OP(5, 54), 1, 12, 0, 7,                     // %12 = OpFunction %1 None %7
    OP(2, 248), 13,                             // %13 = OpLabel
    OP(4, 61), 3, 14, 10,                       // %14 = OpLoad %3 %10
    OP(5, 81), 2, 15, 14, 0,                    // %15 = OpCompositeExtract %2 %14 0
    OP(5, 81), 2, 16, 14, 1,                    // %16 = OpCompositeExtract %2 %14 1
    OP(7, 80), 4, 17, 15, 16, 8, 9,             // %17 = OpCompositeConstruct %4 %15 %16 %8 %9
    OP(3, 62), 11, 17,                          // OpStore %11 %17
    OP(1, 253), OP(1, 56),                      // OpReturn, OpFunctionEnd
};
#undef OP

static const uint32_t rate_caps[] = {1, 5291};
static const uint32_t rate_vs_caps[] = {1, 70};

static VkResult rate_draw_smoke(const struct dm_m4_device *device, const struct dm_m4_draw_requirements *requirements,
                                const char **step)
{
    const VkDevice vk = device->device;
    PFN_vkCreateShaderModule make_shader = (PFN_vkCreateShaderModule)device->get_device_proc(vk, "vkCreateShaderModule");
    PFN_vkCreateDescriptorSetLayout make_set = (PFN_vkCreateDescriptorSetLayout)
        device->get_device_proc(vk, "vkCreateDescriptorSetLayout");
    PFN_vkGetPhysicalDeviceProperties2 get_properties = (PFN_vkGetPhysicalDeviceProperties2)
        device->get_instance_proc(device->instance, "vkGetPhysicalDeviceProperties2");
    *step = "load shading-rate smoke dispatch";
    if (!make_shader || !make_set || !get_properties) return VK_ERROR_INITIALIZATION_FAILED;
    VkPhysicalDeviceShadingRateImagePropertiesNV rate_properties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADING_RATE_IMAGE_PROPERTIES_NV,
    };
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &rate_properties};
    get_properties(device->physical, &properties);
    const VkExtent2D tile = rate_properties.shadingRateTexelSize;
    const VkPhysicalDeviceLimits *limits = &properties.properties.limits;
    *step = "check shading-rate smoke tile dimensions";
    if (!tile.width || !tile.height || (tile.width % 2u) || (tile.height % 2u) ||
        tile.width > limits->maxFramebufferWidth / 2u || tile.height > limits->maxFramebufferHeight / 2u)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    const VkExtent2D extent = {2u * tile.width, 2u * tile.height};
    if (extent.width > SIZE_MAX / 4u / extent.height) return VK_ERROR_OUT_OF_HOST_MEMORY;
    const size_t pixel_bytes = (size_t)extent.width * extent.height * 4u;
    uint8_t *pixels = malloc(pixel_bytes);
    if (!pixels) return VK_ERROR_OUT_OF_HOST_MEMORY;

    struct dm_m4_draw *draw = NULL;
    struct dm_m4_shader vs = {.stage = VK_SHADER_STAGE_VERTEX_BIT, .vprs_viewport = 1,
        .capabilities = rate_vs_caps, .capability_count = sizeof rate_vs_caps / sizeof *rate_vs_caps};
    struct dm_m4_shader ps = {
        .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .set = 1, .frag_write_mask = 15, .frag_sint_mask = 1,
        .capabilities = rate_caps, .capability_count = sizeof rate_caps / sizeof *rate_caps,
    };
    VkShaderModuleCreateInfo module = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .pCode = rate_vertex_spv, .codeSize = sizeof rate_vertex_spv,
    };
    *step = "create shading-rate smoke vertex shader";
    VkResult result = make_shader(vk, &module, NULL, &vs.module);
    if (result != VK_SUCCESS) goto done;
    module.pCode = rate_spv;
    module.codeSize = sizeof rate_spv;
    *step = "create shading-rate smoke fragment shader";
    result = make_shader(vk, &module, NULL, &ps.module);
    if (result != VK_SUCCESS) goto done;
    const VkDescriptorSetLayoutCreateInfo set = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    *step = "create shading-rate smoke descriptor set layouts";
    result = make_set(vk, &set, NULL, &vs.set_layout);
    if (result != VK_SUCCESS) goto done;
    result = make_set(vk, &set, NULL, &ps.set_layout);
    if (result != VK_SUCCESS) goto done;
    result = dm_m4_draw_create(&draw, device, requirements, step);
    if (result != VK_SUCCESS) goto done;

    const float vertices[][4] = {{-1.f, -1.f, .5f, 1.f}, {3.f, -1.f, .5f, 1.f}, {-1.f, 3.f, .5f, 1.f}};
    const VkVertexInputBindingDescription stream = {.stride = sizeof vertices[0]};
    const VkVertexInputAttributeDescription attribute = {.format = VK_FORMAT_R32G32B32A32_SFLOAT};
    const VkPipelineVertexInputStateCreateInfo input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &stream,
        .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &attribute,
    };
    const VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .lineWidth = 1.f,
    };
    const VkPipelineMultisampleStateCreateInfo samples = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineColorBlendAttachmentState write = {.colorWriteMask = 15};
    const VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &write,
    };
    struct dm_m4_draw_info info = {
        .vs = {&vs}, .ps = {&ps},
        .pipeline = {.vertex_input = &input, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                     .rasterization = &raster, .multisample = &samples, .blend = &blend, .viewport_count = 1},
        .extent = extent, .viewport = {0, 0, (float)extent.width, (float)extent.height, 0.f, 1.f},
        .scissor = {{0, 0}, extent},
        .color_count = 1,
        .colors = {{.format = VK_FORMAT_R8G8B8A8_SINT, .width = extent.width, .height = extent.height,
                    .pixels = {pixels, pixel_bytes, 0}}},
        .vertex_count = 3, .instance_count = 1, .vertices = {{vertices, sizeof vertices}},
    };
    enum { LAST = DM_M4_RATE_PALETTE_SIZE - 1 };
    uint8_t texels[] = {0, LAST, 0xff, 0, 0}; // two rows, stride 3, no trailing padding required
    struct dm_m4_rate_image rate = {.texels = {texels, sizeof texels, 3}, .width = 2, .height = 2};
    for (uint32_t i = 0; i < DM_M4_RATE_PALETTE_SIZE; ++i)
        rate.palette[i] = VK_SHADING_RATE_PALETTE_ENTRY_1_INVOCATION_PER_PIXEL_NV;
    rate.palette[LAST] = VK_SHADING_RATE_PALETTE_ENTRY_1_INVOCATION_PER_2X2_PIXELS_NV;
    static const float halves[][4] = {
        {-1,-1,0,1},{0,-1,0,1},{0,1,0,1}, {-1,-1,0,1},{0,1,0,1},{-1,1,0,1},
        {0,-1,0,1},{1,-1,0,1},{1,1,0,1}, {0,-1,0,1},{1,1,0,1},{0,1,0,1},
    };
    VkShadingRatePaletteEntryNV extra[1][DM_M4_RATE_PALETTE_SIZE];
    static const struct { const char *name; unsigned coarse_tiles; } cases[] = {
        {"unbound and disabled shading rate", 0x0},
        {"padded R8_UINT shading-rate image and palette index 15", 0x2},
        {"shade from updated image without changing PSO", 0x4},
        {"shade from changed palette with unchanged image", 0xb},
        {"explicit null image uses palette entry zero", 0xf},
        {"disabled shading rate ignores prior image and palette", 0x0},
        {"left/right primitives select different palettes with no rate image", 0xa},
        {"changing only palette one invalidates the cached pipeline", 0x0},
        {"ordinary viewport mode ignores the vertex's primitive index", 0x0},
    };
    for (size_t pass = 0; pass < sizeof cases / sizeof *cases; ++pass) {
        switch (pass) {
        case 1: info.rate_image = &rate; break;
        case 2: texels[1] = 0; texels[3] = LAST; break;
        case 3:
            rate.palette[0] = VK_SHADING_RATE_PALETTE_ENTRY_1_INVOCATION_PER_2X2_PIXELS_NV;
            rate.palette[LAST] = VK_SHADING_RATE_PALETTE_ENTRY_1_INVOCATION_PER_PIXEL_NV;
            break;
        case 4: rate.texels = (struct dm_m4_draw_pixels){0}; rate.width = rate.height = 0; break;
        case 5: info.rate_image = NULL; break;
        case 6:
            info.rate_image = &rate;
            info.pipeline.viewport_count = 2;
            info.vertices[0] = (struct dm_m4_draw_span){halves, sizeof halves};
            info.vertex_count = sizeof halves / sizeof *halves;
            rate.palette[0] = VK_SHADING_RATE_PALETTE_ENTRY_1_INVOCATION_PER_PIXEL_NV;
            memcpy(extra[0], rate.palette, sizeof extra[0]);
            extra[0][0] = VK_SHADING_RATE_PALETTE_ENTRY_1_INVOCATION_PER_2X2_PIXELS_NV;
            rate.additional_palettes = extra;
            break;
        case 7: extra[0][0] = VK_SHADING_RATE_PALETTE_ENTRY_1_INVOCATION_PER_PIXEL_NV; break;
        case 8: info.pipeline.viewport_count = 1; rate.additional_palettes = NULL; break;
        }
        memset(pixels, 0, pixel_bytes);
        result = dm_m4_draw_submit(draw, &info, step);
        if (result != VK_SUCCESS) goto done;
        for (uint32_t y = 0; y < extent.height; ++y) {
            for (uint32_t x = 0; x < extent.width; ++x) {
                const unsigned quadrant = 2u * (y / tile.height) + x / tile.width;
                const unsigned expected = ((cases[pass].coarse_tiles >> quadrant) & 1u) + 1u;
                const uint8_t *p = pixels + ((size_t)y * extent.width + x) * 4u;
                if (p[0] != expected || p[1] != expected || p[2] || p[3] != 1u) {
                    fprintf(stderr, "pixel (%u,%u): expected %ux%u, read %ux%u\n", x, y, expected, expected,
                            (unsigned)p[0], (unsigned)p[1]);
                    *step = cases[pass].name;
                    result = VK_ERROR_UNKNOWN;
                    goto done;
                }
            }
        }
    }
    info.rate_image = &rate;
    rate.width = rate.height = 2;
    rate.texels = (struct dm_m4_draw_pixels){texels, sizeof texels - 1, 3};
    result = dm_m4_draw_submit(draw, &info, step);
    if (result != VK_ERROR_INITIALIZATION_FAILED || strcmp(*step, "invalid NV shading-rate image texel rows")) {
        *step = "reject a truncated shading-rate image span";
        result = VK_ERROR_UNKNOWN;
        goto done;
    }
    rate.texels.size = sizeof texels;
    texels[4] = DM_M4_RATE_PALETTE_SIZE;
    result = dm_m4_draw_submit(draw, &info, step);
    if (result != VK_ERROR_INITIALIZATION_FAILED || strcmp(*step, "NV shading-rate image index exceeds palette")) {
        *step = "reject a shading-rate texel index outside its palette";
        result = VK_ERROR_UNKNOWN;
        goto done;
    }
    result = VK_SUCCESS;
done:
    dm_m4_draw_destroy(draw);
    dm_m4_shader_destroy(device, &ps);
    dm_m4_shader_destroy(device, &vs);
    free(pixels);
    return result;
}

static int run_rate_smoke(void)
{
    const struct dm_m4_draw_capabilities caps[] = {
        {VK_SHADER_STAGE_FRAGMENT_BIT, sizeof rate_caps / sizeof *rate_caps, rate_caps},
        {VK_SHADER_STAGE_VERTEX_BIT, sizeof rate_vs_caps / sizeof *rate_vs_caps, rate_vs_caps},
    };
    struct dm_m4_draw_requirements requirements;
    struct dm_m4_window *window = NULL;
    const char *step = NULL;
    VkResult result = dm_m4_draw_requirements_init(&requirements, DM_M4_DRAW_SHADING_RATE_IMAGE,
                                                   caps, sizeof caps / sizeof *caps, &step);
    if (result == VK_SUCCESS) {
        step = "create Vulkan NV shading-rate image device";
        result = dm_m4_window_create(&window, L"NV shading-rate image smoke", 320, 180, &requirements.requirements);
    }
    if (result == VK_SUCCESS) result = rate_draw_smoke(dm_m4_window_get_device(window), &requirements, &step);
    if (window) dm_m4_window_destroy(window);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "%s: VkResult %d\n", step, (int)result);
        return 1;
    }
    puts("NV 1x1/2x2 readback, per-primitive palette selection, palette changes and null binds passed");
    return 0;
}
