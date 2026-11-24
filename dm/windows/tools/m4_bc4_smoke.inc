// Draw the captured Ocarina BC4 encoder pair into a raw block color view.
// The synthetic source has a zero plane and a plane with one zero column.
static VkResult draw_bc4_smoke(const struct dm_m4_device *device,
                               const struct dm_m4_draw_requirements *requirements,
                               const wchar_t *directory, const char **step)
{
    static const uint8_t vs_key[] = {0xa8, 0xc2, 0x83, 0xdb, 0x8a, 0xc4, 0x1d, 0x16};
    static const uint8_t ps_key[] = {0x95, 0x6e, 0xed, 0xe8, 0x55, 0xc3, 0xf3, 0x79};
    struct dm_m4_shader vs = {0}, ps = {0};
    struct dm_m4_draw *draw = NULL;
    *step = "load captured BC4 encoder vertex shader";
    VkResult result = dm_m4_shader_load(device, directory, vs_key, VK_SHADER_STAGE_VERTEX_BIT, &vs);
    if (result != VK_SUCCESS) goto done;
    *step = "load captured BC4 encoder fragment shader";
    result = dm_m4_shader_load(device, directory, ps_key, VK_SHADER_STAGE_FRAGMENT_BIT, &ps);
    if (result != VK_SUCCESS) goto done;
    *step = "check captured BC4 shader recipes and unsigned output";
    if (vs.binding_count != 1 || ps.binding_count != 2 || ps.frag_uint_mask != 1 || ps.frag_sint_mask ||
        vs.bindings[0].source != DM_M4_SHADER_CBUF || ps.bindings[0].source != DM_M4_SHADER_TEXTURE ||
        ps.bindings[0].image.type != 4 || ps.bindings[1].source != DM_M4_SHADER_CBUF) {
        result = VK_ERROR_INVALID_SHADER_NV;
        goto done;
    }
    result = dm_m4_draw_create(&draw, device, requirements, step);
    if (result != VK_SUCCESS) goto done;

    const uint8_t source[2][4][4] = {
        {{0}},
        {{0, 255, 255, 255}, {0, 255, 255, 255}, {0, 255, 255, 255}, {0, 255, 255, 255}},
    };
    const struct dm_m4_draw_pixels level = {source, sizeof source, 4};
    const struct dm_m4_draw_sampled_image volume = {
        .format = VK_FORMAT_R8_UNORM, .width = 4, .height = 4, .depth = 2, .level_count = 1, .levels = &level,
        .sampler = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE},
    };
    // Encoder c[3] carries reciprocal source width/height, LOD and z. At
    // vertex UVs 0.5 this VS starts its 4×4 footprint at texel (0, 0).
    float vertex_uniform[] = {0.25f, 0.25f, 0.0f, 0.0f, 0.0f, 0.75f};
    const float fragment_uniform[5] = {0};
    const struct dm_m4_draw_binding vs_data = {.buffer = {vertex_uniform, sizeof vertex_uniform}};
    const struct dm_m4_draw_binding ps_data[] = {
        {.texture = &volume}, {.buffer = {fragment_uniform, sizeof fragment_uniform}},
    };
    const float vertices[][4] = {
        {-0.5f, -0.5f, 0.5f, 0.5f}, {1.5f, -0.5f, 0.5f, 0.5f}, {-0.5f, 1.5f, 0.5f, 0.5f},
    };
    const uint16_t indices[] = {0, 1, 2};
    const VkVertexInputBindingDescription stream = {.stride = sizeof *vertices};
    const VkVertexInputAttributeDescription attrs[] = {
        {.location = 0, .format = VK_FORMAT_R32G32_SFLOAT},
        {.location = 1, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 2 * sizeof(float)},
    };
    const VkPipelineVertexInputStateCreateInfo input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &stream,
        .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = attrs,
    };
    const VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineColorBlendAttachmentState target = {.colorWriteMask = 15};
    const VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &target,
    };
    uint8_t blocks[2][8] = {{0xdb, 0xdb}, {17, 34, 51, 68, 85, 102, 119, 136}};
    const uint8_t untouched[] = {17, 34, 51, 68, 85, 102, 119, 136};
    struct dm_m4_draw_info info = {
        .vs = {&vs, &vs_data}, .ps = {&ps, ps_data},
        .pipeline = {.vertex_input = &input, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                     .rasterization = &raster, .multisample = &multisample, .blend = &blend, .viewport_count = 1},
        .viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f}, .scissor = {{0, 0}, {1, 1}},
        .extent = {1, 1}, .color_count = 1,
        .colors = {{.format = VK_FORMAT_R16G16B16A16_UINT, .width = 2, .height = 1,
                    .pixels = {blocks, sizeof blocks, 0}}},
        .indices = {indices, sizeof indices}, .index_type = VK_INDEX_TYPE_UINT16, .index_count = 3, .instance_count = 1,
        .vertices = {{vertices, sizeof vertices}},
    };
    result = dm_m4_draw_submit(draw, &info, step);
    if (result != VK_SUCCESS) goto done;
    *step = "verify unsigned BC4 block endpoints, indices and unchanged adjacent block";
    if (blocks[0][0] != 255 || blocks[0][1] != 0 || memcmp(blocks[1], untouched, sizeof untouched)) {
        result = VK_ERROR_UNKNOWN;
        goto done;
    }
    // BC4 (255 > 0): each 3-bit selector must pick endpoint 0 (255)
    // or endpoint 1 (0); four source texels have the zero endpoint.
    uint64_t selectors = 0;
    for (unsigned i = 0; i < 6; ++i) selectors |= (uint64_t)blocks[0][i + 2] << (8u * i);
    unsigned zeros = 0;
    for (unsigned i = 0; i < 16; ++i, selectors >>= 3) {
        unsigned index = (unsigned)(selectors & 7u);
        if (index > 1) {
            result = VK_ERROR_UNKNOWN;
            goto done;
        }
        zeros += index;
    }
    if (zeros != 4) {
        result = VK_ERROR_UNKNOWN;
        goto done;
    }
    // Changing only z to the first plane must produce zero endpoints.
    vertex_uniform[5] = 0.25f;
    result = dm_m4_draw_submit(draw, &info, step);
    if (result != VK_SUCCESS) goto done;
    *step = "verify BC4 encoder reads the other volume slice";
    if (blocks[0][0] || blocks[0][1]) result = VK_ERROR_UNKNOWN;
done:
    dm_m4_draw_destroy(draw);
    dm_m4_shader_destroy(device, &ps);
    dm_m4_shader_destroy(device, &vs);
    return result;
}
