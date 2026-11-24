// Included by m3_services.c after the command-memory helpers. Records use the
// high 24 opcode bits for small arguments and copy only the elements supplied.
#include "m4_draw_state.h"
#include <math.h>

enum {
    DRAW_OP_TARGETS = COMMAND_DRAW_INSTANCED + 1, DRAW_OP_PROGRAM, DRAW_OP_ATTRIBS, DRAW_OP_STREAMS,
    DRAW_OP_VERTEX, DRAW_OP_UNIFORM, DRAW_OP_STORAGE, DRAW_OP_TEXTURES,
    DRAW_OP_TEXTURE_POOL, DRAW_OP_SAMPLER_POOL, DRAW_OP_VIEWPORTS,
    DRAW_OP_SCISSORS, DRAW_OP_DEPTH_RANGES, DRAW_OP_COLOR, DRAW_OP_BLEND,
    DRAW_OP_CHANNEL_MASK, DRAW_OP_DEPTH_STENCIL, DRAW_OP_POLYGON, DRAW_OP_MULTISAMPLE,
    DRAW_OP_BLEND_COLOR, DRAW_OP_POLYGON_OFFSET, DRAW_OP_DEPTH_CLAMP, DRAW_OP_STENCIL, DRAW_OP_SAMPLE_MASK,
    DRAW_OP_RASTERIZER_DISCARD, DRAW_OP_CONSERVATIVE_RASTER,
    DRAW_OP_IMAGES, DRAW_OP_COMPUTE_PUSH, DRAW_OP_LINE_WIDTH, DRAW_OP_POINT_SIZE, DRAW_OP_DEPTH_BOUNDS, DRAW_OP_GEOMETRY_MODE,
    DRAW_OP_RATE_PALETTE, DRAW_OP_RATE_IMAGE, DRAW_OP_RATE_PALETTE_MODE,
    DRAW_OP_PRIMITIVE_RESTART,
};
static_assert(sizeof(((struct headless_program *)0)->keys) == DM_M4_DRAW_STAGES * 8, "program and replay stage count");
static_assert(DRAW_OP_PRIMITIVE_RESTART < 0x60 && DM_M4_DRAW_TEXTURES <= 128 && DM_M4_DRAW_IMAGES <= 32,
              "packed state opcode fields");
static_assert(sizeof(struct dm_m4_draw_viewport) == 16 && sizeof(struct dm_m4_draw_scissor) == 16 &&
              sizeof(struct dm_m4_draw_depth_range) == 8, "SDK rectangle strides");
static_assert(sizeof(struct texture_view) == sizeof(struct dm_m4_texture_view), "texture view layout");
static_assert(sizeof(struct sampler_state) == sizeof(struct dm_m4_draw_sampler), "sampler state layout");

static unsigned draw_popcount(unsigned value) {
    unsigned count = 0;
    for (; value; value &= value - 1) ++count;
    return count;
}

static uint8_t *draw_record(struct command_buffer *c, unsigned op, unsigned args, size_t payload) {
    size_t size = (sizeof(struct command_header) + payload + 7) & ~(size_t)7;
    struct command_header header = {op | (args << 8), (uint32_t)size};
    uint8_t *dst = command_reserve(c, size);
    memcpy(dst, &header, sizeof(header));
    if (size > sizeof(header) + payload)
        memset(dst + sizeof(header) + payload, 0, size - sizeof(header) - payload);
    return dst + sizeof(header);
}

static void draw_require_recording(const struct command_buffer *c) {
    command_check(c, 1);
    if (!c->recording) dm_fail("NVN command buffer: command outside recording");
}

static void draw_guest_span(const void *data, size_t bytes, const char *name) {
    if (bytes && !dm_guest_range(data, bytes, 0)) dm_fail("%s: invalid guest input span", name);
}

static int draw_range_fits(unsigned first, unsigned count, unsigned limit) {
    return first <= limit && count <= limit - first;
}

// [6:0] first, [14:7] count, [17:15] stage, [19:18] encoding or origin.
static unsigned draw_slot_args(unsigned first, unsigned count, unsigned stage, unsigned mode) {
    return first | (count << 7) | (stage << 15) | (mode << 18);
}

static struct dm_m4_draw_texture draw_texture_snapshot(const struct headless_texture *t,
                                                      const struct texture_view *v) {
    struct dm_m4_draw_texture result = {.pool = t->pool, .offset = t->offset, .desc = t->desc,
        .depth_stencil_mode = t->depth_stencil_mode};
    memcpy(result.swizzle, t->swizzle, sizeof(result.swizzle));
    if (v) memcpy(&result.view, v, sizeof(result.view));
    return result;
}

static int draw_view_fits(const struct dm_m4_draw_texture *t, const nvn_texlayout *layout) {
    const struct dm_m4_texture_view *v = &t->view;
    if (v->set & ~(DM_M4_VIEW_LEVELS | DM_M4_VIEW_LAYERS | DM_M4_VIEW_FORMAT | DM_M4_VIEW_TARGET |
                   DM_M4_VIEW_SWIZZLE | DM_M4_VIEW_DEPTH_STENCIL)) return 0;
    unsigned level = v->set & DM_M4_VIEW_LEVELS ? v->base_level : 0;
    if (level >= t->desc.levels || ((v->set & DM_M4_VIEW_LEVELS) &&
        v->levels > t->desc.levels - level)) return 0;
    if (v->set & DM_M4_VIEW_LAYERS) {
        unsigned layers = t->desc.target == NVN_TEXTURE_TARGET_3D ? mip_dim(t->desc.depth, level) : layout->layers;
        if (v->base_layer >= layers || v->layers > layers - v->base_layer) return 0;
    }
    return 1;
}

void nvn_command_set_render_targets(struct command_buffer *c, int count,
        const struct headless_texture *const *colors, const struct texture_view *const *views,
        const struct headless_texture *depth, const struct texture_view *depth_view) {
    const char *name = "nvnCommandBufferSetRenderTargets";
    draw_require_recording(c);
    if ((unsigned)count > DM_M4_DRAW_TARGETS) dm_fail("%s: invalid color count %d", name, count);
    if (colors) draw_guest_span(colors, (size_t)count * sizeof(*colors), name);
    if (views) draw_guest_span(views, (size_t)count * sizeof(*views), name);
    unsigned used = depth ? 1u << DM_M4_DRAW_TARGETS : 0;
    for (int i = 0; colors && i < count; ++i) if (colors[i]) used |= 1u << i;
    uint8_t *dst = draw_record(c, DRAW_OP_TARGETS, (unsigned)count | (used << 4),
                               draw_popcount(used) * sizeof(struct dm_m4_draw_texture));
    for (int i = 0; i <= count; ++i) {
        const struct headless_texture *t = i == count ? depth : colors ? colors[i] : NULL;
        if (!t) continue;
        const struct texture_view *v = i == count ? depth_view : views ? views[i] : NULL;
        draw_guest_span(t, sizeof(*t), name);
        if (t->magic != HEADLESS_TEXTURE) dm_fail("%s: invalid texture", name);
        draw_guest_span(v, v ? sizeof(*v) : 0, name);
        struct dm_m4_draw_texture snap = draw_texture_snapshot(t, v);
        if (!draw_view_fits(&snap, &t->layout)) dm_fail("%s: invalid attachment subresource", name);
        pool_check(t->pool, 0);
        if (t->pool->device != c->device) dm_fail("%s: attachment belongs to another device", name);
        memcpy(dst, &snap, sizeof(snap));
        dst += sizeof(snap);
    }
}

void nvn_command_rate_palette(struct command_buffer *c, int table, int count, const uint32_t *entries) {
    const char *name = "nvnCommandBufferShadingRateImagePalette";
    draw_require_recording(c);
    if ((unsigned)table >= DM_M4_RATE_TABLES || (unsigned)count > DM_M4_RATE_ENTRIES)
        dm_fail("%s: invalid table %d or entry count %d", name, table, count);
    // The SDK disables this table on NULL, regardless of the palette's old contents.
    unsigned args = (unsigned)table << 1;
    if (!entries) {
        draw_record(c, DRAW_OP_RATE_PALETTE, args, 0);
        return;
    }
    draw_guest_span(entries, (size_t)count * sizeof(*entries), name);
    uint64_t packed = 0;
    for (int i = 0; i < count; ++i) {
        uint32_t rate;
        memcpy(&rate, (const uint8_t *)entries + i * sizeof(rate), sizeof(rate));
        if (rate > DM_M4_RATE_4X4) dm_fail("%s: invalid entry %d rate %u", name, i, rate);
        packed |= (uint64_t)rate << (4 * i);
    }
    if (count < DM_M4_RATE_ENTRIES)
        packed |= (UINT64_C(0x1111111111111111) * DM_M4_RATE_1X1) << (4 * count);
    memcpy(draw_record(c, DRAW_OP_RATE_PALETTE, args | 1, sizeof(packed)), &packed, sizeof(packed));
}

