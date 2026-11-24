// NVN2 production SDK value objects. These bytes are copied by value and
// accessed by inline guest code, so keep the SDK's little-endian word layout.
// No state here depends on a device, command buffer, or backend.
static void raster_check(const void *ptr, size_t size, int write, const char *proc) {
    if (!dm_guest_range(ptr, size, write)) dm_fail("%s: invalid guest span", proc);
}

static uint32_t raster_load_word(const void *ptr) {
    uint32_t word;
    memcpy(&word, ptr, sizeof(word));
    return word;
}

static void raster_store_word(void *ptr, uint32_t word) {
    memcpy(ptr, &word, sizeof(word));
}

static uint32_t raster_insert(uint32_t word, unsigned shift, uint32_t mask, uint32_t value) {
    return (word & ~(mask << shift)) | (value << shift);
}

static unsigned raster_bool(uint32_t value) {
    // NVNboolean is a byte; the SDK ignores the upper bits of the argument register.
    return (NVNboolean)value != 0;
}

static unsigned raster_target(int target, const char *proc) {
    if ((unsigned)target >= 8) dm_fail("%s: invalid color target %d", proc, target);
    return (unsigned)target;
}

#define RASTER_DEFAULTS(type, family, name, ...) \
    void nvn_##name##_defaults(type *s) { \
        static const uint32_t words[] = { __VA_ARGS__ }; \
        static_assert(sizeof(words) == sizeof(type), #family " size"); \
        raster_check(s, sizeof(*s), 1, "nvn" #family "SetDefaults"); \
        memcpy(s, words, sizeof(words)); \
    }

// Scalar fields live in the first word. Reject values that would spill into
// a neighboring field, and preserve all the other bits, including padding.
#define RASTER_ACCESSOR(type, family, property, name, result, param, value, shift, mask) \
    void nvn_##name(type *s, param v) { \
        const char *proc = "nvn" #family "Set" #property; \
        uint32_t field = (value); \
        if (field > (mask)) dm_fail("%s: value exceeds packed field", proc); \
        raster_check(s, sizeof(*s), 1, proc); \
        raster_store_word(s, raster_insert(raster_load_word(s), shift, mask, field)); \
    } \
    result nvn_##name##_get(const type *s) { \
        raster_check(s, sizeof(*s), 0, "nvn" #family "Get" #property); \
        return (result)((raster_load_word(s) >> (shift)) & (mask)); \
    }
#define RASTER_VALUE(type, family, property, name, scalar, shift, mask) \
    RASTER_ACCESSOR(type, family, property, name, scalar, scalar, (uint32_t)v, shift, mask)
#define RASTER_BOOL(type, family, property, name, shift) \
    RASTER_ACCESSOR(type, family, property, name, NVNboolean, uint32_t, raster_bool(v), shift, 1u)

RASTER_DEFAULTS(NVNcolorState, ColorState, color, 0x08030000)
RASTER_VALUE(NVNcolorState, ColorState, LogicOp, color_logic_op, NVNlogicOp, 16, 0xffu)
RASTER_VALUE(NVNcolorState, ColorState, AlphaTest, color_alpha_test, NVNalphaFunc, 24, 0xfu)

void nvn_color_blend_enable(NVNcolorState *s, int target, uint32_t enabled) {
    const char *proc = "nvnColorStateSetBlendEnable";
    unsigned bit = 8 + raster_target(target, proc);
    raster_check(s, sizeof(*s), 1, proc);
    raster_store_word(s, raster_insert(raster_load_word(s), bit, 1, raster_bool(enabled)));
}

NVNboolean nvn_color_blend_enable_get(const NVNcolorState *s, int target) {
    const char *proc = "nvnColorStateGetBlendEnable";
    unsigned bit = 8 + raster_target(target, proc);
    raster_check(s, sizeof(*s), 0, proc);
    return (NVNboolean)((raster_load_word(s) >> bit) & 1u);
}

RASTER_DEFAULTS(NVNblendState, BlendState, blend, 0x110c0000, 0x01020102)
RASTER_VALUE(NVNblendState, BlendState, BlendTarget, blend_target, int, 0, 7u)
RASTER_VALUE(NVNblendState, BlendState, AdvancedMode, blend_advanced_mode, NVNblendAdvancedMode, 8, 0x3fu)
RASTER_VALUE(NVNblendState, BlendState, AdvancedOverlap, blend_advanced_overlap, NVNblendAdvancedOverlap, 16, 3u)
RASTER_BOOL(NVNblendState, BlendState, AdvancedPremultipliedSrc, blend_advanced_premultiplied_src, 18)
RASTER_BOOL(NVNblendState, BlendState, AdvancedNormalizedDst, blend_advanced_normalized_dst, 19)

