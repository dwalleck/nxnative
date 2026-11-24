// Compile the real taskless and taskful gameplay pipelines without needing
// a guest memory snapshot. The live New Game replay tests their submissions.
static VkResult mesh_smoke_pair(const struct dm_m4_device *device, const wchar_t *directory,
                                const uint8_t ms_key[8], const uint8_t ps_key[8], const uint8_t *ts_key,
                                const char **step)
{
    struct dm_m4_shader ms = {0}, ps = {0}, ts = {0};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    PFN_vkCreatePipelineLayout create = (PFN_vkCreatePipelineLayout)
        device->get_device_proc(device->device, "vkCreatePipelineLayout");
    PFN_vkDestroyPipelineLayout destroy = (PFN_vkDestroyPipelineLayout)
        device->get_device_proc(device->device, "vkDestroyPipelineLayout");
    *step = "load native mesh layout dispatch";
    if (!create || !destroy) return VK_ERROR_INITIALIZATION_FAILED;
    *step = "load real MeshEXT shader and descriptor set";
    VkResult result = dm_m4_shader_load(device, directory, ms_key, VK_SHADER_STAGE_MESH_BIT_EXT, &ms);
    if (result != VK_SUCCESS) goto done;
    *step = "load real mesh fragment shader and descriptor set";
    result = dm_m4_shader_load(device, directory, ps_key, VK_SHADER_STAGE_FRAGMENT_BIT, &ps);
    if (result != VK_SUCCESS) goto done;
    if (ts_key) {
        *step = "load real TaskEXT shader and descriptor set";
        result = dm_m4_shader_load(device, directory, ts_key, VK_SHADER_STAGE_TASK_BIT_EXT, &ts);
        if (result != VK_SUCCESS) goto done;
    }
    if (ms.mesh_has_task != (ts_key != NULL) || ms.mesh_topology != 3 || ms.local[0] != 32 ||
        ms.mesh_vertices != 96 || ms.mesh_primitives != (ts_key ? 96u : 64u) ||
        (ts_key && (ts.local[0] != 32 || ms.gmem != 1 || ts.gmem != 1))) {
        *step = "check real mesh stage and checked-memory metadata";
        result = VK_ERROR_INVALID_SHADER_NV;
        goto done;
    }
    const VkDescriptorSetLayout sets[] = {ms.set_layout, ps.set_layout, ts.set_layout};
    const VkPushConstantRange first = {
        .stageFlags = ts_key ? VK_SHADER_STAGE_TASK_BIT_EXT : VK_SHADER_STAGE_MESH_BIT_EXT,
        .size = sizeof(uint32_t),
    };
    const VkPipelineLayoutCreateInfo li = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = ts_key ? 3u : 2u,
        .pSetLayouts = sets, .pushConstantRangeCount = 1, .pPushConstantRanges = &first,
    };
    *step = "create real mesh pipeline layout";
    result = create(device->device, &li, NULL, &layout);
    if (result != VK_SUCCESS) goto done;
    const VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo samples = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineColorBlendAttachmentState attachment = {.colorWriteMask = 15};
    const VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment,
    };
    const VkFormat color = VK_FORMAT_R8G8B8A8_UNORM;
    const struct dm_m4_pipeline_info info = {
        .layout = layout, .ms = ms.module, .ps = ps.module, .ts = ts.module,
        .ms_entry = DM_M4_SHADER_ENTRY, .ps_entry = DM_M4_SHADER_ENTRY, .ts_entry = DM_M4_SHADER_ENTRY,
        .rasterization = &raster, .multisample = &samples, .blend = &blend, .color_formats = &color,
        .viewport_count = 1,
    };
    *step = ts_key ? "compile real task + mesh graphics pipeline" : "compile real taskless mesh graphics pipeline";
    result = dm_m4_pipeline_create(device, &info, &pipeline);
done:
    dm_m4_pipeline_destroy(device, pipeline);
    if (layout) destroy(device->device, layout, NULL);
    dm_m4_shader_destroy(device, &ts);
    dm_m4_shader_destroy(device, &ps);
    dm_m4_shader_destroy(device, &ms);
    return result;
}

static int run_mesh_smoke(const wchar_t *directory)
{
    static const uint8_t mesh[][8] = {
        {0x0c, 0x1f, 0xfe, 0xcd, 0x02, 0x01, 0x8f, 0xb0},
        {0xbc, 0x9d, 0x26, 0xe2, 0x9a, 0x45, 0xe6, 0xb6},
    };
    static const uint8_t fragment[][8] = {
        {0x7a, 0xd8, 0x84, 0x3a, 0xfe, 0x10, 0x5a, 0x32},
        {0xc5, 0x6d, 0x95, 0x81, 0x47, 0xf5, 0xab, 0xd3},
    };
    static const uint8_t task[] = {0x47, 0xf9, 0xb3, 0x84, 0x90, 0xdc, 0xf9, 0xa2};
    static const uint32_t mesh_caps[] = {1, 10, 11, 25, 46, 47, 50, 62, 64, 65, 5283, 5347};
    static const uint32_t fragment_caps[] = {1, 10, 25, 46, 47, 50, 62, 64, 65, 5378};
    const struct dm_m4_draw_capabilities caps[] = {
        {VK_SHADER_STAGE_MESH_BIT_EXT, sizeof mesh_caps / sizeof *mesh_caps, mesh_caps},
        {VK_SHADER_STAGE_TASK_BIT_EXT, sizeof mesh_caps / sizeof *mesh_caps, mesh_caps},
        {VK_SHADER_STAGE_FRAGMENT_BIT, sizeof fragment_caps / sizeof *fragment_caps, fragment_caps},
    };
    struct dm_m4_draw_requirements requirements;
    struct dm_m4_window *window = NULL;
    const char *step = NULL;
    VkResult result = dm_m4_draw_requirements_init(&requirements, 0, caps, sizeof caps / sizeof *caps, &step);
    if (result == VK_SUCCESS) {
        step = "create Vulkan mesh device";
        result = dm_m4_window_create(&window, L"Native mesh pipeline smoke", 320, 180, &requirements.requirements);
    }
    if (result == VK_SUCCESS) {
        const struct dm_m4_device *device = dm_m4_window_get_device(window);
        result = mesh_smoke_pair(device, directory, mesh[0], fragment[0], NULL, &step);
        if (result == VK_SUCCESS) result = mesh_smoke_pair(device, directory, mesh[1], fragment[1], task, &step);
    }
    if (window) dm_m4_window_destroy(window);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "%s: VkResult %d\n", step, (int)result);
        return 1;
    }
    puts("real taskless mesh and task + mesh graphics pipelines compiled");
    return 0;
}