void nvn_command_bind_rate_image(struct command_buffer *c, const struct headless_texture *t,
                                  const struct texture_view *v) {
    const char *name = "nvnCommandBufferBindShadingRateImage";
    draw_require_recording(c);
    if (!t) {
        draw_record(c, DRAW_OP_RATE_IMAGE, 0, 0);
        return;
    }
    draw_guest_span(t, sizeof(*t), name);
    if (t->magic != HEADLESS_TEXTURE) dm_fail("%s: invalid texture", name);
    draw_guest_span(v, v ? sizeof(*v) : 0, name);
    struct dm_m4_draw_texture snap = draw_texture_snapshot(t, v);
    if (!draw_view_fits(&snap, &t->layout)) dm_fail("%s: invalid image subresource", name);
    pool_check(t->pool, 0);
    if (t->pool->device != c->device) dm_fail("%s: image belongs to another device", name);
    memcpy(draw_record(c, DRAW_OP_RATE_IMAGE, 1, sizeof(snap)), &snap, sizeof(snap));
}

void nvn_command_rate_palette_mode(struct command_buffer *c, int mode) {
    draw_require_recording(c);
    if ((unsigned)mode > DM_M4_RATE_PALETTE_VIEWPORT)
        dm_fail("nvnCommandBufferSetShadingRatePaletteMode: invalid mode %d", mode);
    draw_record(c, DRAW_OP_RATE_PALETTE_MODE, (unsigned)mode, 0);
}

enum {
    DRAW_PROGRAM_MESH = 1u << DM_M4_NV_MESH_STAGE,
    DRAW_PROGRAM_POINT_STAGES = (unsigned)NVN_SHADER_STAGE_VERTEX_BIT | DRAW_PROGRAM_MESH,
    DRAW_PROGRAM_RATE_MIDDLE = DM_M4_RATE_INDEX_CLASSIC & ~(unsigned)NVN_SHADER_STAGE_VERTEX_BIT,
    DRAW_PROGRAM_RATE_VERTEX_ARG = 1u << 1,
    DRAW_PROGRAM_RATE_MESH_ARG = 1u << 5,
    DRAW_PROGRAM_RATE_ARGS = DRAW_PROGRAM_RATE_MIDDLE | DRAW_PROGRAM_RATE_VERTEX_ARG | DRAW_PROGRAM_RATE_MESH_ARG,
};
static_assert(!(DRAW_PROGRAM_POINT_STAGES & DRAW_PROGRAM_RATE_ARGS), "packed shader output metadata");

void nvn_command_bind_program(struct command_buffer *c, const struct headless_program *p, int mask) {
    const char *name = "nvnCommandBufferBindProgram";
    draw_require_recording(c);
    unsigned stages = (unsigned)mask, enabled = 0;
    const unsigned supported = (1u << DM_M4_DRAW_STAGES) - 1;
    if (stages & ~supported) dm_fail("%s: unsupported stage mask %#x", name, stages);
    if (p) {
        draw_guest_span(p, sizeof(*p), name);
        if (p->magic != HEADLESS_PROGRAM || p->device != c->device || (p->stage_mask & ~supported))
            dm_fail("%s: invalid program", name);
        // NVN leaves the vertex stage alone when a non-mesh graphics program
        // containing only later stages is bound with the vertex bit set.
        if ((p->stage_mask & 0x1e) && !(p->stage_mask & (1u | (1u << 7)))) stages &= ~1u;
        // A compute-only program does not replace the active graphics program.
        if (p->stage_mask == NVN_SHADER_STAGE_COMPUTE_BIT && (stages & NVN_SHADER_STAGE_COMPUTE_BIT))
            stages = NVN_SHADER_STAGE_COMPUTE_BIT;
        enabled = stages & p->stage_mask;
    }
    if (!stages) return;
    unsigned point_size = p ? enabled & p->point_size_outputs : 0;
    unsigned rate_index = p ? enabled & p->rate_index_outputs & DM_M4_RATE_INDEX_STAGES : 0;
    unsigned outputs = point_size | (rate_index & DRAW_PROGRAM_RATE_MIDDLE) |
                       ((rate_index & NVN_SHADER_STAGE_VERTEX_BIT) << 1) |
                       ((rate_index & DRAW_PROGRAM_MESH) >> 2);
    uint8_t *dst = draw_record(c, DRAW_OP_PROGRAM, stages | (enabled << 8) | (outputs << 16),
                               draw_popcount(enabled) * 8u);
    for (unsigned st = 0; st < DM_M4_DRAW_STAGES; ++st) {
        if (!(enabled & (1u << st))) continue;
        memcpy(dst, p->keys[st], sizeof(p->keys[st]));
        dst += sizeof(p->keys[st]);
    }
}

static void draw_record_shape(struct command_buffer *c, unsigned op, unsigned count, unsigned limit,
                              const void *data, size_t elem, const char *name) {
    draw_require_recording(c);
    if (count > limit) dm_fail("%s: invalid state count %u", name, count);
    size_t bytes = count * elem;
    draw_guest_span(data, bytes, name);
    uint8_t *dst = draw_record(c, op, count, bytes);
    if (bytes) memcpy(dst, data, bytes);
}

void nvn_command_bind_vertex_attribs(struct command_buffer *c, int count, const NVNvertexAttribState *attrs) {
    draw_record_shape(c, DRAW_OP_ATTRIBS, (unsigned)count, DM_M4_DRAW_ATTRIBS, attrs, sizeof(*attrs),
                      "nvnCommandBufferBindVertexAttribState");
}
void nvn_command_bind_vertex_streams(struct command_buffer *c, int count, const NVNvertexStreamState *streams) {
    draw_record_shape(c, DRAW_OP_STREAMS, (unsigned)count, DM_M4_DRAW_STREAMS, streams, sizeof(*streams),
                      "nvnCommandBufferBindVertexStreamState");
}

// Non-deferred buffer bindings carry addresses, not their live contents.
// Store ordinary ranges in 12 bytes and aligned uniform ranges in 8 bytes.
// Fall back to a full NVNbufferRange for valid ranges larger than UINT32_MAX.
enum { DRAW_BUFFER_COMPACT, DRAW_BUFFER_FULL, DRAW_BUFFER_ALIGNED_UNIFORM };
static size_t draw_buffer_stride(unsigned mode) {
    return mode == DRAW_BUFFER_COMPACT ? 12 : mode == DRAW_BUFFER_ALIGNED_UNIFORM ? 8 : sizeof(NVNbufferRange);
}
static int draw_buffer_range_valid(NVNbufferRange range) {
    return range.size <= UINT64_MAX - range.address && (range.address || !range.size);
}

static void draw_record_buffers(struct command_buffer *c, unsigned op, unsigned stage, unsigned first,
                                unsigned count, unsigned limit, const NVNbufferRange *ranges, int guest, const char *name) {
    draw_require_recording(c);
    if (stage >= DM_M4_DRAW_STAGES || !draw_range_fits(first, count, limit))
        dm_fail("%s: invalid stage %u or slot range %u + %u", name, stage, first, count);
    if (guest) draw_guest_span(ranges, count * sizeof(*ranges), name);
    if (!count) return;
    int full = 0, aligned = op == DRAW_OP_UNIFORM;
    for (unsigned i = 0; i < count; ++i) {
        NVNbufferRange range = ranges[i];
        if (!draw_buffer_range_valid(range)) dm_fail("%s: invalid buffer range at slot %u", name, first + i);
        full |= range.size > UINT32_MAX;
        aligned &= !(range.address & 0xffu) && !(range.address >> 48) && range.size < (1u << 24);
    }
    unsigned mode = full ? DRAW_BUFFER_FULL : aligned ? DRAW_BUFFER_ALIGNED_UNIFORM : DRAW_BUFFER_COMPACT;
    size_t stride = draw_buffer_stride(mode);
    uint8_t *dst = draw_record(c, op, draw_slot_args(first, count, stage, mode), count * stride);
    if (mode == DRAW_BUFFER_FULL) {
        memcpy(dst, ranges, count * stride);
        return;
    }
    for (unsigned i = 0; i < count; ++i, dst += stride) {
        NVNbufferRange range = ranges[i];
        if (mode == DRAW_BUFFER_ALIGNED_UNIFORM) {
            uint64_t packed = (range.address >> 8) | ((uint64_t)range.size << 40);
            memcpy(dst, &packed, sizeof(packed));
        } else {
            uint32_t size = (uint32_t)range.size;
            memcpy(dst, &range.address, sizeof(range.address));
            memcpy(dst + sizeof(range.address), &size, sizeof(size));
        }
    }
}