void nvn_blend_func(NVNblendState *s, NVNblendFunc src, NVNblendFunc dst,
                    NVNblendFunc src_alpha, NVNblendFunc dst_alpha) {
    const char *proc = "nvnBlendStateSetBlendFunc";
    if (((uint32_t)src | (uint32_t)dst | (uint32_t)src_alpha | (uint32_t)dst_alpha) > 0x7fu)
        dm_fail("%s: value exceeds packed field", proc);
    raster_check(s, sizeof(*s), 1, proc);
    uint32_t word = raster_load_word(s->reserved + 4) & UINT32_C(0x80808080);
    raster_store_word(s->reserved + 4, word | (uint32_t)src | ((uint32_t)dst << 8) |
                      ((uint32_t)src_alpha << 16) | ((uint32_t)dst_alpha << 24));
}

void nvn_blend_func_get(const NVNblendState *s, NVNblendFunc *src, NVNblendFunc *dst,
                        NVNblendFunc *src_alpha, NVNblendFunc *dst_alpha) {
    const char *proc = "nvnBlendStateGetBlendFunc";
    raster_check(s, sizeof(*s), 0, proc);
    raster_check(src, sizeof(*src), 1, proc);
    raster_check(dst, sizeof(*dst), 1, proc);
    raster_check(src_alpha, sizeof(*src_alpha), 1, proc);
    raster_check(dst_alpha, sizeof(*dst_alpha), 1, proc);
    uint32_t word = raster_load_word(s->reserved + 4);
    raster_store_word(src, word & 0x7fu);
    raster_store_word(dst, (word >> 8) & 0x7fu);
    raster_store_word(src_alpha, (word >> 16) & 0x7fu);
    raster_store_word(dst_alpha, (word >> 24) & 0x7fu);
}

void nvn_blend_equation(NVNblendState *s, NVNblendEquation rgb, NVNblendEquation alpha) {
    const char *proc = "nvnBlendStateSetBlendEquation";
    if (((uint32_t)rgb | (uint32_t)alpha) > 7u) dm_fail("%s: value exceeds packed field", proc);
    raster_check(s, sizeof(*s), 1, proc);
    uint32_t word = raster_load_word(s) & UINT32_C(0x88ffffff);
    raster_store_word(s, word | ((uint32_t)rgb << 24) | ((uint32_t)alpha << 28));
}

void nvn_blend_equation_get(const NVNblendState *s, NVNblendEquation *rgb, NVNblendEquation *alpha) {
    const char *proc = "nvnBlendStateGetBlendEquation";
    raster_check(s, sizeof(*s), 0, proc);
    raster_check(rgb, sizeof(*rgb), 1, proc);
    raster_check(alpha, sizeof(*alpha), 1, proc);
    uint32_t word = raster_load_word(s);
    raster_store_word(rgb, (word >> 24) & 7u);
    raster_store_word(alpha, (word >> 28) & 7u);
}

RASTER_DEFAULTS(NVNchannelMaskState, ChannelMaskState, channel_mask, UINT32_MAX)

void nvn_channel_mask(NVNchannelMaskState *s, int target, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    const char *proc = "nvnChannelMaskStateSetChannelMask";
    unsigned shift = 4 * raster_target(target, proc);
    raster_check(s, sizeof(*s), 1, proc);
    uint32_t mask = raster_bool(r) | (raster_bool(g) << 1) | (raster_bool(b) << 2) | (raster_bool(a) << 3);
    raster_store_word(s, raster_insert(raster_load_word(s), shift, 0xf, mask));
}

void nvn_channel_mask_get(const NVNchannelMaskState *s, int target, NVNboolean *r, NVNboolean *g,
                          NVNboolean *b, NVNboolean *a) {
    const char *proc = "nvnChannelMaskStateGetChannelMask";
    unsigned shift = 4 * raster_target(target, proc);
    raster_check(s, sizeof(*s), 0, proc);
    raster_check(r, sizeof(*r), 1, proc);
    raster_check(g, sizeof(*g), 1, proc);
    raster_check(b, sizeof(*b), 1, proc);
    raster_check(a, sizeof(*a), 1, proc);
    uint32_t word = raster_load_word(s) >> shift;
    *r = (NVNboolean)(word & 1u);
    *g = (NVNboolean)((word >> 1) & 1u);
    *b = (NVNboolean)((word >> 2) & 1u);
    *a = (NVNboolean)((word >> 3) & 1u);
}

