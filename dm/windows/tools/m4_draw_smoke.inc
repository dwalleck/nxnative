// Minimal SPIR-V 1.0 fixtures to avoid requiring a shader compiler to run the
// Windows smoke. VS forwards location 0 to Position and defaults PointSize to 1;
// PS samples set 1 binding 1 at (0.5, 0.5), a layer with or without depth comparison,
// a cube face, or a volume slice.
#define OP(words, opcode) (((words) << 16u) | (opcode))
static const uint32_t position_spv[] = {
    0x07230203, 0x00010000, 0, 21, 0,
    OP(2, 17), 1,                               // OpCapability Shader
    OP(3, 14), 0, 1,                            // OpMemoryModel Logical GLSL450
    OP(7, 15), 0, 9, 0x6e69616d, 0, 7, 8,       // OpEntryPoint Vertex %9 "main" %7 %8
    OP(4, 71), 7, 30, 0,                        // OpDecorate %7 Location 0
    OP(3, 71), 12, 2,                           // OpDecorate %12 Block (gl_PerVertex)
    OP(5, 72), 12, 0, 11, 0,                     // OpMemberDecorate %12 0 BuiltIn Position
    OP(5, 72), 12, 1, 11, 1,                     // OpMemberDecorate %12 1 BuiltIn PointSize
    OP(2, 19), 1,                               // %1 = OpTypeVoid
    OP(3, 22), 2, 32,                           // %2 = OpTypeFloat 32
    OP(4, 23), 3, 2, 4,                         // %3 = OpTypeVector %2 4
    OP(4, 32), 4, 1, 3,                         // %4 = OpTypePointer Input %3
    OP(4, 30), 12, 3, 2,                         // %12 = OpTypeStruct %3 %2
    OP(4, 32), 5, 3, 12,                         // %5 = OpTypePointer Output %12
    OP(4, 32), 13, 3, 3,                         // %13 = OpTypePointer Output %3
    OP(4, 32), 14, 3, 2,                         // %14 = OpTypePointer Output %2
    OP(4, 21), 15, 32, 0,                        // %15 = OpTypeInt 32 0
    OP(4, 43), 15, 16, 0,                        // %16 = OpConstant %15 0
    OP(4, 43), 15, 17, 1,                        // %17 = OpConstant %15 1
    OP(4, 43), 2, 18, 0x3f800000,                // %18 = OpConstant %2 1.0
    OP(3, 33), 6, 1,                            // %6 = OpTypeFunction %1
    OP(4, 59), 4, 7, 1,                         // %7 = OpVariable %4 Input
    OP(4, 59), 5, 8, 3,                         // %8 = OpVariable %5 Output
    OP(5, 54), 1, 9, 0, 6,                      // %9 = OpFunction %1 None %6
    OP(2, 248), 10,                             // %10 = OpLabel
    OP(4, 61), 3, 11, 7,                        // %11 = OpLoad %3 %7
    OP(5, 65), 13, 19, 8, 16,                    // %19 = OpAccessChain %13 %8 %16
    OP(3, 62), 19, 11,                           // OpStore %19 %11
    OP(5, 65), 14, 20, 8, 17,                    // %20 = OpAccessChain %14 %8 %17
    OP(3, 62), 20, 18,                           // OpStore %20 %18
    OP(1, 253), OP(1, 56),                      // OpReturn, OpFunctionEnd
};
// Vertex atomic OR through set 0 binding 1, with position forwarded for the
// zero-coverage test that keeps rasterization enabled.
static const uint32_t vertex_store_spv[] = {
    0x07230203, 0x00010000, 0, 24, 0,
    OP(2, 17), 1,                               // OpCapability Shader
    OP(3, 14), 0, 1,                            // OpMemoryModel Logical GLSL450
    OP(7, 15), 0, 13, 0x6e69616d, 0, 21, 22,    // OpEntryPoint Vertex %13 "main" %21 %22
    OP(3, 71), 5, 3,                            // OpDecorate %5 BufferBlock
    OP(5, 72), 5, 0, 35, 0,                     // OpMemberDecorate %5 0 Offset 0
    OP(4, 71), 8, 34, 0,                        // OpDecorate %8 DescriptorSet 0
    OP(4, 71), 8, 33, 1,                        // OpDecorate %8 Binding 1
    OP(4, 71), 21, 30, 0,                       // OpDecorate %21 Location 0
    OP(4, 71), 22, 11, 0,                       // OpDecorate %22 BuiltIn Position
    OP(2, 19), 1,                               // %1 = OpTypeVoid
    OP(4, 21), 2, 32, 0,                        // %2 = OpTypeInt 32 0
    OP(3, 33), 3, 1,                            // %3 = OpTypeFunction %1
    OP(3, 30), 5, 2,                            // %5 = OpTypeStruct %2
    OP(4, 32), 6, 2, 5,                         // %6 = OpTypePointer Uniform %5
    OP(4, 32), 7, 2, 2,                         // %7 = OpTypePointer Uniform %2
    OP(4, 43), 2, 9, 0,                         // %9 = OpConstant %2 0 (member and relaxed semantics)
    OP(4, 43), 2, 10, 0x00ab0023,               // %10 = OpConstant %2 0x00ab0023
    OP(4, 43), 2, 11, 1,                        // %11 = OpConstant %2 1 (Device scope)
    OP(4, 59), 6, 8, 2,                         // %8 = OpVariable %6 Uniform
    OP(3, 22), 17, 32,                          // %17 = OpTypeFloat 32
    OP(4, 23), 18, 17, 4,                       // %18 = OpTypeVector %17 4
    OP(4, 32), 19, 1, 18,                       // %19 = OpTypePointer Input %18
    OP(4, 32), 20, 3, 18,                       // %20 = OpTypePointer Output %18
    OP(4, 59), 19, 21, 1,                       // %21 = OpVariable %19 Input
    OP(4, 59), 20, 22, 3,                       // %22 = OpVariable %20 Output
    OP(5, 54), 1, 13, 0, 3,                     // %13 = OpFunction %1 None %3
    OP(2, 248), 14,                             // %14 = OpLabel
    OP(5, 65), 7, 15, 8, 9,                     // %15 = OpAccessChain %7 %8 %9
    OP(7, 241), 2, 16, 15, 11, 9, 10,           // %16 = OpAtomicOr %2 %15 %11 %9 %10
    OP(4, 61), 18, 23, 21,                      // %23 = OpLoad %18 %21
    OP(3, 62), 22, 23,                          // OpStore %22 %23
    OP(1, 253), OP(1, 56),                      // OpReturn, OpFunctionEnd
};