void nvn_command_bind_vertex_buffer(struct command_buffer *c, int slot, NVNbufferAddress address, size_t size) {
    NVNbufferRange range = {address, size};
    draw_record_buffers(c, DRAW_OP_VERTEX, 0, (unsigned)slot, 1, DM_M4_DRAW_STREAMS, &range, 0,
                        "nvnCommandBufferBindVertexBuffer");
}
void nvn_command_bind_vertex_buffers(struct command_buffer *c, int first, int count, const NVNbufferRange *ranges) {
    draw_record_buffers(c, DRAW_OP_VERTEX, 0, (unsigned)first, (unsigned)count, DM_M4_DRAW_STREAMS, ranges, 1,
                        "nvnCommandBufferBindVertexBuffers");
}
#define DRAW_BIND_BUFFERS(name, nvn, op, slots) \
    void nvn_command_bind_##name##_buffer(struct command_buffer *c, NVNshaderStage stage, int slot, \
                                         NVNbufferAddress address, size_t size) { \
        NVNbufferRange range = {address, size}; \
        draw_record_buffers(c, op, (unsigned)stage, (unsigned)slot, 1, slots, &range, 0, \
                            "nvnCommandBufferBind" #nvn "Buffer"); \
    } \
    void nvn_command_bind_##name##_buffers(struct command_buffer *c, NVNshaderStage stage, int first, \
                                          int count, const NVNbufferRange *ranges) { \
        draw_record_buffers(c, op, (unsigned)stage, (unsigned)first, (unsigned)count, slots, ranges, 1, \
                            "nvnCommandBufferBind" #nvn "Buffers"); \
    }
DRAW_BIND_BUFFERS(uniform, Uniform, DRAW_OP_UNIFORM, DM_M4_DRAW_UBOS)
DRAW_BIND_BUFFERS(storage, Storage, DRAW_OP_STORAGE, DM_M4_DRAW_SSBOS)
#undef DRAW_BIND_BUFFERS

static void draw_record_handles(struct command_buffer *c, unsigned op, unsigned stage, unsigned first,
                                unsigned count, const uint64_t *handles, int guest, const char *name) {
    draw_require_recording(c);
    unsigned limit = op == DRAW_OP_IMAGES ? DM_M4_DRAW_IMAGES : DM_M4_DRAW_TEXTURES;
    if (stage >= DM_M4_DRAW_STAGES || !draw_range_fits(first, count, limit))
        dm_fail("%s: invalid stage %u or slot range %u + %u", name, stage, first, count);
    if (guest) draw_guest_span(handles, count * sizeof(*handles), name);
    if (!count) return;
    uint8_t *dst = draw_record(c, op, draw_slot_args(first, count, stage, 0), count * sizeof(uint32_t));
    for (unsigned i = 0; i < count; ++i) {
        uint32_t word = (uint32_t)handles[i]; // NVN writes the low 32 bits into the GPU handle table.
        memcpy(dst + i * sizeof(word), &word, sizeof(word));
    }
}
void nvn_command_bind_texture(struct command_buffer *c, NVNshaderStage stage, int slot, NVNtextureHandle handle) {
    draw_record_handles(c, DRAW_OP_TEXTURES, (unsigned)stage, (unsigned)slot, 1, &handle, 0,
                        "nvnCommandBufferBindTexture");
}
void nvn_command_bind_textures(struct command_buffer *c, NVNshaderStage stage, int first, int count,
                               const NVNtextureHandle *handles) {
    draw_record_handles(c, DRAW_OP_TEXTURES, (unsigned)stage, (unsigned)first, (unsigned)count, handles, 1,
                        "nvnCommandBufferBindTextures");
}
void nvn_command_bind_textures_deferred(struct command_buffer *c, unsigned stage, unsigned first, unsigned count,
                                        const NVNtextureHandle *handles) {
    draw_require_recording(c);
    if (stage >= DM_M4_DRAW_STAGES || !draw_range_fits(first, count, DM_M4_DRAW_TEXTURES))
        dm_fail("nvnCommandBufferBindTexturesDeferred: invalid stage %u or slot range %u + %u", stage, first, count);
    if (!count) return;
    uint8_t *dst = draw_record(c, DRAW_OP_TEXTURES, draw_slot_args(first, count, stage, 1), sizeof(handles));
    memcpy(dst, &handles, sizeof(handles));
}
void nvn_command_bind_image(struct command_buffer *c, NVNshaderStage stage, int slot, NVNimageHandle handle) {
    draw_record_handles(c, DRAW_OP_IMAGES, (unsigned)stage, (unsigned)slot, 1, &handle, 0,
                        "nvnCommandBufferBindImage");
}
void nvn_command_bind_images(struct command_buffer *c, NVNshaderStage stage, int first, int count,
                             const NVNimageHandle *handles) {
    draw_record_handles(c, DRAW_OP_IMAGES, (unsigned)stage, (unsigned)first, (unsigned)count, handles, 1,
                        "nvnCommandBufferBindImages");
}

enum { DRAW_PUSH_OFFSET_BITS = 11, DRAW_PUSH_OFFSET_MASK = (1u << DRAW_PUSH_OFFSET_BITS) - 1 };
static_assert(DMSH_PUSH_BLOCKS * DMSH_PUSH_SIZE == DRAW_PUSH_OFFSET_MASK + 1 &&
              DMSH_PUSH_SIZE < (1u << (24 - DRAW_PUSH_OFFSET_BITS)), "packed push range");

void nvn_command_push_compute(struct command_buffer *c, uint64_t block, uint64_t off, uint64_t size,
                              const void *data) {
    const char *name = "nvnCommandBufferPushComputeConstants";
    draw_require_recording(c);
    if (block >= DMSH_PUSH_BLOCKS || off > DMSH_PUSH_SIZE || size > DMSH_PUSH_SIZE - off)
        dm_fail("%s: invalid block or byte range", name);
    if (!size) return;
    draw_guest_span(data, (size_t)size, name);
    unsigned args = (unsigned)(block * DMSH_PUSH_SIZE + off) | ((unsigned)size << DRAW_PUSH_OFFSET_BITS);
    memcpy(draw_record(c, DRAW_OP_COMPUTE_PUSH, args, (size_t)size), data, (size_t)size);
}

void nvn_command_dispatch_compute(struct command_buffer *c, int x, int y, int z) {
    draw_require_recording(c);
    if (x < 0 || y < 0 || z < 0) dm_fail("nvnCommandBufferDispatchCompute: negative group count");
    const uint32_t groups[] = {(uint32_t)x, (uint32_t)y, (uint32_t)z};
    memcpy(draw_record(c, COMMAND_DISPATCH_COMPUTE, 0, sizeof(groups)), groups, sizeof(groups));
}
void nvn_command_dispatch_indirect(struct command_buffer *c, NVNbufferAddress address) {
    draw_require_recording(c);
    if (address & 3) dm_fail("nvnCommandBufferDispatchComputeIndirect: misaligned argument address");
    memcpy(draw_record(c, COMMAND_DISPATCH_COMPUTE, 1, sizeof(address)), &address, sizeof(address));
}

static int draw_pool_live(const struct descriptor_table *t, const struct nvn_device *device) {
    return dm_guest_range(t->backing, sizeof(*t->backing), 0) &&
        t->backing->magic == HEADLESS_NVN_POOL && t->backing->device == device &&
        dm_guest_range((const void *)t->begin, t->end - t->begin, 0);
}

static void draw_record_pool(struct command_buffer *c, const struct descriptor_pool *pool, int sampler) {
    draw_require_recording(c);
    const char *name = sampler ? "nvnCommandBufferSetSamplerPool" : "nvnCommandBufferSetTexturePool";
    struct dm_m4_draw_pool snapshot = {0};
    if (pool) {
        draw_guest_span(pool, sizeof(*pool), name);
        AcquireSRWLockShared(&descriptor_lock);
        const struct descriptor_table *t = descriptor_table_find(pool, sampler);
        int live = t && draw_pool_live(t, c->device);
        if (live) snapshot = (struct dm_m4_draw_pool){t->begin, (uint32_t)t->count};
        ReleaseSRWLockShared(&descriptor_lock);
        if (!live) dm_fail("%s: invalid pool or device", name);
    }
    // Descriptors themselves are updated independently. Record the pool base
    // and size, not a pointer to the CPU-side NVN descriptor-pool object.
    memcpy(draw_record(c, sampler ? DRAW_OP_SAMPLER_POOL : DRAW_OP_TEXTURE_POOL,
                       snapshot.count, sizeof(snapshot.address)), &snapshot.address, sizeof(snapshot.address));
}
void nvn_command_set_texture_pool(struct command_buffer *c, const struct descriptor_pool *pool) { draw_record_pool(c, pool, 0); }
void nvn_command_set_sampler_pool(struct command_buffer *c, const struct descriptor_pool *pool) { draw_record_pool(c, pool, 1); }

static int draw_viewport_valid(struct dm_m4_draw_viewport v) {
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.width) && isfinite(v.height) && v.width >= 0 && v.height >= 0;
}
static int draw_scissor_valid(struct dm_m4_draw_scissor r) { return r.width >= 0 && r.height >= 0; }
static int draw_depth_range_valid(struct dm_m4_draw_depth_range r) {
    return r.near_z >= 0 && r.near_z <= 1 && r.far_z >= 0 && r.far_z <= 1;
}