RASTER_DEFAULTS(NVNdepthStencilState, DepthStencilState, depth_stencil, 0x22, 0x11181118)
RASTER_BOOL(NVNdepthStencilState, DepthStencilState, DepthTestEnable, depth_test_enable, 0)
RASTER_BOOL(NVNdepthStencilState, DepthStencilState, DepthWriteEnable, depth_write_enable, 1)
RASTER_BOOL(NVNdepthStencilState, DepthStencilState, StencilTestEnable, stencil_test_enable, 2)
RASTER_VALUE(NVNdepthStencilState, DepthStencilState, DepthFunc, depth_func, NVNdepthFunc, 4, 0xfu)

// The front and back halves have identical layouts: compare function, stencil
// fail, depth pass, depth fail (each one nibble, low to high).
static void raster_stencil_update(NVNdepthStencilState *s, NVNface face, uint32_t mask, uint32_t value,
                                  const char *proc) {
    if (!(face & NVN_FACE_FRONT_AND_BACK)) return;
    if (value & ~mask) dm_fail("%s: value exceeds packed field", proc);
    raster_check(s, sizeof(*s), 1, proc);
    uint32_t word = raster_load_word(s->reserved + 4);
    if (face & NVN_FACE_FRONT) word = raster_insert(word, 0, mask, value);
    if (face & NVN_FACE_BACK) word = raster_insert(word, 16, mask, value);
    raster_store_word(s->reserved + 4, word);
}

void nvn_stencil_func(NVNdepthStencilState *s, NVNface face, NVNstencilFunc func) {
    raster_stencil_update(s, face, 0xf, (uint32_t)func, "nvnDepthStencilStateSetStencilFunc");
}

NVNstencilFunc nvn_stencil_func_get(const NVNdepthStencilState *s, NVNface face) {
    raster_check(s, sizeof(*s), 0, "nvnDepthStencilStateGetStencilFunc");
    unsigned shift = face == NVN_FACE_BACK ? 16 : 0;
    return (NVNstencilFunc)((raster_load_word(s->reserved + 4) >> shift) & 0xfu);
}

void nvn_stencil_op(NVNdepthStencilState *s, NVNface face, NVNstencilOp fail, NVNstencilOp depth_fail,
                    NVNstencilOp depth_pass) {
    const char *proc = "nvnDepthStencilStateSetStencilOp";
    if (((uint32_t)fail | (uint32_t)depth_fail | (uint32_t)depth_pass) > 0xfu)
        dm_fail("%s: value exceeds packed field", proc);
    uint32_t value = ((uint32_t)fail << 4) | ((uint32_t)depth_fail << 12) | ((uint32_t)depth_pass << 8);
    raster_stencil_update(s, face, 0xfff0, value, proc);
}

void nvn_stencil_op_get(const NVNdepthStencilState *s, NVNface face, NVNstencilOp *fail,
                        NVNstencilOp *depth_fail, NVNstencilOp *depth_pass) {
    const char *proc = "nvnDepthStencilStateGetStencilOp";
    raster_check(s, sizeof(*s), 0, proc);
    raster_check(fail, sizeof(*fail), 1, proc);
    raster_check(depth_fail, sizeof(*depth_fail), 1, proc);
    raster_check(depth_pass, sizeof(*depth_pass), 1, proc);
    unsigned shift = face == NVN_FACE_BACK ? 16 : 0;
    uint32_t ops = raster_load_word(s->reserved + 4) >> shift;
    raster_store_word(fail, (ops >> 4) & 0xfu);
    raster_store_word(depth_fail, (ops >> 12) & 0xfu);
    raster_store_word(depth_pass, (ops >> 8) & 0xfu);
}

RASTER_DEFAULTS(NVNpolygonState, PolygonState, polygon, 0x14)
RASTER_VALUE(NVNpolygonState, PolygonState, CullFace, polygon_cull_face, NVNface, 0, 3u)
RASTER_VALUE(NVNpolygonState, PolygonState, FrontFace, polygon_front_face, NVNfrontFace, 2, 1u)
RASTER_VALUE(NVNpolygonState, PolygonState, PolygonMode, polygon_mode, NVNpolygonMode, 3, 3u)
RASTER_VALUE(NVNpolygonState, PolygonState, PolygonOffsetEnables, polygon_offset_enables, int, 15, 0xfu)

RASTER_DEFAULTS(NVNmultisampleState, MultisampleState, multisample,
                5, 0x88888888, 0x88888888, 0x88888888, 0x88888888, 0)