static VkResult draw_sizes_smoke(struct dm_m4_draw *draw, const struct dm_m4_draw_info *base, const char **step)
{
    const uint32_t polygon_defaults = 0x14;
    NVNpolygonState polygon;
    memcpy(&polygon, &polygon_defaults, sizeof polygon);
    VkPipelineRasterizationStateCreateInfo raster;
    *step = "NVN fill ignores unrelated line width";
    if (dm_m4_nvn_rasterization(&polygon, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 0, 3.f, 0, 0, 0, &raster) ||
        raster.lineWidth != 1.f)
        return VK_ERROR_UNKNOWN;

    const float vertices[][4] = {{-.125f, .125f, .25f, 1.f}, {.625f, .125f, .25f, 1.f}};
    const uint16_t indices[] = {0, 1};
    float depth[8][8];
    struct dm_m4_draw_info info = *base;
    info.samples_passed = NULL;
    info.ps = (struct dm_m4_draw_stage){0};
    info.color_count = 0;
    info.pipeline.blend = NULL;
    info.pipeline.rasterization = &raster;
    info.depth.pixels = (struct dm_m4_draw_mutable_pixels){depth, sizeof depth, 0};
    info.indices = (struct dm_m4_draw_span){indices, sizeof indices};
    info.index_type = VK_INDEX_TYPE_UINT16;
    info.vertices[0] = (struct dm_m4_draw_span){vertices, sizeof vertices};
    const float sizes[] = {1.f, 3.f, 1.f};
    for (unsigned point = 0; point < 2; ++point) {
        const NVNdrawPrimitive primitive = point ? NVN_DRAW_PRIMITIVE_POINTS : NVN_DRAW_PRIMITIVE_LINES;
        info.pipeline.topology = dm_m4_nvn_topology(primitive);
        info.index_count = point ? 1 : 2;
        for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; ++i) {
            for (unsigned y = 0; y < 8; ++y)
                for (unsigned x = 0; x < 8; ++x) depth[y][x] = 1.f;
            *step = "convert NVN line or point raster state";
            if (dm_m4_nvn_rasterization(&polygon, primitive, 0, 0, sizes[i], 0, 0, 0, &raster))
                return VK_ERROR_UNKNOWN;
            info.point_size = point && sizes[i] > 1.f ? sizes[i] : 0.f;
            VkResult result = dm_m4_draw_submit(draw, &info, step);
            if (result != VK_SUCCESS) return result;
            *step = point ? "point-size change and restored PSO depth coverage" : "line-width change and restored PSO depth coverage";
            unsigned rows = 0;
            const unsigned x = point ? 3 : 4;
            for (unsigned y = 0; y < 8; ++y) rows += depth[y][x] == .25f;
            if (rows != (unsigned)sizes[i] || depth[4][x] != .25f || depth[0][x] != 1.f ||
                (point && (depth[4][2] == .25f) != (sizes[i] > 1.f)))
                return VK_ERROR_UNKNOWN;
        }
        if (point) info.point_size = 1.e30f;
        else raster.lineWidth = 1.e30f;
        VkResult result = dm_m4_draw_submit(draw, &info, step);
        if (result != VK_ERROR_FEATURE_NOT_PRESENT ||
            strcmp(*step, point ? "draw large points not enabled or outside device range" :
                                  "draw wide lines not enabled or outside device range")) {
            *step = "reject dimensions beyond the physical device raster range";
            return VK_ERROR_UNKNOWN;
        }
        info.point_size = 0;
    }
    return VK_SUCCESS;
}