static void draw_record_rects(struct command_buffer *c, unsigned op, unsigned first, unsigned count,
                              const void *data, int guest, const char *name) {
    draw_require_recording(c);
    if (!draw_range_fits(first, count, DM_M4_DRAW_VIEWPORTS))
        dm_fail("%s: invalid slot range %u + %u", name, first, count);
    size_t stride = op == DRAW_OP_DEPTH_RANGES ? 8 : 16;
    if (guest) draw_guest_span(data, count * stride, name);
    if (!count) return;
    unsigned origin = 0;
    if (op == DRAW_OP_VIEWPORTS) {
        if (!dm_guest_range(c->device, sizeof(*c->device), 0) || c->device->magic != HEADLESS_NVN_DEVICE || c->device->origin > 1)
            dm_fail("%s: invalid device origin", name);
        origin = c->device->origin;
    }
    uint8_t *dst = draw_record(c, op, draw_slot_args(first, count, 0, origin), count * stride);
    memcpy(dst, data, count * stride);
    for (unsigned i = 0; i < count; ++i, dst += stride) {
        int valid;
        if (op == DRAW_OP_VIEWPORTS) {
            struct dm_m4_draw_viewport v;
            memcpy(&v, dst, sizeof(v));
            valid = draw_viewport_valid(v);
        } else if (op == DRAW_OP_SCISSORS) {
            struct dm_m4_draw_scissor r;
            memcpy(&r, dst, sizeof(r));
            valid = draw_scissor_valid(r);
        } else {
            struct dm_m4_draw_depth_range r;
            memcpy(&r, dst, sizeof(r));
            valid = draw_depth_range_valid(r);
        }
        if (!valid) dm_fail("%s: invalid values for slot %u", name, first + i);
    }
}
void nvn_command_set_viewport(struct command_buffer *c, int x, int y, int w, int h) {
    struct dm_m4_draw_viewport v = {(float)x, (float)y, (float)w, (float)h};
    draw_record_rects(c, DRAW_OP_VIEWPORTS, 0, 1, &v, 0, "nvnCommandBufferSetViewport");
}
void nvn_command_set_viewports(struct command_buffer *c, int first, int count, const float *rects) {
    draw_record_rects(c, DRAW_OP_VIEWPORTS, (unsigned)first, (unsigned)count, rects, 1, "nvnCommandBufferSetViewports");
}
void nvn_command_set_scissor(struct command_buffer *c, int x, int y, int w, int h) {
    struct dm_m4_draw_scissor r = {x, y, w, h};
    draw_record_rects(c, DRAW_OP_SCISSORS, 0, 1, &r, 0, "nvnCommandBufferSetScissor");
}
void nvn_command_set_scissors(struct command_buffer *c, int first, int count, const int *rects) {
    draw_record_rects(c, DRAW_OP_SCISSORS, (unsigned)first, (unsigned)count, rects, 1, "nvnCommandBufferSetScissors");
}
void nvn_command_set_depth_range(struct command_buffer *c, float near_z, float far_z) {
    struct dm_m4_draw_depth_range r = {near_z, far_z};
    draw_record_rects(c, DRAW_OP_DEPTH_RANGES, 0, 1, &r, 0, "nvnCommandBufferSetDepthRange");
}
void nvn_command_set_depth_ranges(struct command_buffer *c, int first, int count, const float *ranges) {
    draw_record_rects(c, DRAW_OP_DEPTH_RANGES, (unsigned)first, (unsigned)count, ranges, 1, "nvnCommandBufferSetDepthRanges");
}

#define DRAW_BIND_RASTER(name, type, nvn, op) \
    void nvn_command_bind_##name(struct command_buffer *c, const NVN##type *s) { \
        draw_require_recording(c); \
        draw_guest_span(s, sizeof(*s), "nvnCommandBufferBind" #nvn); \
        memcpy(draw_record(c, op, 0, sizeof(*s)), s, sizeof(*s)); \
    }
DRAW_BIND_RASTER(color, colorState, ColorState, DRAW_OP_COLOR)
DRAW_BIND_RASTER(blend, blendState, BlendState, DRAW_OP_BLEND)
DRAW_BIND_RASTER(channel_mask, channelMaskState, ChannelMaskState, DRAW_OP_CHANNEL_MASK)
DRAW_BIND_RASTER(depth_stencil, depthStencilState, DepthStencilState, DRAW_OP_DEPTH_STENCIL)
DRAW_BIND_RASTER(polygon, polygonState, PolygonState, DRAW_OP_POLYGON)
DRAW_BIND_RASTER(multisample, multisampleState, MultisampleState, DRAW_OP_MULTISAMPLE)
#undef DRAW_BIND_RASTER
void nvn_command_set_blend_color(struct command_buffer *c, const float *rgba) {
    draw_require_recording(c);
    draw_guest_span(rgba, 4 * sizeof(float), "nvnCommandBufferSetBlendColor");
    memcpy(draw_record(c, DRAW_OP_BLEND_COLOR, 0, 4 * sizeof(float)), rgba, 4 * sizeof(float));
}
void nvn_command_set_polygon_offset_clamp(struct command_buffer *c, float factor, float units, float clamp) {
    draw_require_recording(c);
    struct dm_m4_draw_polygon_offset offset = {factor, units, clamp};
    memcpy(draw_record(c, DRAW_OP_POLYGON_OFFSET, 0, sizeof(offset)), &offset, sizeof(offset));
}
void nvn_command_set_depth_bounds(struct command_buffer *c, NVNboolean enable, float lo, float hi) {
    draw_require_recording(c);
    const struct dm_m4_draw_depth_range bounds = {lo, hi};
    memcpy(draw_record(c, DRAW_OP_DEPTH_BOUNDS, enable != 0, sizeof(bounds)), &bounds, sizeof(bounds));
}
void nvn_command_set_depth_clamp(struct command_buffer *c, NVNboolean enabled) {
    draw_require_recording(c);
    draw_record(c, DRAW_OP_DEPTH_CLAMP, enabled != 0, 0);
}
void nvn_command_set_line_width(struct command_buffer *c, float width) {
    draw_require_recording(c);
    memcpy(draw_record(c, DRAW_OP_LINE_WIDTH, 0, sizeof(width)), &width, sizeof(width));
}
void nvn_command_set_point_size(struct command_buffer *c, float size) {
    draw_require_recording(c);
    memcpy(draw_record(c, DRAW_OP_POINT_SIZE, 0, sizeof(size)), &size, sizeof(size));
}
void nvn_command_set_primitive_restart(struct command_buffer *c, NVNboolean enable, uint32_t index) {
    draw_require_recording(c);
    memcpy(draw_record(c, DRAW_OP_PRIMITIVE_RESTART, enable != 0, sizeof(index)), &index, sizeof(index));
}
void nvn_command_set_geometry_mode(struct command_buffer *c, int mode) {
    draw_require_recording(c);
    // NVN2's geometry-mode MME switches the classic and mesh pipeline paths.
    // The game's modes 1 and 2 both issue DrawMeshTasks; Vulkan derives its
    // task stage and workgroup entry point from the bound mesh program.
    if ((unsigned)mode > 2) dm_fail("nvnCommandBufferSetGeometryProcessingMode: invalid mode %d", mode);
    draw_record(c, DRAW_OP_GEOMETRY_MODE, (unsigned)mode, 0);
}
void nvn_command_rasterizer_discard(struct command_buffer *c, uint32_t enable) {
    draw_require_recording(c);
    draw_record(c, DRAW_OP_RASTERIZER_DISCARD, enable != 0, 0);
}
void nvn_command_conservative_raster(struct command_buffer *c, NVNboolean enable) {
    draw_require_recording(c);
    draw_record(c, DRAW_OP_CONSERVATIVE_RASTER, enable != 0, 0);
}
void nvn_command_set_sample_mask(struct command_buffer *c, int mask) {
    draw_require_recording(c);
    // The SDK writes the low 16 bits to each hardware sample-mask register.
    draw_record(c, DRAW_OP_SAMPLE_MASK, (uint16_t)mask, 0);
}

