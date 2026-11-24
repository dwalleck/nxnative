// Layout2 variation 67 pipeline creation and staged graphics/compute readback.
// Run m4_shader_smoke.exe <Windows shader library> after --reflect-existing
// or a fresh win_nvn_shaders.py build.
#define VK_NO_PROTOTYPES
#include "m4_draw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "m4_draw_smoke.inc"
#include "m4_compute_smoke.inc"
#include "m4_bc4_smoke.inc"
#include "m4_gmem_smoke.inc"
#include "m4_mesh_smoke.inc"
#include "m4_rate_smoke.inc"

int wmain(int argc, wchar_t **argv)
{
    if (argc == 2 && !wcscmp(argv[1], L"--shading-rate"))
        return run_rate_smoke();
    if (argc == 3 && !wcscmp(argv[1], L"--gmem"))
        return run_gmem_smoke(argv[2]);
    if (argc == 3 && !wcscmp(argv[1], L"--mesh"))
        return run_mesh_smoke(argv[2]);
    const int bc4 = argc == 3 && !wcscmp(argv[1], L"--bc4");
    if (argc != 2 && !bc4) {
        fputs("usage: m4_shader_smoke <Windows shader library> | --mesh/--gmem/--bc4 <smoke shader library> | --shading-rate\n", stderr);
        return 2;
    }
    static const uint8_t vs_key[8] = {0x01, 0x15, 0xb5, 0x70, 0x97, 0xac, 0x7e, 0xcd};
    static const uint8_t ps_key[8] = {0x43, 0xae, 0x88, 0xe5, 0x44, 0x14, 0xd5, 0x46};
    static const uint32_t vs_caps[] = {1, 10, 25, 46, 47, 50, 62, 64, 65, 4427};
    static const uint32_t ps_caps[] = {1, 10, 25, 46, 47, 50, 62, 64, 65, 5378};
    static const uint32_t cs_caps[] = {1, 46, 47};
    const struct dm_m4_draw_capabilities caps[] = {
        {VK_SHADER_STAGE_VERTEX_BIT, sizeof vs_caps / sizeof *vs_caps, vs_caps},
        {VK_SHADER_STAGE_FRAGMENT_BIT, sizeof ps_caps / sizeof *ps_caps, ps_caps},
        {VK_SHADER_STAGE_COMPUTE_BIT, sizeof cs_caps / sizeof *cs_caps, cs_caps},
    };
    struct dm_m4_draw_requirements requirements;
    const char *step = NULL;
    VkResult result = dm_m4_draw_requirements_init(&requirements, DM_M4_DRAW_NEGATIVE_ONE_TO_ONE |
                                                     DM_M4_DRAW_INDEPENDENT_BLEND | DM_M4_DRAW_VERTEX_STORAGE_WRITES |
                                                     (bc4 ? 0u : DM_M4_DRAW_WIDE_LINES | DM_M4_DRAW_LARGE_POINTS),
                                                    caps, sizeof caps / sizeof *caps, &step);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "%s: VkResult %d\n", step, (int)result);
        return 1;
    }
    struct dm_m4_window *window = NULL;
    step = "create Vulkan device";
    result = dm_m4_window_create(&window, L"Layout2 drawing smoke", 320, 180, &requirements.requirements);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "%s: VkResult %d\n", step, (int)result);
        return 1;
    }
    const struct dm_m4_device *device = dm_m4_window_get_device(window);
    struct dm_m4_shader vs = {0}, ps = {0}, wrong_stage = {0};
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    PFN_vkCreatePipelineLayout create_layout = (PFN_vkCreatePipelineLayout)
        device->get_device_proc(device->device, "vkCreatePipelineLayout");
    PFN_vkDestroyPipelineLayout destroy_layout = (PFN_vkDestroyPipelineLayout)
        device->get_device_proc(device->device, "vkDestroyPipelineLayout");
    PFN_vkGetPhysicalDeviceProperties2 get_properties = (PFN_vkGetPhysicalDeviceProperties2)
        device->get_instance_proc(device->instance, "vkGetPhysicalDeviceProperties2");
    step = "load Vulkan dispatch";
    if (!create_layout || !destroy_layout || !get_properties) {
        result = VK_ERROR_INITIALIZATION_FAILED;
        goto done;
    }
    VkPhysicalDeviceSubgroupProperties subgroup = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                               .pNext = &subgroup};
    get_properties(device->physical, &properties);
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkSubgroupFeatureFlags operations = VK_SUBGROUP_FEATURE_VOTE_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT |
                                             VK_SUBGROUP_FEATURE_SHUFFLE_BIT;
    step = "check vertex/fragment subgroup capabilities";
    if ((subgroup.supportedStages & stages) != stages || (subgroup.supportedOperations & operations) != operations) {
        result = VK_ERROR_FEATURE_NOT_PRESENT;
        goto done;
    }
    if (bc4) {
        result = draw_bc4_smoke(device, &requirements, argv[2], &step);
        goto done;
    }
    step = "load 0115b57097ac7ecd (VS)";
    result = dm_m4_shader_load(device, argv[1], vs_key, VK_SHADER_STAGE_VERTEX_BIT, &vs);
    if (result != VK_SUCCESS)
        goto done;
    step = "load 43ae88e54414d546 (PS)";
    result = dm_m4_shader_load(device, argv[1], ps_key, VK_SHADER_STAGE_FRAGMENT_BIT, &ps);
    if (result != VK_SUCCESS)
        goto done;
    step = "check stage-local binding recipes";
    if (vs.set != 0 || ps.set != 1 || !vs.c1_size || !ps.c1_size ||
        vs.binding_count != 5 || ps.binding_count != 6 ||
        vs.bindings[1].source != DM_M4_SHADER_SBUF || vs.bindings[1].sbuf.cb_offset != 92 ||
        ps.bindings[1].source != DM_M4_SHADER_TEXTURE || ps.bindings[1].image.cbuf_slot != -1 ||
        ps.bindings[1].image.handle != 24 ||
        ps.bindings[1].descriptor_type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || ps.frag_write_mask != 15) {
        result = VK_ERROR_INVALID_SHADER_NV;
        goto done;
    }
    step = "reject wrong stage for retained key";
    result = dm_m4_shader_load(device, argv[1], vs_key, VK_SHADER_STAGE_FRAGMENT_BIT, &wrong_stage);
    if (result != VK_ERROR_INVALID_SHADER_NV || wrong_stage.module || wrong_stage.set_layout) {
        result = VK_ERROR_UNKNOWN;
        goto done;
    }
    const VkDescriptorSetLayout sets[] = {vs.set_layout, ps.set_layout};
    const VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = sizeof(sets) / sizeof(sets[0]),
        .pSetLayouts = sets,
    };
    step = "create two-set pipeline layout";
    VkPipelineLayout created;
    result = create_layout(device->device, &layout_info, NULL, &created);
    if (result != VK_SUCCESS)
        goto done;
    layout = created;

    // The translated VS takes vec4 inputs at locations 0 and 1; the PS
    // returns vec4 at location 0. No guest geometry or draw is needed here.
    const VkVertexInputBindingDescription stream = {.stride = 32, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[] = {
        {.location = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT},
        {.location = 1, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 16},
    };
    const VkPipelineVertexInputStateCreateInfo vertex_input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &stream,
        .vertexAttributeDescriptionCount = sizeof(attributes) / sizeof(attributes[0]),
        .pVertexAttributeDescriptions = attributes,
    };
    const VkPipelineRasterizationStateCreateInfo rasterization = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineColorBlendAttachmentState attachment = {.colorWriteMask = ps.frag_write_mask};
    const VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &attachment,
    };
    const VkFormat color = VK_FORMAT_R8G8B8A8_UNORM;
    const struct dm_m4_pipeline_info info = {
        .layout = layout, .vs = vs.module, .ps = ps.module,
        .vs_entry = DM_M4_SHADER_ENTRY, .ps_entry = DM_M4_SHADER_ENTRY,
        .vertex_input = &vertex_input, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .rasterization = &rasterization, .multisample = &multisample,
        .blend = &blend, .color_formats = &color, .viewport_count = 1,
        .negative_one_to_one = VK_TRUE,
    };
    step = "vkCreateGraphicsPipelines for Layout2 variation 67";
    result = dm_m4_pipeline_create(device, &info, &pipeline);
    if (result == VK_SUCCESS && !pipeline)
        result = VK_ERROR_INITIALIZATION_FAILED;
    if (result == VK_SUCCESS)
        result = draw_pixels_smoke(device, &requirements, VK_IMAGE_VIEW_TYPE_2D, 0, &step);
    if (result == VK_SUCCESS)
        result = draw_pixels_smoke(device, &requirements, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 0, &step);
    if (result == VK_SUCCESS)
        result = draw_pixels_smoke(device, &requirements, VK_IMAGE_VIEW_TYPE_3D, 0, &step);
    if (result == VK_SUCCESS)
        result = draw_pixels_smoke(device, &requirements, VK_IMAGE_VIEW_TYPE_CUBE, 0, &step);
    if (result == VK_SUCCESS)
        result = draw_pixels_smoke(device, &requirements, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, &step);
    if (result == VK_SUCCESS)
        result = compute_pixels_smoke(device, &requirements, 0, &step);
    if (result == VK_SUCCESS)
        result = compute_pixels_smoke(device, &requirements, 1, &step);

done:
    dm_m4_pipeline_destroy(device, pipeline);
    if (layout)
        destroy_layout(device->device, layout, NULL);
    dm_m4_shader_destroy(device, &wrong_stage);
    dm_m4_shader_destroy(device, &ps);
    dm_m4_shader_destroy(device, &vs);
    dm_m4_window_destroy(window);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "%s: VkResult %d\n", step, (int)result);
        return 1;
    }
    if (bc4) {
        puts("Captured BC4 encoder writes integer blocks from both volume slices");
        return 0;
    }
    printf("Layout2 pipeline, line/point coverage, sparse/depth-only/attachmentless draws, 2D/array/3D/cube/shadow sampling, and compute readback passed on %s\n",
           properties.properties.deviceName);
    return 0;
}