static VkResult draw_restart_smoke(struct dm_m4_draw *draw, const struct dm_m4_draw_info *base, const char **step)
{
    // The array reference starts at vertex 1. Indexed draws use a negative
    // base vertex so the index type's max maps to vertex 7 (a copy of vertex 6).
    enum { SIDE = 8, CUSTOM8 = 248, CUSTOM16 = 65528 };
    static const float vertices[][4] = {
        {0.f, .75f, .125f, 1.f},
        {-.9375f, -.75f, .25f, 1.f}, {-.25f, -.75f, .25f, 1.f}, {-.9375f, .75f, .25f, 1.f},
        {.25f, -.75f, .5f, 1.f}, {.9375f, -.75f, .5f, 1.f}, {.9375f, .75f, .5f, 1.f},
        {.9375f, .75f, .5f, 1.f},
    };
    static const uint8_t fixed8[] = {
        UINT8_MAX, 249, 250, 251, UINT8_MAX, UINT8_MAX, 252, 253, 254, UINT8_MAX,
    };
    static const uint8_t custom8[] = {249, 250, 251, CUSTOM8, CUSTOM8, 252, 253, UINT8_MAX};
    static const uint16_t fixed16[] = {
        UINT16_MAX, 65529, 65530, 65531, UINT16_MAX, UINT16_MAX, 65532, 65533, 65534, UINT16_MAX,
    };
    static const uint16_t custom16[] = {65529, 65530, 65531, CUSTOM16, CUSTOM16, 65532, 65533, UINT16_MAX};
    // Repeated ordinary indices make a degenerate connector without restart.
    static const uint16_t wide16[] = {65529, 65530, 65531, 65531, 65532, 65532, 65533, UINT16_MAX};
    // Restart must discard the two indices of a partial list primitive.
    static const uint16_t list16[] = {65529, 65530, 65531, 65529, 65531, CUSTOM16, 65532, 65533, UINT16_MAX};
    static const struct {
        const char *step;
        struct dm_m4_draw_span indices;
        VkIndexType type;
        VkPrimitiveTopology topology;
        uint32_t marker;
        VkBool32 enable;
    } cases[] = {
        {"verify UINT8 fixed restart with repeated boundaries", {fixed8, sizeof fixed8},
         VK_INDEX_TYPE_UINT8_EXT, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, UINT8_MAX, VK_TRUE},
        {"verify UINT8 custom restart preserves index 0xff", {custom8, sizeof custom8},
         VK_INDEX_TYPE_UINT8_EXT, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, CUSTOM8, VK_TRUE},
        {"verify UINT16 fixed restart with repeated boundaries", {fixed16, sizeof fixed16},
         VK_INDEX_TYPE_UINT16, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, UINT16_MAX, VK_TRUE},
        {"verify disabled restart draws across a fixed marker", {fixed16, sizeof fixed16},
         VK_INDEX_TYPE_UINT16, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, UINT16_MAX, VK_FALSE},
        {"verify UINT16 custom restart preserves index 0xffff", {custom16, sizeof custom16},
         VK_INDEX_TYPE_UINT16, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, CUSTOM16, VK_TRUE},
        {"verify over-width restart marker does not match index 0xffff", {wide16, sizeof wide16},
         VK_INDEX_TYPE_UINT16, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, 0x1ffffu, VK_TRUE},
        {"verify list restart discards a partial triangle", {list16, sizeof list16},
         VK_INDEX_TYPE_UINT16, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, CUSTOM16, VK_TRUE},
    };
    float reference[SIDE][SIDE], depth[SIDE][SIDE];
    for (unsigned y = 0; y < SIDE; ++y)
        for (unsigned x = 0; x < SIDE; ++x) reference[y][x] = 1.f;
    struct dm_m4_draw_info info = *base;
    info.samples_passed = NULL;
    info.ps = (struct dm_m4_draw_stage){0};
    info.color_count = 0;
    info.pipeline.blend = NULL;
    info.pipeline.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    info.depth.pixels = (struct dm_m4_draw_mutable_pixels){reference, sizeof reference, 0};
    info.vertices[0] = (struct dm_m4_draw_span){vertices, sizeof vertices};
    info.indices = (struct dm_m4_draw_span){0};
    info.index_count = 0;
    info.vertex_count = 6;
    info.base_vertex = 1;
    // Restart is ignored by nonindexed draws even if the marker is in the vertex range.
    info.pipeline.primitive_restart_enable = VK_TRUE;
    info.restart_index = 2;
    VkResult result = dm_m4_draw_submit(draw, &info, step);
    if (result != VK_SUCCESS) return result;
    *step = "verify two reference triangles leave a clear gap";
    if (reference[2][1] != .25f || reference[2][6] != .5f || reference[2][3] != 1.f)
        return VK_ERROR_UNKNOWN;
    for (unsigned y = 0; y < SIDE; ++y)
        for (unsigned x = 0; x < SIDE; ++x)
            if (reference[y][x] != .25f && reference[y][x] != .5f && reference[y][x] != 1.f)
                return VK_ERROR_UNKNOWN;

    info.depth.pixels = (struct dm_m4_draw_mutable_pixels){depth, sizeof depth, 0};
    // Same draw and staging layout, but the second triangle is outside the
    // NVN stream. Neither adjacent guest vertices nor the previous upload
    // may leak into this fetch.
    for (unsigned y = 0; y < SIDE; ++y)
        for (unsigned x = 0; x < SIDE; ++x) depth[y][x] = 1.f;
    info.vertices[0].size = 4 * sizeof vertices[0];
    info.vertex_zero_tail[0] = 3 * sizeof vertices[0];
    result = dm_m4_draw_submit(draw, &info, step);
    if (result != VK_SUCCESS) return result;
    *step = "vertex fetches past the bound stream return zero, not adjacent vertices";
    for (unsigned y = 0; y < SIDE; ++y)
        for (unsigned x = 0; x < SIDE; ++x)
            if (depth[y][x] != (reference[y][x] == .25f ? .25f : 1.f)) return VK_ERROR_UNKNOWN;
    info.vertices[0].size = sizeof vertices;
    info.vertex_zero_tail[0] = 0;
    info.vertex_count = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; ++i) {
        for (unsigned y = 0; y < SIDE; ++y)
            for (unsigned x = 0; x < SIDE; ++x) depth[y][x] = 1.f;
        const int byte = cases[i].type == VK_INDEX_TYPE_UINT8_EXT;
        info.indices = cases[i].indices;
        info.index_type = cases[i].type;
        info.index_count = (uint32_t)(info.indices.size / (byte ? sizeof(uint8_t) : sizeof(uint16_t)));
        info.base_vertex = 7 - (int32_t)(byte ? UINT8_MAX : UINT16_MAX);
        info.pipeline.topology = cases[i].topology;
        info.pipeline.primitive_restart_enable = cases[i].enable;
        info.restart_index = cases[i].marker;
        result = dm_m4_draw_submit(draw, &info, step);
        if (result != VK_SUCCESS) return result;
        *step = cases[i].step;
        if (cases[i].enable) {
            if (memcmp(depth, reference, sizeof depth)) return VK_ERROR_UNKNOWN;
        } else {
            // With restart disabled, the same fixed16 stream covers the gap.
            if (depth[2][1] != .25f || depth[2][6] != .5f ||
                !(depth[2][3] > .25f && depth[2][3] < .5f))
                return VK_ERROR_UNKNOWN;
            for (unsigned y = 0; y < SIDE; ++y)
                for (unsigned x = 0; x < SIDE; ++x)
                    if (!(depth[y][x] >= 0.f && depth[y][x] <= reference[y][x]))
                        return VK_ERROR_UNKNOWN;
        }
    }
    return VK_SUCCESS;
}

struct checked_smoke_upload {
    const struct dm_m4_draw_pixels *planes;
    unsigned materializations;
};

static const char *checked_smoke_materialize(void *context, const struct dm_m4_draw_pixels **planes)
{
    struct checked_smoke_upload *upload = context;
    ++upload->materializations;
    *planes = upload->planes;
    return NULL;
}