enum { DRAW_STENCIL_COMPARE_MASK, DRAW_STENCIL_WRITE_MASK, DRAW_STENCIL_REF };
static void draw_record_stencil(struct command_buffer *c, NVNface face, unsigned field, int value) {
    draw_require_recording(c);
    // Native NVN consumes only the two face bits and the low stencil byte.
    unsigned faces = (unsigned)face & NVN_FACE_FRONT_AND_BACK;
    if (faces) draw_record(c, DRAW_OP_STENCIL, faces | (field << 2) | ((unsigned)(uint8_t)value << 4), 0);
}
void nvn_command_set_stencil_value_mask(struct command_buffer *c, NVNface face, int mask) {
    draw_record_stencil(c, face, DRAW_STENCIL_COMPARE_MASK, mask);
}
void nvn_command_set_stencil_mask(struct command_buffer *c, NVNface face, int mask) {
    draw_record_stencil(c, face, DRAW_STENCIL_WRITE_MASK, mask);
}
void nvn_command_set_stencil_ref(struct command_buffer *c, NVNface face, int ref) {
    draw_record_stencil(c, face, DRAW_STENCIL_REF, ref);
}

struct draw_queue_state {
    struct draw_queue_state *next;
    const struct headless_queue *queue;
    struct dm_m4_draw_state state;
    struct dm_m4_guest_resources resources;
};
static struct draw_queue_state *draw_queues;

// Called with submit_lock held; copy-only queues need no graphics state.
static struct dm_m4_draw_state *draw_queue_state(const struct headless_queue *q) {
    for (struct draw_queue_state *entry = draw_queues; entry; entry = entry->next)
        if (entry->queue == q) return &entry->state;
    struct draw_queue_state *entry = calloc(1, sizeof(*entry));
    if (!entry) dm_fail("nvnQueueSubmitCommands: cannot allocate draw state");
    entry->queue = q;
    entry->next = draw_queues;
    draw_queues = entry;

    struct dm_m4_draw_state *s = &entry->state;
    // The production NVN2 defaults use the packed layouts of m3_raster_state.inc.
    const uint32_t color = 0x08030000, channel_mask = UINT32_MAX, polygon = 0x14;
    const uint32_t ds[] = {0x22, 0x11181118};
    const uint32_t ms[] = {5, 0x88888888, 0x88888888, 0x88888888, 0x88888888, 0};
    memcpy(&s->color, &color, sizeof(s->color));
    memcpy(&s->channel_mask, &channel_mask, sizeof(s->channel_mask));
    memcpy(&s->polygon, &polygon, sizeof(s->polygon));
    memcpy(&s->depth_stencil, ds, sizeof(s->depth_stencil));
    memcpy(&s->multisample, ms, sizeof(s->multisample));
    s->stencil[0] = s->stencil[1] = (struct dm_m4_draw_stencil){UINT8_MAX, UINT8_MAX, 0};
    s->sample_mask = UINT16_MAX;
    s->line_width = s->point_size = 1.0f;
    for (unsigned i = 0; i < DM_M4_DRAW_TARGETS; ++i) {
        const uint32_t blend[] = {0x110c0000 | i, 0x01020102};
        memcpy(&s->blend[i], blend, sizeof(s->blend[i]));
    }
    for (unsigned i = 0; i < DM_M4_DRAW_VIEWPORTS; ++i) s->depth_ranges[i].far_z = 1;
    return s;
}

static void m4_draw_queue_drop(const struct headless_queue *q) {
    struct draw_queue_state **prev = &draw_queues;
    while (*prev && (*prev)->queue != q) prev = &(*prev)->next;
    if (*prev) {
        struct draw_queue_state *entry = *prev;
        *prev = entry->next;
        dm_m4_guest_resources_destroy(&entry->resources);
        free(entry);
    }
}

static void draw_check_record(struct command_header h, int valid, size_t payload) {
    if (!valid || h.size != ((sizeof(h) + payload + 7) & ~(size_t)7))
        dm_fail("nvnQueueSubmitCommands: invalid graphics or compute record %#x", h.op);
}

static void draw_replay_buffers(struct dm_m4_draw_state *s, struct command_header h, const uint8_t *src,
                                unsigned op, unsigned stage, unsigned first, unsigned count, unsigned mode) {
    unsigned limit = op == DRAW_OP_VERTEX ? DM_M4_DRAW_STREAMS : op == DRAW_OP_UNIFORM ? DM_M4_DRAW_UBOS : DM_M4_DRAW_SSBOS;
    size_t stride = draw_buffer_stride(mode);
    draw_check_record(h, count && draw_range_fits(first, count, limit) && (h.op >> 28) == 0 &&
        (op != DRAW_OP_VERTEX || !stage) && mode <= DRAW_BUFFER_ALIGNED_UNIFORM &&
        (op == DRAW_OP_UNIFORM || mode != DRAW_BUFFER_ALIGNED_UNIFORM), count * stride);
    NVNbufferRange *out = op == DRAW_OP_VERTEX ? s->vertex : op == DRAW_OP_UNIFORM ? s->uniform[stage] : s->storage[stage];
    for (unsigned i = 0; i < count; ++i, src += stride) {
        NVNbufferRange range;
        if (mode == DRAW_BUFFER_FULL) {
            memcpy(&range, src, sizeof(range));
        } else if (mode == DRAW_BUFFER_ALIGNED_UNIFORM) {
            uint64_t packed;
            memcpy(&packed, src, sizeof(packed));
            range = (NVNbufferRange){(packed & ((UINT64_C(1) << 40) - 1)) << 8, packed >> 40};
        } else {
            uint32_t size;
            memcpy(&range.address, src, sizeof(range.address));
            memcpy(&size, src + sizeof(range.address), sizeof(size));
            range.size = size;
        }
        if (!draw_buffer_range_valid(range)) dm_fail("nvnQueueSubmitCommands: invalid recorded buffer range");
        out[first + i] = range;
    }
    if (op == DRAW_OP_UNIFORM) s->uniform_set[stage] |= (uint16_t)(((1u << count) - 1) << first);
}

static void draw_replay_rects(struct dm_m4_draw_state *s, struct command_header h, const uint8_t *src,
                              unsigned op, unsigned first, unsigned count, unsigned mode) {
    size_t stride = op == DRAW_OP_DEPTH_RANGES ? 8 : 16;
    unsigned args = h.op >> 8;
    draw_check_record(h, count && draw_range_fits(first, count, DM_M4_DRAW_VIEWPORTS) &&
        args == draw_slot_args(first, count, 0, mode) && mode <= (unsigned)(op == DRAW_OP_VIEWPORTS), count * stride);
    for (unsigned i = first; i < first + count; ++i, src += stride) {
        int valid;
        if (op == DRAW_OP_VIEWPORTS) {
            memcpy(&s->viewports[i], src, stride);
            s->viewport_set |= (uint16_t)(1u << i);
            s->viewport_origin[i] = (uint8_t)mode;
            valid = draw_viewport_valid(s->viewports[i]);
        } else if (op == DRAW_OP_SCISSORS) {
            memcpy(&s->scissors[i], src, stride);
            s->scissor_set |= (uint16_t)(1u << i);
            valid = draw_scissor_valid(s->scissors[i]);
        } else {
            memcpy(&s->depth_ranges[i], src, stride);
            valid = draw_depth_range_valid(s->depth_ranges[i]);
        }
        if (!valid) dm_fail("nvnQueueSubmitCommands: invalid graphics rectangle record %#x", h.op);
    }
}

