// Standalone observer fixture: the same black upload can come from either inside
// or outside the final blit's raster rectangle. Only the former is a linked pair.
#include "m4_overlay.h"
#include <string.h>

int main(int argc, char **argv) {
    const int outside = argc == 2 && !strcmp(argv[1], "outside");
    const uint8_t vk[8] = {1}, pk[8] = {2}, bk[8] = {3};
    const struct dm_m4_replay_keys pane_keys = {.vs = vk, .ps = pk}, blit_keys = {.vs = vk, .ps = bk};
    const struct dm_m4_shader_binding buf = {.binding = 1, .count = 1, .source = DM_M4_SHADER_CBUF},
        tex = {.binding = 1, .count = 1, .source = DM_M4_SHADER_TEXTURE};
    const struct dm_m4_shader pane_shader = {.binding_count = 1, .bindings = &buf},
        blit_shader = {.binding_count = 1, .bindings = &tex};
    float alpha = 1.0f;
    const struct dm_m4_draw_binding material = {.buffer = {&alpha, sizeof alpha}};
    VkPipelineColorBlendAttachmentState blend = {.blendEnable = VK_TRUE, .colorWriteMask = 15,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};
    const VkPipelineColorBlendStateCreateInfo state = {.attachmentCount = 1, .pAttachments = &blend};
    uint32_t pane[4] = {0xff000000, 0x00201008, 0, 0x00000001}, sink[8] = {0}, upload[4] = {0};
    const VkExtent2D crop = {2, 2};
    struct dm_m4_draw_info draw = {.vs = {&pane_shader, &material}, .ps = {&pane_shader, &material},
        .color_count = 1, .extent = {2, 2}, .scissor = {{0, 0}, {2, 2}}, .viewport = {0, 2, 2, -2, 0, 1}};
    draw.colors[0] = (struct dm_m4_draw_attachment){.format = VK_FORMAT_R8G8B8A8_UNORM,
        .width = 2, .height = 2, .pixels = {pane, sizeof pane, 0}};
    draw.pipeline.blend = &state;
    const struct dm_m4_draw_sample_upload backing = {.encoded = (const uint8_t *)pane,
        .encoded_size = sizeof pane, .layout.pitch = 8};
    const struct dm_m4_draw_sampled_image image = {.upload = &backing, .width = 2, .height = 2,
        .level_count = 1, .format = VK_FORMAT_R8G8B8A8_UNORM};
    const struct dm_m4_draw_binding sample = {.texture = &image};
    struct dm_m4_draw_info blit = draw;
    blit.ps = (struct dm_m4_draw_stage){&blit_shader, &sample};
    blit.colors[0].width = blit.extent.width = 4;
    blit.colors[0].pixels = (struct dm_m4_draw_mutable_pixels){sink, sizeof sink, 0};
    blit.scissor.offset.x = 1;
    blit.viewport.x = 1;
    struct dm_m4_overlay_frame frame = {.guest_frame = 90, .storage = sink, .x = outside ? 2u : 1u};
    struct dm_m4_overlay *c = dm_m4_overlay_open();
    if (!c) return 1;
    dm_m4_overlay_present(c, &frame, pane, crop, crop, 0);
    for (unsigned i = 0; i < 2; ++i) {
        ++frame.guest_frame;
        alpha = i ? .75f : 1.f;
        blend.blendEnable = VK_TRUE;
        enum dm_m4_overlay_role role = dm_m4_overlay_select(c, &pane_keys);
        if (role != DM_M4_OVERLAY_PANE || !dm_m4_overlay_draw_begin(c, role, &draw, pane)) return 2;
        memset(pane, 0, sizeof pane);
        dm_m4_overlay_draw_end(c, role, &draw, 1);
        blend.blendEnable = VK_FALSE;
        role = dm_m4_overlay_select(c, &blit_keys);
        if (role != DM_M4_OVERLAY_BLIT || !dm_m4_overlay_draw_begin(c, role, &blit, sink)) return 3;
        dm_m4_overlay_draw_end(c, role, &blit, 1);
        dm_m4_overlay_present(c, &frame, upload, crop, crop, 0);
    }
    dm_m4_overlay_close(c);
    return 0;
}