static VkResult attachment_writes_smoke(struct dm_m4_draw *draw, const struct dm_m4_device *device,
                                        const struct dm_m4_draw_info *base, unsigned location,
                                        const uint8_t expected_color[4], unsigned visible, const char **step)
{
    enum { SIDE = 8, COLOR_PITCH = SIDE * 4 + 4 };
    PFN_vkGetPhysicalDeviceFormatProperties get_format = (PFN_vkGetPhysicalDeviceFormatProperties)
        device->get_instance_proc(device->instance, "vkGetPhysicalDeviceFormatProperties");
    *step = "select a packed depth/stencil format for hardware readback";
    if (!get_format) return VK_ERROR_INITIALIZATION_FAILED;
    VkFormatProperties props;
    const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    get_format(device->physical, VK_FORMAT_D32_SFLOAT_S8_UINT, &props);
    const int z32 = (props.optimalTilingFeatures & required) == required;
    const VkFormat format = z32 ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D24_UNORM_S8_UINT;
    const uint32_t far_z = z32 ? 0x3f800000u : 0xffffffu;

    uint8_t color[SIDE][COLOR_PITCH], masked[SIDE][COLOR_PITCH], stencil[SIDE][SIDE + 1];
    uint8_t stencil_before[sizeof stencil];
    uint32_t depth[SIDE][SIDE + 1], depth_before[SIDE][SIDE + 1];
    memset(color, 0x72, sizeof color);
    memset(masked, 0xa5, sizeof masked);
    memset(stencil, 0xc7, sizeof stencil);
    for (unsigned y = 0; y < SIDE; ++y) {
        memset(stencil[y], 0x35, SIDE);
        for (unsigned x = 0; x < SIDE; ++x) depth[y][x] = far_z;
        depth[y][SIDE] = 0xfeedbad1u;
    }
    depth[2][2] = 0; // A covered sample fails depth.
    stencil[2][3] = 0x5f; // A different covered sample fails stencil.
    memcpy(depth_before, depth, sizeof depth);
    memcpy(stencil_before, stencil, sizeof stencil);

    VkPipelineDepthStencilStateCreateInfo zs = *base->pipeline.depth_stencil;
    zs.depthWriteEnable = VK_FALSE;
    zs.stencilTestEnable = VK_TRUE;
    zs.front = (VkStencilOpState){.compareOp = VK_COMPARE_OP_EQUAL,
        .compareMask = 0xff, .writeMask = 0xff, .reference = 0x35};
    zs.back = zs.front;
    zs.back.passOp = VK_STENCIL_OP_REPLACE;
    zs.back.writeMask = 0;
    struct dm_m4_draw_info info = *base;
    VkPipelineColorBlendAttachmentState masks[DM_M4_NVN_COLOR_TARGETS];
    VkPipelineColorBlendStateCreateInfo blend = *base->pipeline.blend;
    memcpy(masks, blend.pAttachments, blend.attachmentCount * sizeof *masks);
    blend.pAttachments = masks;
    uint64_t samples = 0;
    struct dm_m4_draw_attachment_writes writes;
    info.samples_passed = &samples;
    info.attachment_writes = &writes;
    info.pipeline.blend = &blend;
    info.pipeline.depth_stencil = &zs;
    info.viewport.minDepth = info.viewport.maxDepth = 0.0f;
    info.colors[0].pixels = (struct dm_m4_draw_mutable_pixels){masked, sizeof masked, COLOR_PITCH};
    info.colors[location].pixels = (struct dm_m4_draw_mutable_pixels){color, sizeof color, COLOR_PITCH};
    info.depth = (struct dm_m4_draw_attachment){.format = format, .width = SIDE, .height = SIDE,
        .pixels = {depth, sizeof depth, sizeof depth[0]}, .stencil = {stencil, sizeof stencil, sizeof stencil[0]}};
    // Exercise guest-facing token handoff only when no CPU write intervened.
    // The depth plane changes on the second draw; the third consumes it.
    static uint64_t sequence;
    info.depth.output_token = ++sequence;

    VkResult result = dm_m4_draw_submit(draw, &info, step);
    if (result != VK_SUCCESS) return result;
    *step = "verify active color and occlusion with read-only packed depth/stencil and disabled MRT";
    if (samples != visible - 2u || writes.colors != (1u << location) || writes.depth_stencil ||
        memcmp(&color[2][4 * 4], expected_color, 4) || color[2][4 * 2] != 0x72 || color[2][4 * 3] != 0x72 ||
        memcmp(depth, depth_before, sizeof depth) || memcmp(stencil, stencil_before, sizeof stencil))
        return VK_ERROR_UNKNOWN;
    for (unsigned y = 0; y < SIDE; ++y)
        for (unsigned x = 0; x < COLOR_PITCH; ++x)
            if (masked[y][x] != 0xa5) return VK_ERROR_UNKNOWN;

    memset(color, 0x72, sizeof color);
    masks[location].colorWriteMask = 0; // Mask a location the shader actually writes.
    zs.depthWriteEnable = VK_TRUE;
    // Stencil writes the reference back to samples that already equal it.
    zs.front.passOp = VK_STENCIL_OP_REPLACE;
    zs.back = zs.front;
    info.depth.input_token = info.depth.output_token;
    info.depth.output_token = ++sequence;
    result = dm_m4_draw_submit(draw, &info, step);
    if (result != VK_SUCCESS) return result;
    *step = "verify changed depth with masked color and same-value stencil writes";
    if (samples != 2u * (visible - 2u) || writes.colors || writes.depth_stencil != VK_IMAGE_ASPECT_DEPTH_BIT ||
        (depth[2][4] & (z32 ? UINT32_MAX : 0xffffffu)) != 0 ||
        (depth[2][3] & (z32 ? UINT32_MAX : 0xffffffu)) != far_z ||
        memcmp(stencil, stencil_before, sizeof stencil))
        return VK_ERROR_UNKNOWN;
    for (unsigned y = 0; y < SIDE; ++y) {
        if (depth[y][SIDE] != 0xfeedbad1u) return VK_ERROR_UNKNOWN;
        for (unsigned x = 0; x < COLOR_PITCH; ++x)
            if (color[y][x] != 0x72 || masked[y][x] != 0xa5) return VK_ERROR_UNKNOWN;
    }
    memcpy(depth_before, depth, sizeof depth);

    masks[location].colorWriteMask = base->pipeline.blend->pAttachments[location].colorWriteMask;
    for (unsigned y = 0; y < SIDE; ++y)
        for (unsigned x = 0; x < SIDE; ++x)
            memcpy(&color[y][4 * x], expected_color, 4);
    // Depth already matches the fragments that pass stencil; color also
    // matches the fragment output. Only stencil changes in this draw.
    zs.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    zs.front.passOp = VK_STENCIL_OP_INCREMENT_AND_WRAP;
    zs.front.writeMask = 0x0f;
    zs.back = zs.front;
    info.depth.input_token = info.depth.output_token;
    info.depth.output_token = ++sequence;
    result = dm_m4_draw_submit(draw, &info, step);
    if (result != VK_SUCCESS) return result;
    *step = "verify changed stencil with same-value depth and color writes";
    if (samples != 3u * visible - 5u || writes.colors || writes.depth_stencil != VK_IMAGE_ASPECT_STENCIL_BIT ||
        stencil[2][4] != 0x36 || stencil[2][3] != 0x5f || stencil[6][6] != 0x35 ||
        memcmp(depth, depth_before, sizeof depth))
        return VK_ERROR_UNKNOWN;
    for (unsigned y = 0; y < SIDE; ++y) {
        if (stencil[y][SIDE] != 0xc7) return VK_ERROR_UNKNOWN;
        for (unsigned x = 4 * SIDE; x < COLOR_PITCH; ++x)
            if (color[y][x] != 0x72) return VK_ERROR_UNKNOWN;
        for (unsigned x = 0; x < SIDE; ++x)
            if (memcmp(&color[y][4 * x], expected_color, 4)) return VK_ERROR_UNKNOWN;
    }

    get_format(device->physical, VK_FORMAT_S8_UINT, &props);
    if ((props.optimalTilingFeatures & required) != required) return VK_SUCCESS;
    info.depth = (struct dm_m4_draw_attachment){.format = VK_FORMAT_S8_UINT, .width = SIDE, .height = SIDE,
        .pixels = {stencil, sizeof stencil, sizeof stencil[0]}};
    zs.depthTestEnable = zs.depthWriteEnable = VK_FALSE;
    zs.front.compareOp = VK_COMPARE_OP_ALWAYS;
    zs.front.passOp = VK_STENCIL_OP_REPLACE;
    zs.front.reference = 0x3a;
    zs.back = zs.front;
    info.depth.output_token = ++sequence;
    samples = 0;
    result = dm_m4_draw_submit(draw, &info, step);
    if (result != VK_SUCCESS) return result;
    *step = "verify standalone stencil readback with unchanged color";
    if (samples != visible || writes.colors || writes.depth_stencil != VK_IMAGE_ASPECT_STENCIL_BIT ||
        stencil[2][4] != 0x3a || stencil[2][3] != 0x5a || stencil[6][6] != 0x35)
        return VK_ERROR_UNKNOWN;
    for (unsigned y = 0; y < SIDE; ++y)
        if (stencil[y][SIDE] != 0xc7) return VK_ERROR_UNKNOWN;
    memcpy(stencil_before, stencil, sizeof stencil);
    info.depth.input_token = info.depth.output_token;
    info.depth.output_token = ++sequence;

    result = dm_m4_draw_submit(draw, &info, step);
    if (result != VK_SUCCESS) return result;
    *step = "verify same-value standalone stencil writes still pass occlusion";
    if (samples != 2u * visible || writes.colors || writes.depth_stencil ||
        memcmp(stencil, stencil_before, sizeof stencil))
        return VK_ERROR_UNKNOWN;
    return VK_SUCCESS;
}