static int draw_reduce(const struct headless_queue *q, struct dm_m4_draw_state **state,
                       struct command_header h, const uint8_t *src) {
    unsigned op = h.op & 0xff, args = h.op >> 8;
    if (op < DRAW_OP_TARGETS || op > DRAW_OP_PRIMITIVE_RESTART) return 0;
    if (!*state) *state = draw_queue_state(q);
    struct dm_m4_draw_state *s = *state;
    unsigned first = args & 0x7f, count = (args >> 7) & 0xff, stage = (args >> 15) & 7, mode = (args >> 18) & 3;
    switch (op) {
    case DRAW_OP_TARGETS: {
        unsigned n = args & 15, used = args >> 4;
        unsigned allowed = ((1u << n) - 1) | (1u << DM_M4_DRAW_TARGETS);
        draw_check_record(h, n <= DM_M4_DRAW_TARGETS && !(used & ~allowed),
                          draw_popcount(used) * sizeof(struct dm_m4_draw_texture));
        for (unsigned i = 0; i <= DM_M4_DRAW_TARGETS; ++i) {
            struct dm_m4_draw_texture *out = i == DM_M4_DRAW_TARGETS ? &s->depth : &s->colors[i];
            if (used & (1u << i)) {
                memcpy(out, src, sizeof(*out));
                src += sizeof(*out);
                if (!out->pool) dm_fail("nvnQueueSubmitCommands: invalid recorded render target");
            } else {
                out->pool = NULL;
            }
        }
        s->color_count = n;
        break;
    }
    case DRAW_OP_PROGRAM: {
        unsigned stages = args & 0xff, enabled = (args >> 8) & 0xff, outputs = args >> 16;
        unsigned point_size = outputs & DRAW_PROGRAM_POINT_STAGES;
        unsigned rate_index = (outputs & DRAW_PROGRAM_RATE_MIDDLE) |
                              ((outputs & DRAW_PROGRAM_RATE_VERTEX_ARG) >> 1) |
                              ((outputs & DRAW_PROGRAM_RATE_MESH_ARG) << 2);
        draw_check_record(h, stages && !(enabled & ~stages) && !((point_size | rate_index) & ~enabled) &&
            !(outputs & ~(DRAW_PROGRAM_POINT_STAGES | DRAW_PROGRAM_RATE_ARGS)), draw_popcount(enabled) * 8u);
        s->shader_mask = (s->shader_mask & ~stages) | enabled;
        s->point_size_outputs = (s->point_size_outputs & ~stages) | point_size;
        s->rate_index_outputs = (s->rate_index_outputs & ~stages) | rate_index;
        for (unsigned i = 0; i < DM_M4_DRAW_STAGES; ++i) {
            if (enabled & (1u << i)) {
                memcpy(s->shader_keys[i], src, sizeof(s->shader_keys[i]));
                src += sizeof(s->shader_keys[i]);
            } else if (stages & (1u << i)) {
                memset(s->shader_keys[i], 0, sizeof(s->shader_keys[i]));
            }
        }
        break;
    }
    case DRAW_OP_ATTRIBS:
        if (args > ARRAYSIZE(s->attribs)) dm_fail("nvnQueueSubmitCommands: too many vertex attributes");
        draw_check_record(h, 1, args * sizeof(s->attribs[0]));
        memcpy(s->attribs, src, args * sizeof(s->attribs[0]));
        s->attrib_count = args;
        break;
    case DRAW_OP_STREAMS:
        if (args > ARRAYSIZE(s->streams)) dm_fail("nvnQueueSubmitCommands: too many vertex streams");
        draw_check_record(h, 1, args * sizeof(s->streams[0]));
        memcpy(s->streams, src, args * sizeof(s->streams[0]));
        s->stream_count = args;
        break;
    case DRAW_OP_VERTEX: case DRAW_OP_UNIFORM: case DRAW_OP_STORAGE:
        draw_replay_buffers(s, h, src, op, stage, first, count, mode);
        break;
    case DRAW_OP_TEXTURES:
        draw_check_record(h, count && mode <= 1 && draw_range_fits(first, count, DM_M4_DRAW_TEXTURES) &&
            args == draw_slot_args(first, count, stage, mode), mode ? sizeof(void *) : count * sizeof(s->textures[0][0]));
        if (mode) {
            const uint8_t *handles;
            memcpy(&handles, src, sizeof(handles));
            draw_guest_span(handles, count * sizeof(NVNtextureHandle), "nvnQueueSubmitCommands: deferred texture handles");
            for (unsigned i = 0; i < count; ++i)
                memcpy(&s->textures[stage][first + i], handles + i * sizeof(NVNtextureHandle), sizeof(uint32_t));
        } else memcpy(&s->textures[stage][first], src, count * sizeof(s->textures[0][0]));
        break;
    case DRAW_OP_IMAGES: {
        draw_check_record(h, count && draw_range_fits(first, count, DM_M4_DRAW_IMAGES) &&
            args == draw_slot_args(first, count, stage, 0), count * sizeof(s->images[0][0]));
        memcpy(&s->images[stage][first], src, count * sizeof(s->images[0][0]));
        break;
    }
    case DRAW_OP_COMPUTE_PUSH: {
        unsigned offset = args & DRAW_PUSH_OFFSET_MASK, size = args >> DRAW_PUSH_OFFSET_BITS;
        draw_check_record(h, size && size <= DMSH_PUSH_SIZE - offset % DMSH_PUSH_SIZE, size);
        memcpy(s->compute_push + offset, src, size);
        break;
    }
    case DRAW_OP_TEXTURE_POOL: case DRAW_OP_SAMPLER_POOL: {
        struct dm_m4_draw_pool pool = {.count = args};
        draw_check_record(h, 1, sizeof(pool.address));
        memcpy(&pool.address, src, sizeof(pool.address));
        if ((pool.address == 0) != (pool.count == 0))
            dm_fail("nvnQueueSubmitCommands: invalid recorded descriptor pool");
        *(op == DRAW_OP_TEXTURE_POOL ? &s->texture_pool : &s->sampler_pool) = pool;
        break;
    }
    case DRAW_OP_VIEWPORTS: case DRAW_OP_SCISSORS: case DRAW_OP_DEPTH_RANGES:
        draw_replay_rects(s, h, src, op, first, count, mode);
        break;
    case DRAW_OP_BLEND: {
        uint32_t word;
        draw_check_record(h, !args, sizeof(s->blend[0]));
        memcpy(&word, src, sizeof(word));
        memcpy(&s->blend[word & 7], src, sizeof(s->blend[0]));
        break;
    }
    case DRAW_OP_CONSERVATIVE_RASTER:
        draw_check_record(h, args <= 1, 0);
        s->conservative_raster = (NVNboolean)args;
        break;
    case DRAW_OP_RASTERIZER_DISCARD:
        draw_check_record(h, args <= 1, 0);
        s->rasterizer_discard = (NVNboolean)args;
        break;
    case DRAW_OP_DEPTH_BOUNDS:
        draw_check_record(h, args <= 1, sizeof(s->depth_bounds));
        memcpy(&s->depth_bounds, src, sizeof(s->depth_bounds));
        s->depth_bounds_test = (NVNboolean)args;
        break;
    case DRAW_OP_DEPTH_CLAMP:
        draw_check_record(h, args <= 1, 0);
        s->depth_clamp = (NVNboolean)args;
        break;
    case DRAW_OP_GEOMETRY_MODE:
        draw_check_record(h, args <= 2, 0);
        s->geometry_mode = (uint8_t)args;
        break;
    case DRAW_OP_RATE_PALETTE: {
        unsigned table = args >> 1, enabled = args & 1;
        uint64_t packed;
        draw_check_record(h, table < DM_M4_RATE_TABLES, enabled ? sizeof(packed) : 0);
        if (!enabled) {
            s->rate_palette_mask &= ~(1u << table);
            break;
        }
        memcpy(&packed, src, sizeof(packed));
        for (unsigned i = 0; i < DM_M4_RATE_ENTRIES; ++i, packed >>= 4) {
            uint32_t rate = (uint32_t)packed & 15;
            if (rate > DM_M4_RATE_4X4) dm_fail("nvnQueueSubmitCommands: invalid recorded shading rate");
            s->rate_palette[table][i] = rate;
        }
        s->rate_palette_mask |= 1u << table;
        break;
    }
    case DRAW_OP_RATE_IMAGE:
        draw_check_record(h, args <= 1, args ? sizeof(s->rate_image) : 0);
        if (!args) {
            s->rate_image.pool = NULL;
            break;
        }
        memcpy(&s->rate_image, src, sizeof(s->rate_image));
        if (!s->rate_image.pool) dm_fail("nvnQueueSubmitCommands: invalid recorded shading rate image");
        break;
    case DRAW_OP_RATE_PALETTE_MODE:
        draw_check_record(h, args <= DM_M4_RATE_PALETTE_VIEWPORT, 0);
        s->rate_palette_mode = (uint8_t)args;
        break;
    case DRAW_OP_PRIMITIVE_RESTART:
        draw_check_record(h, args <= 1, sizeof(s->restart_index));
        s->restart_enable = (uint8_t)args;
        memcpy(&s->restart_index, src, sizeof(s->restart_index));
        break;
    case DRAW_OP_SAMPLE_MASK:
        draw_check_record(h, args <= UINT16_MAX, 0);
        s->sample_mask = args;
        break;
    case DRAW_OP_STENCIL: {
        unsigned faces = args & NVN_FACE_FRONT_AND_BACK, field = (args >> 2) & 3;
        draw_check_record(h, faces && field <= DRAW_STENCIL_REF && (args >> 4) <= UINT8_MAX, 0);
        uint8_t value = (uint8_t)(args >> 4);
        for (unsigned i = 0; i < ARRAYSIZE(s->stencil); ++i) {
            if (!(faces & (1u << i))) continue;
            struct dm_m4_draw_stencil *stencil = &s->stencil[i];
            if (field == DRAW_STENCIL_COMPARE_MASK) stencil->compare_mask = value;
            else if (field == DRAW_STENCIL_WRITE_MASK) stencil->write_mask = value;
            else stencil->reference = value;
        }
        break;
    }
#define DRAW_REPLAY_RASTER(op, field) \
    case op: \
        draw_check_record(h, !args, sizeof(s->field)); \
        memcpy(&s->field, src, sizeof(s->field)); \
        break
    DRAW_REPLAY_RASTER(DRAW_OP_COLOR, color);
    DRAW_REPLAY_RASTER(DRAW_OP_BLEND_COLOR, blend_color);
    DRAW_REPLAY_RASTER(DRAW_OP_CHANNEL_MASK, channel_mask);
    DRAW_REPLAY_RASTER(DRAW_OP_DEPTH_STENCIL, depth_stencil);
    DRAW_REPLAY_RASTER(DRAW_OP_POLYGON, polygon);
    DRAW_REPLAY_RASTER(DRAW_OP_POLYGON_OFFSET, polygon_offset);
    DRAW_REPLAY_RASTER(DRAW_OP_MULTISAMPLE, multisample);
    DRAW_REPLAY_RASTER(DRAW_OP_LINE_WIDTH, line_width);
    DRAW_REPLAY_RASTER(DRAW_OP_POINT_SIZE, point_size);
#undef DRAW_REPLAY_RASTER
    }
    return 1;
}