RASTER_BOOL(NVNmultisampleState, MultisampleState, MultisampleEnable, multisample_enable, 0)
RASTER_BOOL(NVNmultisampleState, MultisampleState, AlphaToCoverageEnable, multisample_alpha_to_coverage_enable, 1)
RASTER_BOOL(NVNmultisampleState, MultisampleState, AlphaToCoverageDither, multisample_alpha_to_coverage_dither, 2)
RASTER_BOOL(NVNmultisampleState, MultisampleState, SampleLocationsEnable, multisample_sample_locations_enable, 3)
RASTER_BOOL(NVNmultisampleState, MultisampleState, SampleLocationsGridEnable, multisample_sample_locations_grid_enable, 4)
RASTER_BOOL(NVNmultisampleState, MultisampleState, CoverageTruncationEnable, multisample_coverage_truncation_enable, 5)
RASTER_VALUE(NVNmultisampleState, MultisampleState, Samples, multisample_samples, int, 8, 0x1fu)
RASTER_VALUE(NVNmultisampleState, MultisampleState, CoverageModulationMode, multisample_coverage_modulation_mode,
             NVNcoverageModulationMode, 13, 3u)
RASTER_VALUE(NVNmultisampleState, MultisampleState, RasterSamples, multisample_raster_samples, int, 15, 0x1fu)
RASTER_BOOL(NVNmultisampleState, MultisampleState, CoverageToColorEnable, multisample_coverage_to_color_enable, 20)
RASTER_VALUE(NVNmultisampleState, MultisampleState, CoverageToColorOutput, multisample_coverage_to_color_output, int, 21, 7u)

#undef RASTER_BOOL
#undef RASTER_VALUE
#undef RASTER_ACCESSOR
#undef RASTER_DEFAULTS

void nvn_multisample_sample_locations_grid(const NVNmultisampleState *s, int *width, int *height) {
    const char *proc = "nvnMultisampleStateGetSampleLocationsGrid";
    raster_check(s, sizeof(*s), 0, proc);
    uint32_t word = raster_load_word(s);
    unsigned samples = (word >> 8) & 0x1fu, raster = (word >> 15) & 0x1fu;
    unsigned w = 1, h = 1;
    switch (samples > raster ? samples : raster) {
    case 0: case 1: case 16: break;
    case 2: w = 2; h = 4; break;
    case 4: w = 2; h = 2; break;
    case 8: h = 2; break;
    default: return;  // The production SDK leaves both outputs untouched.
    }
    raster_check(width, sizeof(*width), 1, proc);
    raster_check(height, sizeof(*height), 1, proc);
    raster_store_word(width, w);
    raster_store_word(height, h);
}

// Sixteen packed x/y nibbles follow the control word. The final word is not
// part of that array and must survive sample-location updates.
enum { RASTER_SAMPLE_OFFSET = 4, RASTER_SAMPLE_COUNT = 16 };

static void raster_sample_bounds(int first, int count, const char *proc) {
    if ((unsigned)first >= RASTER_SAMPLE_COUNT || count > RASTER_SAMPLE_COUNT - first)
        dm_fail("%s: sample range exceeds 16 locations", proc);
}

static unsigned raster_sample_coordinate(float coordinate) {
    // The SDK scales by 16, adds 0.5, converts toward zero, then clamps.
    if (!(coordinate > 0.0f)) return 0;
    if (coordinate >= 0.9375f) return 15;
    return (unsigned)((double)coordinate * 16.0 + 0.5);
}

void nvn_multisample_sample_locations(NVNmultisampleState *s, int first, int count, const float *locations) {
    if (count <= 0) return;
    const char *proc = "nvnMultisampleStateSetSampleLocations";
    raster_sample_bounds(first, count, proc);
    raster_check(s, sizeof(*s), 1, proc);
    raster_check(locations, 2 * (size_t)count * sizeof(*locations), 0, proc);
    uint8_t *packed = (uint8_t *)s->reserved + RASTER_SAMPLE_OFFSET + first;
    for (int i = 0; i < count; ++i) {
        float xy[2];
        memcpy(xy, (const uint8_t *)locations + 2 * (size_t)i * sizeof(float), sizeof(xy));
        packed[i] = (uint8_t)(raster_sample_coordinate(xy[0]) | (raster_sample_coordinate(xy[1]) << 4));
    }
}

void nvn_multisample_sample_locations_get(const NVNmultisampleState *s, int first, int count, float *locations) {
    if (count <= 0) return;
    const char *proc = "nvnMultisampleStateGetSampleLocations";
    raster_sample_bounds(first, count, proc);
    raster_check(s, sizeof(*s), 0, proc);
    raster_check(locations, 2 * (size_t)count * sizeof(*locations), 1, proc);
    const uint8_t *packed = (const uint8_t *)s->reserved + RASTER_SAMPLE_OFFSET + first;
    for (int i = 0; i < count; ++i) {
        unsigned value = packed[i];
        float xy[2] = { (float)(value & 0xfu) * 0.0625f, (float)(value >> 4) * 0.0625f };
        memcpy((uint8_t *)locations + 2 * (size_t)i * sizeof(float), xy, sizeof(xy));
    }
}