static VkResult draw_pixels_smoke(const struct dm_m4_device *device,
                                  const struct dm_m4_draw_requirements *requirements, VkImageViewType view,
                                  unsigned shadow, const char **step)
{
    const unsigned arrayed = view == VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    const unsigned volume = view == VK_IMAGE_VIEW_TYPE_3D;
    const unsigned cube = view == VK_IMAGE_VIEW_TYPE_CUBE;
    const unsigned location = view == VK_IMAGE_VIEW_TYPE_2D && !shadow ? 2u : 0u;
    const uint32_t sample_prefix[] = {
    0x07230203, 0x00010000, 0, 23, 0,
    OP(2, 17), 1,                               // OpCapability Shader
    OP(3, 14), 0, 1,                            // OpMemoryModel Logical GLSL450
    OP(6, 15), 4, 14, 0x6e69616d, 0, 13,        // OpEntryPoint Fragment %14 "main" %13
    OP(3, 16), 14, 7,                           // OpExecutionMode %14 OriginUpperLeft
    OP(4, 71), 12, 34, 1,                       // OpDecorate %12 DescriptorSet 1
    OP(4, 71), 12, 33, 1,                       // OpDecorate %12 Binding 1
    OP(4, 71), 20, 34, 1,                       // OpDecorate %20 DescriptorSet 1
    OP(4, 71), 20, 33, 2,                       // OpDecorate %20 Binding 2
    OP(4, 71), 13, 30, location,                // OpDecorate %13 Location
    OP(2, 19), 1,                               // %1 = OpTypeVoid
    OP(3, 22), 2, 32,                           // %2 = OpTypeFloat 32
    OP(4, 23), 3, 2, 3,                         // %3 = OpTypeVector %2 3
    OP(4, 23), 4, 2, 4,                         // %4 = OpTypeVector %2 4
    OP(9, 25), 5, 2, cube ? 3u : volume ? 2u : 1u, shadow, arrayed, 0, 1, 0, // Image type with Dim, Depth, Arrayed
    OP(3, 27), 6, 5,                            // %6 = OpTypeSampledImage %5
    OP(4, 32), 7, 0, 6,                         // %7 = OpTypePointer UniformConstant %6
    OP(4, 32), 8, 3, 4,                         // %8 = OpTypePointer Output %4
    OP(3, 33), 9, 1,                            // %9 = OpTypeFunction %1
    OP(4, 43), 2, 10, 0x3f000000,               // %10 = OpConstant %2 0.5
    OP(4, 43), 2, 18, volume ? 0x3f400000 : 0x3f800000, // %18 = OpConstant %2 0.75 (volume) or 1.0 (array)
    OP(6, 44), 3, 11, 10, 10, 18,               // %11 = OpConstantComposite %3 %10 %10 %18
    OP(4, 59), 7, 12, 0,                        // %12 = OpVariable %7 UniformConstant
    OP(4, 59), 7, 20, 0,                        // Second sampler for the same depth image
    OP(4, 59), 8, 13, 3,                        // %13 = OpVariable %8 Output
    OP(5, 54), 1, 14, 0, 9,                     // %14 = OpFunction %1 None %9
    OP(2, 248), 15,                             // %15 = OpLabel
    OP(4, 61), 6, 16, 12,                       // %16 = OpLoad %6 %12
    };
    const uint32_t color_end[] = {
    OP(5, 87), 4, 17, 16, 11,                   // %17 = OpImageSampleImplicitLod %4 %16 %11
    OP(3, 62), 13, 17,                          // OpStore %13 %17
    OP(1, 253), OP(1, 56),                      // OpReturn, OpFunctionEnd
    };
    const uint32_t shadow_end[] = {
    OP(6, 89), 2, 17, 16, 11, 10,               // %17 = OpImageSampleDrefImplicitLod %2 %16 %11 %10
    OP(4, 61), 6, 21, 20,                       // %21 = OpLoad %6 %20
    OP(6, 89), 2, 22, 21, 11, 10,               // %22 = second comparison of the same texel
    OP(7, 80), 4, 19, 17, 22, 17, 18,           // %19 = OpCompositeConstruct %4 %17 %22 %17 %18
    OP(3, 62), 13, 19,                          // OpStore %13 %19
    OP(1, 253), OP(1, 56),
    };
    uint32_t sample_spv[(sizeof sample_prefix + sizeof shadow_end) / sizeof(uint32_t)];
    memcpy(sample_spv, sample_prefix, sizeof sample_prefix);
    const size_t tail = shadow ? sizeof shadow_end : sizeof color_end;
    memcpy((uint8_t *)sample_spv + sizeof sample_prefix, shadow ? shadow_end : color_end, tail);
#undef OP

    const VkDevice vk = device->device;
    PFN_vkCreateShaderModule make_shader = (PFN_vkCreateShaderModule)device->get_device_proc(vk, "vkCreateShaderModule");
    PFN_vkCreateDescriptorSetLayout make_set = (PFN_vkCreateDescriptorSetLayout)
        device->get_device_proc(vk, "vkCreateDescriptorSetLayout");
    *step = "load staged draw smoke dispatch";
    if (!make_shader || !make_set) return VK_ERROR_INITIALIZATION_FAILED;
    struct dm_m4_draw *draw = NULL;
    struct dm_m4_shader vs = {.stage = VK_SHADER_STAGE_VERTEX_BIT};
    struct dm_m4_shader ps = {.stage = VK_SHADER_STAGE_FRAGMENT_BIT, .set = 1, .frag_write_mask = 15u << (4 * location)};
    struct dm_m4_shader writes = {.stage = VK_SHADER_STAGE_VERTEX_BIT};
    const size_t vs_words = sizeof position_spv / sizeof *position_spv;
    uint32_t point_spv[sizeof position_spv / sizeof *position_spv + DM_M4_POINT_SIZE_EXTRA_WORDS];
    memcpy(point_spv, position_spv, sizeof position_spv);
    size_t shader_words = dm_m4_shader_specialize_point_size(point_spv, vs_words);
    vs.point_size_fallback = shader_words == vs_words + DM_M4_POINT_SIZE_EXTRA_WORDS;
    *step = "specialize staged draw test PointSize output";
    if (!vs.point_size_fallback) return VK_ERROR_INVALID_SHADER_NV;
    VkShaderModuleCreateInfo module = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .pCode = point_spv, .codeSize = shader_words * sizeof(*point_spv),
    };
    *step = "create staged draw test vertex shader";
    VkResult result = make_shader(vk, &module, NULL, &vs.module);
    if (result != VK_SUCCESS) goto done;
    module.pCode = sample_spv;
    module.codeSize = sizeof sample_prefix + tail;
    *step = "create staged draw test fragment shader";
    result = make_shader(vk, &module, NULL, &ps.module);
    if (result != VK_SUCCESS) goto done;
    const VkDescriptorSetLayoutBinding vk_bindings[] = {
        {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
        {.binding = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    VkDescriptorSetLayoutCreateInfo set = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    *step = "create staged draw test vertex set";
    result = make_set(vk, &set, NULL, &vs.set_layout);
    if (result != VK_SUCCESS) goto done;
    set.bindingCount = shadow ? 2u : 1u;
    set.pBindings = vk_bindings;
    *step = "create staged draw test fragment set";
    result = make_set(vk, &set, NULL, &ps.set_layout);
    if (result != VK_SUCCESS) goto done;
    result = dm_m4_draw_create(&draw, device, requirements, step);
    if (result != VK_SUCCESS) goto done;
    const struct dm_m4_shader_binding recipe = {
        .binding = 1, .count = 1, .descriptor_type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .source = DM_M4_SHADER_TEXTURE,
        .image = {.type = (cube ? 5u : volume ? 4u : arrayed ? 259u : 3u) | (shadow ? 0x400u : 0u), .tbx = -1},
    };
    struct dm_m4_shader_binding recipes[] = {recipe, recipe};
    recipes[1].binding = 2;
    ps.binding_count = shadow ? 2u : 1u;
    ps.bindings = recipes;
    uint8_t rgba_texel[] = {198, 55, 141, 255};
    const uint8_t other_layer[] = {3, 2, 1, 0};
    const struct dm_m4_draw_pixels texels[] = {
        {other_layer, sizeof other_layer, 0}, {rgba_texel, sizeof rgba_texel, 0},
    };
    const struct dm_m4_draw_pixels faces[] = {
        texels[0], texels[0], texels[0], texels[0], texels[1], texels[0],
    }; // direction (0.5, 0.5, 1) selects +Z, not face zero.
    float depth_layers[][2] = {{0.25f, 0.0f}, {0.75f, 0.0f}};
    const struct dm_m4_draw_pixels depth_texels[] = {
        {depth_layers[0], sizeof(float), 0}, {depth_layers[1], sizeof(float), 0},
    };
    // A 1x2x4 volume needs three mips even though its largest XY dimension
    // is two. At mip 1 the selected second slice follows a padded first row.
    const uint8_t mip0[4 * 2 * 4] = {0};
    uint8_t mip1[] = {3, 2, 1, 0, 0xdb, 0xdb, 0xdb, 0xdb, 198, 55, 141, 255};
    const struct dm_m4_draw_pixels volume_mips[] = {
        {mip0, sizeof mip0, 0}, {mip1, sizeof mip1, 8}, {other_layer, sizeof other_layer, 0},
    };
    const uint8_t initial_shadow[] = {255, 0, 255, 255};
    uint8_t *texel = volume ? mip1 + 8 : rgba_texel;
    struct dm_m4_draw_sampled_image texture = {
        .format = shadow ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM,
        .width = 1, .height = volume ? 2u : 1u,
        .level_count = volume ? 3u : 1u,
        .levels = shadow ? depth_texels : cube ? faces : volume ? volume_mips : arrayed ? texels : texels + 1,
        .array_layers = arrayed ? 2u : 0u, .depth = volume ? 4u : 0u, .cube = cube,
        .sampler = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                    .compareEnable = shadow, .compareOp = VK_COMPARE_OP_LESS,
                    .minLod = volume ? 1.0f : 0.0f, .maxLod = volume ? 1.0f : 0.0f},
    };
    struct checked_smoke_upload encoded = {.planes = texture.levels};
    const struct dm_m4_draw_sample_upload upload = {
        .pool_generation = 1, .id = 19, .kind = recipe.image.type, .layers = shadow ? 2u : 1u,
        .encoded = shadow ? (const uint8_t *)depth_layers : rgba_texel,
        .encoded_size = shadow ? sizeof depth_layers : sizeof rgba_texel,
        .layer_bytes = sizeof rgba_texel, .layer_stride = shadow ? sizeof depth_layers[0] : sizeof rgba_texel,
        .context = &encoded, .materialize = checked_smoke_materialize,
    };
    if (shadow || location) {
        texture.levels = NULL;
        texture.upload = &upload;
    }
    VkSamplerCreateInfo opposite = texture.sampler;
    opposite.compareOp = VK_COMPARE_OP_GREATER;
    const struct dm_m4_draw_binding bindings[] = {
        {.texture = &texture}, {.texture = &texture, .sampler = &opposite},
    };
    const VkVertexInputBindingDescription stream = {.stride = 16, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attribute = {.format = VK_FORMAT_R32G32B32A32_SFLOAT};
    const VkPipelineVertexInputStateCreateInfo input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &stream,
        .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &attribute,
    };
    const VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .lineWidth = 1.0f,
    };
    const VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    const VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_LESS, .maxDepthBounds = 1.0f,
    };
    VkPipelineColorBlendAttachmentState write[3] = {0};
    write[location].colorWriteMask = 15;
    const VkPipelineColorBlendStateCreateInfo blend = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = location + 1, .pAttachments = write,
    };
    enum { SIDE = 8, PITCH = SIDE * 4 + 4 };
    uint8_t rgba[SIDE][PITCH];
    uint8_t first[SIDE][PITCH];
    float zs[SIDE][SIDE];
    const uint8_t untouched[] = {17, 34, 51, 68};
    memset(rgba, 0xdb, sizeof rgba);
    for (size_t y = 0; y < SIDE; ++y) {
        for (size_t x = 0; x < SIDE; ++x) {
            memcpy(&rgba[y][4 * x], untouched, sizeof untouched);
            zs[y][x] = 1.0f;
        }
    }
    if (location) memcpy(first, rgba, sizeof first);
    const float vertices[][4] = {
        {-0.75f, -0.75f, 0.25f, 1.0f}, {0.75f, -0.75f, 0.25f, 1.0f}, {-0.75f, 0.75f, 0.25f, 1.0f},
    };
    const uint8_t indices[] = {0, 1, 2};
    uint64_t samples = 0;
    struct dm_m4_draw_info info = {
        .vs = {&vs, NULL}, .ps = {&ps, bindings},
        .samples_passed = &samples,
        .pipeline = {
            .vertex_input = &input, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .rasterization = &raster, .multisample = &multisample, .depth_stencil = &depth, .blend = &blend,
            .viewport_count = 1,
        },
        .viewport = {0.0f, 0.0f, SIDE, SIDE, 0.0f, 1.0f}, .scissor = {{0, 0}, {SIDE, SIDE}},
        .extent = {SIDE, SIDE}, .color_count = location + 1,
        .depth = {.format = VK_FORMAT_D32_SFLOAT, .width = SIDE, .height = SIDE, .pixels = {zs, sizeof zs, 0}},
        .indices = {indices, sizeof indices}, .index_type = VK_INDEX_TYPE_UINT8_EXT, .index_count = 3, .instance_count = 1,
        .vertices = {{vertices, sizeof vertices}},
    };
    info.colors[location] = (struct dm_m4_draw_attachment){
        .format = VK_FORMAT_R8G8B8A8_UNORM, .width = SIDE, .height = SIDE, .pixels = {rgba, sizeof rgba, PITCH},
    };
    if (location) info.colors[0] = (struct dm_m4_draw_attachment){
        .format = VK_FORMAT_B8G8R8A8_UNORM, .width = SIDE, .height = SIDE, .pixels = {first, sizeof first, PITCH},
    };
    result = dm_m4_draw_submit(draw, &info, step);
    if (result != VK_SUCCESS) goto done;
    unsigned visible = 0;
    for (unsigned y = 0; y < SIDE; ++y)
        for (unsigned x = 0; x < SIDE; ++x) visible += zs[y][x] != 1.0f;
    if (!visible || samples != visible) {
        *step = "verify precise samples passed against depth readback";
        result = VK_ERROR_UNKNOWN;
        goto done;
    }
    *step = "verify staged draw color, depth and preserved target pixels";
    if (memcmp(&rgba[2][8], shadow ? initial_shadow : texel, sizeof rgba_texel) || zs[2][2] != 0.25f ||
        memcmp(&rgba[6][24], untouched, sizeof untouched) || zs[6][6] != 1.0f || rgba[2][PITCH - 1] != 0xdb) {
        result = VK_ERROR_UNKNOWN;
        goto done;
    }
    if (location && memcmp(&first[2][8], untouched, sizeof untouched)) {
        *step = "verify slot zero stays untouched when the shader writes slot two across a hole";
        result = VK_ERROR_UNKNOWN;
        goto done;
    }
    if (location) {
        result = attachment_writes_smoke(draw, device, &info, location, texel, visible, step);
        if (result != VK_SUCCESS) goto done;
    }
    if (shadow) {
        // The next draw samples a different depth array, then the first
        // registered view returns. A miss must not evict that resident view.
        struct dm_m4_draw_sampled_image alternate = texture;
        const struct dm_m4_draw_pixels layers[] = {depth_texels[0], depth_texels[0]};
        alternate.upload = NULL;
        alternate.levels = layers;
        const struct dm_m4_draw_binding other[] = {
            {.texture = &alternate}, {.texture = &alternate, .sampler = &opposite},
        };
        struct dm_m4_draw_info interleaved = info;
        interleaved.ps.bindings = other;
        interleaved.samples_passed = NULL;
        zs[2][2] = 1.0f;
        result = dm_m4_draw_submit(draw, &interleaved, step);
        if (result != VK_SUCCESS) goto done;
        const uint8_t alternate_shadow[] = {0, 255, 0, 255};
        if (memcmp(&rgba[2][8], alternate_shadow, sizeof alternate_shadow)) {
            *step = "verify the interleaved draw sampled the other depth array";
            result = VK_ERROR_UNKNOWN;
            goto done;
        }
        // Reverse the two independent comparisons without changing the
        // strided depth backing or materializing another image.
        texture.sampler.compareOp = VK_COMPARE_OP_GREATER;
        opposite.compareOp = VK_COMPARE_OP_LESS;
        zs[2][2] = 1.0f;
    } else {
        // Same depth fails LESS on the second draw despite a changed texture.
        texel[0] = 7;
    }
    result = dm_m4_draw_submit(draw, &info, step);
    if (result != VK_SUCCESS) goto done;
    if (samples != visible + (unsigned)shadow) {
        *step = "verify accumulated samples ignore depth-rejected fragments";
        result = VK_ERROR_UNKNOWN;
        goto done;
    }
    const uint8_t reversed_shadow[] = {0, 255, 0, 255};
    *step = shadow ? "verify independent samplers reuse an unchanged checked depth view" :
                    "verify the next draw preserves prior depth and color";
    if ((shadow ? memcmp(&rgba[2][8], reversed_shadow, sizeof reversed_shadow) || encoded.materializations != 1 :
                  rgba[2][8] != 198) || zs[2][2] != 0.25f)
        result = VK_ERROR_UNKNOWN;
    if (result != VK_SUCCESS) goto done;
    if (shadow) {
        depth_layers[0][1] = 1.0f; // gap between the two checked layer spans
        zs[2][2] = 1.0f;
        result = dm_m4_draw_submit(draw, &info, step);
        if (result != VK_SUCCESS) goto done;
        *step = "verify padding between checked layers does not invalidate the sample";
        if (memcmp(&rgba[2][8], reversed_shadow, sizeof reversed_shadow) || encoded.materializations != 1) {
            result = VK_ERROR_UNKNOWN;
            goto done;
        }
        depth_layers[1][0] = 0.25f;
        zs[2][2] = 1.0f;
        result = dm_m4_draw_submit(draw, &info, step);
        if (result != VK_SUCCESS) goto done;
        *step = "verify changed encoded bytes in the second checked layer reach the GPU";
        if (memcmp(&rgba[2][8], initial_shadow, sizeof initial_shadow) || encoded.materializations != 2) {
            result = VK_ERROR_UNKNOWN;
            goto done;
        }
    }
    if (location) {
        // Reveal the replacement texel, then change only the view mapping
        // while keeping its registered ID, layout and bytes.
        zs[2][2] = 1.0f;
        result = dm_m4_draw_submit(draw, &info, step);
        if (result != VK_SUCCESS) goto done;
        *step = "verify a changed checked backing is uploaded once and then reused";
        if (memcmp(&rgba[2][8], rgba_texel, sizeof rgba_texel) || encoded.materializations != 2) {
            result = VK_ERROR_UNKNOWN;
            goto done;
        }
        texture.components.r = VK_COMPONENT_SWIZZLE_G;
        texture.components.g = VK_COMPONENT_SWIZZLE_R;
        zs[2][2] = 1.0f;
        result = dm_m4_draw_submit(draw, &info, step);
        if (result != VK_SUCCESS) goto done;
        const uint8_t remapped[] = {55, 7, 141, 255};
        *step = "verify changed view mapping cannot reuse the stale checked view";
        if (memcmp(&rgba[2][8], remapped, sizeof remapped)) {
            result = VK_ERROR_UNKNOWN;
            goto done;
        }
    }
    if (device->null_descriptor && (location || volume || shadow)) {
        // Replace populated views, keeping the registered samplers. The Dref
        // shader compares twice with opposite functions and writes alpha one.
        const struct dm_m4_draw_binding null_images[] = {
            {.sampler = &texture.sampler}, {.sampler = &opposite},
        };
        info.ps.bindings = null_images;
        zs[2][2] = 1.0f;
        result = dm_m4_draw_submit(draw, &info, step);
        info.ps.bindings = bindings;
        if (result != VK_SUCCESS) goto done;
        const uint8_t zero[4] = {0, 0, 0, shadow ? 255 : 0};
        if (memcmp(&rgba[2][8], zero, sizeof zero) || zs[2][2] != 0.25f) {
            fprintf(stderr, "null sample readback RGBA: %u %u %u %u\n",
                    rgba[2][8], rgba[2][9], rgba[2][10], rgba[2][11]);
            *step = "verify null color and depth-comparison samples return zero";
            result = VK_ERROR_UNKNOWN;
            goto done;
        }
    }
    if (!location) goto done;
    result = draw_sizes_smoke(draw, &info, step);
    if (result != VK_SUCCESS) goto done;
    result = draw_restart_smoke(draw, &info, step);
    if (result != VK_SUCCESS) goto done;

    // The same geometry must update depth without either a color attachment
    // or a fragment entry point.
    info.color_count = 0;
    info.pipeline.blend = NULL;
    info.ps = (struct dm_m4_draw_stage){0};
    zs[2][2] = 1.0f;
    result = dm_m4_draw_submit(draw, &info, step);
    if (result != VK_SUCCESS) goto done;
    if (zs[2][2] != 0.25f || zs[6][6] != 1.0f) {
        *step = "verify depth-only draw without fragment module";
        result = VK_ERROR_UNKNOWN;
        goto done;
    }
    // Two nonindexed instances start at vertex one; vertex zero is a poison
    // position. LESS rejects the second instance, giving the indexed coverage.
    float shifted[4][4] = {{8.f, 8.f, 0.f, 1.f}};
    memcpy(shifted + 1, vertices, sizeof vertices);
    struct dm_m4_draw_info arrays = info;
    arrays.indices = (struct dm_m4_draw_span){0};
    arrays.index_count = 0;
    arrays.vertex_count = 3;
    arrays.base_vertex = 1;
    arrays.instance_count = 2;
    arrays.first_instance = 7;
    arrays.vertices[0] = (struct dm_m4_draw_span){shifted, sizeof shifted};
    for (unsigned y = 0; y < SIDE; ++y)
        for (unsigned x = 0; x < SIDE; ++x) zs[y][x] = 1.0f;
    samples = 0;
    result = dm_m4_draw_submit(draw, &arrays, step);
    if (result != VK_SUCCESS) goto done;
    if (samples != visible || zs[2][2] != 0.25f || zs[6][6] != 1.f) {
        *step = "verify nonindexed instancing with nonzero first vertex";
        result = VK_ERROR_UNKNOWN;
        goto done;
    }

    module.pCode = vertex_store_spv;
    module.codeSize = sizeof vertex_store_spv;
    *step = "create attachmentless storage-writing vertex shader";
    result = make_shader(vk, &module, NULL, &writes.module);
    if (result != VK_SUCCESS) goto done;
    const VkDescriptorSetLayoutBinding storage = {
        .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
    };
    set.pBindings = &storage;
    *step = "create attachmentless vertex storage set";
    result = make_set(vk, &set, NULL, &writes.set_layout);
    if (result != VK_SUCCESS) goto done;
    const struct dm_m4_shader_binding store_recipe = {
        .binding = 1, .count = 1, .descriptor_type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .source = DM_M4_SHADER_SBUF, .sbuf = {.flags = 1},
    };
    writes.binding_count = 1;
    writes.bindings = &store_recipe;
    uint32_t word = 0x10000100;
    const struct dm_m4_draw_binding store_data = {.buffer = {&word, sizeof word}, .writeback = &word};
    info.vs = (struct dm_m4_draw_stage){&writes, &store_data};
    info.depth = (struct dm_m4_draw_attachment){0};
    info.extent = (VkExtent2D){1, 1};
    info.scissor = (VkRect2D){{0, 0}, {1, 1}};
    info.viewport = (VkViewport){0.f, 0.f, 1.f, 1.f, 0.f, 1.f};
    VkPipelineRasterizationStateCreateInfo discard = raster;
    discard.rasterizerDiscardEnable = VK_TRUE;
    info.pipeline.rasterization = &discard;
    info.pipeline.depth_stencil = NULL;
    result = dm_m4_draw_submit(draw, &info, step);
    if (result == VK_SUCCESS && word != 0x10ab0123) {
        *step = "verify vertex storage readback from an attachmentless draw";
        result = VK_ERROR_UNKNOWN;
    }
    if (result != VK_SUCCESS) goto done;

    uint8_t saved_color[sizeof rgba], saved_masked[sizeof first];
    float saved_depth[SIDE][SIDE];
    memcpy(saved_color, rgba, sizeof rgba);
    memcpy(saved_masked, first, sizeof first);
    memcpy(saved_depth, zs, sizeof zs);
    struct dm_m4_draw_attachment_writes plane_changes;
    info.attachment_writes = &plane_changes;
    info.color_count = location + 1;
    info.pipeline.blend = &blend;
    info.pipeline.depth_stencil = &depth;
    info.depth = (struct dm_m4_draw_attachment){
        .format = VK_FORMAT_D32_SFLOAT, .width = SIDE, .height = SIDE, .pixels = {zs, sizeof zs, 0}};
    word = 0x10000100;
    samples = 0;
    result = dm_m4_draw_submit(draw, &info, step);
    if (result == VK_SUCCESS && (word != 0x10ab0123 || samples || plane_changes.colors || plane_changes.depth_stencil ||
        memcmp(rgba, saved_color, sizeof rgba) || memcmp(first, saved_masked, sizeof first) ||
        memcmp(zs, saved_depth, sizeof zs))) {
        *step = "verify discard leaves bound attachments and occlusion untouched but writes vertex storage";
        result = VK_ERROR_UNKNOWN;
    }
    if (result != VK_SUCCESS) goto done;

    // Color and depth can write, but no samples are covered.
    info.ps = (struct dm_m4_draw_stage){&ps, bindings};
    info.pipeline.rasterization = &raster;
    info.extent = (VkExtent2D){SIDE, SIDE};
    info.viewport = (VkViewport){0.f, 0.f, SIDE, SIDE, 0.f, 1.f};
    info.scissor = (VkRect2D){{0, 0}, {0, SIDE}};
    word = 0x10000100;
    result = dm_m4_draw_submit(draw, &info, step);
    if (result == VK_SUCCESS && (word != 0x10ab0123 || samples || plane_changes.colors || plane_changes.depth_stencil ||
        memcmp(rgba, saved_color, sizeof rgba) || memcmp(first, saved_masked, sizeof first) ||
        memcmp(zs, saved_depth, sizeof zs))) {
        *step = "verify zero coverage changes no attachments but executes vertex storage writes";
        result = VK_ERROR_UNKNOWN;
    }
done:
    dm_m4_draw_destroy(draw);
    writes.bindings = NULL;
    dm_m4_shader_destroy(device, &writes);
    ps.bindings = NULL; // Static/local smoke recipe was not allocated by the shader loader.
    dm_m4_shader_destroy(device, &ps);
    dm_m4_shader_destroy(device, &vs);
    return result;
}