struct draw_resolve_context {
    const struct nvn_device *device;
    const struct dm_m4_draw_state *state;
};

static int draw_resolve_gmem(const void *context, dm_m4_gmem_accept accept, void *userdata) {
    const struct draw_resolve_context *ctx = context;
    return visit_live_pools(ctx->device, accept, userdata);
}

static void *draw_resolve_buffer(const void *context, NVNbufferAddress address, size_t size, int write) {
    (void)context;
    void *memory = (void *)(uintptr_t)address;
    return size && dm_guest_range(memory, size, write) ? memory : NULL;
}

static void *draw_resolve_storage(const void *context, const struct dm_m4_draw_texture *t,
                                   nvn_texlayout *layout, int write) {
    const struct draw_resolve_context *ctx = context;
    const struct pool *p = t->pool;
    if (!dm_guest_range(p, sizeof(*p), 0) || p->magic != HEADLESS_NVN_POOL || p->device != ctx->device ||
        !texture_layout(&t->desc, layout) || !draw_view_fits(t, layout)) return NULL;
    return pool_storage(p, t->offset, (size_t)layout->size, layout->align, write);
}

// Resolution and GPU submission borrow the same read-locked descriptor sets.
// Keep pool_alias_lock outermost, as pool finalization uses the same order.
static const struct descriptor_table *draw_descriptor_pool(const struct draw_resolve_context *ctx, int sampler) {
    const struct dm_m4_draw_pool pool = sampler ? ctx->state->sampler_pool : ctx->state->texture_pool;
    for (const struct descriptor_table *t = descriptor_tables; t; t = t->next) {
        if (t->sampler == sampler && t->begin == pool.address && (unsigned)t->count == pool.count &&
            dm_guest_range(t->backing, sizeof(*t->backing), 0) && t->backing->magic == HEADLESS_NVN_POOL &&
            t->backing->device == ctx->device && dm_guest_range((const void *)t->begin, t->end - t->begin, 0))
            return t;
    }
    return NULL;
}

static uint64_t draw_resolve_pool_generation(const void *context, int sampler) {
    const struct descriptor_table *t = draw_descriptor_pool(context, sampler);
    return t ? t->generation : 0;
}

static const struct descriptor_table *draw_descriptor_find(const struct draw_resolve_context *ctx,
                                                             int sampler, unsigned id) {
    const struct descriptor_table *t = draw_descriptor_pool(ctx, sampler);
    return t && id < (unsigned)t->count && (id >= (unsigned)t->reserved || (sampler && !id)) ? t : NULL;
}

static int draw_resolve_texture(const void *context, unsigned id, struct dm_m4_draw_texture *out) {
    const struct descriptor_table *table = draw_descriptor_find(context, 0, id);
    const struct texture_binding *entry = table ? table->textures[id - (unsigned)table->reserved] : NULL;
    int found = entry != NULL;
    if (found) *out = draw_texture_snapshot(&entry->texture, &entry->view);
    return found;
}

static int draw_resolve_sampler(const void *context, unsigned id, struct dm_m4_draw_sampler *out) {
    const struct descriptor_table *table = draw_descriptor_find(context, 1, id);
    if (table && !id) {
        // NVN seeds the reserved sampler 0 on pool initialization. It is not
        // the default API sampler object (its address and LOD modes differ).
        const uint32_t edge = NVN_WRAP_MODE_CLAMP_TO_EDGE;
        *out = (struct dm_m4_draw_sampler){.lod_max = 15.f, .aniso = 1.f,
            .modes = edge | (edge << 4) | (edge << 8) | (NVN_MIN_FILTER_LINEAR << 16) |
                     (NVN_MAG_FILTER_LINEAR << 20) | (NVN_COMPARE_FUNC_LESS << 26)};
        return 1;
    }
    const struct sampler_slot *slot = table ? &table->slots[id - (unsigned)table->reserved] : NULL;
    int found = slot && slot->populated;
    if (found) memcpy(out, &slot->state, sizeof(*out));
    return found;
}

static int draw_replay_indexed(const struct headless_queue *q, struct dm_m4_draw_state **state,
                              struct command_header h, const uint8_t *src) {
    struct draw_instanced extended = {.instances = 1};
    const int arrays = h.op == COMMAND_DRAW_ARRAYS_INSTANCED;
    size_t size = h.op == COMMAND_DRAW_ELEMENTS ? sizeof(extended.indexed) : sizeof(extended);
    draw_check_record(h, 1, size - sizeof(h));
    memcpy(&extended, src, size);
    struct draw_elements record = extended.indexed;
    unsigned primitive = record.params & 0xff, index_type = (record.params >> 8) & 3;
    if (primitive > NVN_DRAW_PRIMITIVE_PATCHES || index_type > NVN_INDEX_TYPE_UNSIGNED_INT ||
        record.params >> 12 || record.count > INT_MAX)
        dm_fail("nvnQueueSubmitCommands: invalid indexed draw record");
    if (!record.count || !extended.instances) return 0;
    unsigned stride = 1u << index_type;
    if (!arrays && ((record.index_buffer & (stride - 1)) ||
        !draw_resolve_buffer(NULL, record.index_buffer, (size_t)record.count * stride, 0)))
        dm_fail("nvnQueueSubmitCommands: invalid indexed draw index span");
    if (!*state) *state = draw_queue_state(q);
    struct dm_m4_indexed_draw draw = {
        .primitive = (NVNdrawPrimitive)primitive, .index_type = (NVNindexType)index_type,
        .arrays = arrays,
        .count = record.count, .indices = record.index_buffer, .guest_device = q->device,
        .instances = extended.instances, .first_instance = extended.first_instance, .base_vertex = extended.base_vertex,
        .raster = {(NVNwindowOriginMode)((record.params >> 10) & 1),
                   (NVNwindowOriginMode)q->origin, (NVNdepthMode)((record.params >> 11) & 1)},
    };
    struct draw_resolve_context ctx = {q->device, *state};
    const struct dm_m4_draw_resolver resolve = {&ctx, draw_resolve_buffer, draw_resolve_storage,
                                                draw_resolve_texture, draw_resolve_sampler, draw_resolve_gmem,
                                                draw_resolve_pool_generation};
    struct draw_queue_state *entry = CONTAINING_RECORD(*state, struct draw_queue_state, state);
    AcquireSRWLockShared(&pool_alias_lock);
    AcquireSRWLockShared(&descriptor_lock);
    const char *error = dm_m4_execute_nvn_indexed(&entry->resources, *state, &draw, &resolve);
    ReleaseSRWLockShared(&descriptor_lock);
    ReleaseSRWLockShared(&pool_alias_lock);
    if (error) {
        uint64_t vs, ps;
        memcpy(&vs, (*state)->shader_keys[NVN_SHADER_STAGE_VERTEX], sizeof(vs));
        memcpy(&ps, (*state)->shader_keys[NVN_SHADER_STAGE_FRAGMENT], sizeof(ps));
        dm_fail("nvnQueueSubmitCommands: indexed draw: %s (VS %016llx, PS %016llx)", error,
                _byteswap_uint64(vs), _byteswap_uint64(ps));
    }
    return 1;
}

static int draw_replay_mesh(const struct headless_queue *q, struct dm_m4_draw_state **state,
                            struct command_header h, const uint8_t *src) {
    struct draw_mesh_tasks record;
    if (h.op == COMMAND_DRAW_MESH_INDIRECT) {
        struct draw_indirect_mesh indirect;
        draw_check_record(h, 1, sizeof(indirect) - sizeof(h));
        memcpy(&indirect, src, sizeof(indirect));
        // Culling dispatches write {taskCount, firstTask}; earlier submissions are fenced.
        const void *args = draw_resolve_buffer(NULL, indirect.arguments, 8, 0);
        if (!args || (indirect.arguments & 3)) dm_fail("nvnQueueSubmitCommands: unmapped indirect mesh arguments");
        uint32_t words[2];
        memcpy(words, args, sizeof(words));
        record = (struct draw_mesh_tasks){h, words[1], words[0], indirect.modes, indirect.pad};
    } else {
        draw_check_record(h, 1, sizeof(record) - sizeof(h));
        memcpy(&record, src, sizeof(record));
    }
    if (record.modes > 3 || record.pad)
        dm_fail("nvnQueueSubmitCommands: invalid mesh task draw record");
    if (!record.count) return 0;
    if (!*state) *state = draw_queue_state(q);
    const struct dm_m4_mesh_draw draw = {
        .first = record.first, .count = record.count, .guest_device = q->device,
        .raster = {(NVNwindowOriginMode)(record.modes & 1),
                   (NVNwindowOriginMode)q->origin, (NVNdepthMode)(record.modes >> 1)},
    };
    struct draw_resolve_context ctx = {q->device, *state};
    const struct dm_m4_draw_resolver resolve = {&ctx, draw_resolve_buffer, draw_resolve_storage,
                                                draw_resolve_texture, draw_resolve_sampler, draw_resolve_gmem,
                                                draw_resolve_pool_generation};
    struct draw_queue_state *entry = CONTAINING_RECORD(*state, struct draw_queue_state, state);
    AcquireSRWLockShared(&pool_alias_lock);
    AcquireSRWLockShared(&descriptor_lock);
    const char *error = dm_m4_execute_nvn_mesh(&entry->resources, *state, &draw, &resolve);
    ReleaseSRWLockShared(&descriptor_lock);
    ReleaseSRWLockShared(&pool_alias_lock);
    if (error) {
        uint64_t ts, ms, ps;
        memcpy(&ts, (*state)->shader_keys[DM_M4_NV_TASK_STAGE], sizeof(ts));
        memcpy(&ms, (*state)->shader_keys[DM_M4_NV_MESH_STAGE], sizeof(ms));
        memcpy(&ps, (*state)->shader_keys[NVN_SHADER_STAGE_FRAGMENT], sizeof(ps));
        dm_fail("nvnQueueSubmitCommands: mesh task draw (%u, %u): %s (TS %016llx, MS %016llx, PS %016llx)",
                record.first, record.count, error, _byteswap_uint64(ts), _byteswap_uint64(ms), _byteswap_uint64(ps));
    }
    static int reported_mesh;
    if (!reported_mesh) {
        reported_mesh = 1;
        printf("[nvn] first mesh task draw completed (first %u, count %u)\n", record.first, record.count);
        fflush(stdout);
    }
    return 1;
}

static int reported_compute;

static void draw_replay_dispatch(const struct headless_queue *q, struct dm_m4_draw_state **state,
                                 struct command_header h, const uint8_t *src) {
    struct dm_m4_dispatch dispatch = {.guest_device = q->device};
    if (h.op == COMMAND_DISPATCH_INDIRECT) {
        NVNbufferAddress address;
        draw_check_record(h, 1, sizeof(address));
        memcpy(&address, src, sizeof(address));
        const void *args = draw_resolve_buffer(NULL, address, sizeof(dispatch.groups), 0);
        if (!args || (address & 3)) dm_fail("nvnQueueSubmitCommands: invalid indirect dispatch argument span");
        memcpy(dispatch.groups, args, sizeof(dispatch.groups));
    } else {
        draw_check_record(h, 1, sizeof(dispatch.groups));
        memcpy(dispatch.groups, src, sizeof(dispatch.groups));
    }
    for (unsigned i = 0; i < ARRAYSIZE(dispatch.groups); ++i)
        if (dispatch.groups[i] > INT_MAX) dm_fail("nvnQueueSubmitCommands: invalid compute group count");
    if (!dispatch.groups[0] || !dispatch.groups[1] || !dispatch.groups[2]) return;
    if (!*state) *state = draw_queue_state(q);
    struct draw_resolve_context ctx = {q->device, *state};
    const struct dm_m4_draw_resolver resolve = {&ctx, draw_resolve_buffer, draw_resolve_storage,
                                                draw_resolve_texture, draw_resolve_sampler, draw_resolve_gmem,
                                                draw_resolve_pool_generation};
    struct draw_queue_state *entry = CONTAINING_RECORD(*state, struct draw_queue_state, state);
    AcquireSRWLockShared(&pool_alias_lock);
    AcquireSRWLockShared(&descriptor_lock);
    const char *error = dm_m4_execute_nvn_compute(&entry->resources, *state, &dispatch, &resolve);
    ReleaseSRWLockShared(&descriptor_lock);
    ReleaseSRWLockShared(&pool_alias_lock);
    if (error) {
        const uint8_t *key = (*state)->shader_keys[NVN_SHADER_STAGE_COMPUTE];
        dm_fail("nvnQueueSubmitCommands: dispatch %02x%02x%02x%02x%02x%02x%02x%02x (%u,%u,%u): %s",
                key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
                dispatch.groups[0], dispatch.groups[1], dispatch.groups[2], error);
    }
    if (!reported_compute) {
        reported_compute = 1;
        printf("[nvn] first compute dispatch completed (%u,%u,%u workgroups)\n",
               dispatch.groups[0], dispatch.groups[1], dispatch.groups[2]);
        fflush(stdout);
    }
}
