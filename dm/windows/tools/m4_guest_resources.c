#include "m4_guest_resources.h"
#include "m4_replay.h"
#include "../../gfx/tex/nvn_deswizzle.h"
#include "m4_phase.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "m4_bytes_equal.h"

struct dm_m4_resource_chunk {
    struct dm_m4_resource_chunk *next;
    size_t used, capacity;
};

struct texture_view {
    nvn_texdesc desc;
    void *storage;
    size_t storage_size;
    size_t view_offset, view_stride, view_layer_bytes;
    unsigned level, levels, layer, layers, slice;
    uint32_t width, height, depth; // depth is nonzero for a volume, not for an array
    size_t pitch;
    int format, bpb, bw, bh;
    VkComponentMapping components;
};

enum writeback_target { WRITEBACK_STORAGE, WRITEBACK_DEPTH_STENCIL, WRITEBACK_COLOR0 };

struct attachment_layout {
    uint32_t width, height, row_bytes, gobs;
    unsigned tile_height, gob_order;
    int format;
};

struct dm_m4_retained_attachment {
    struct attachment_layout layout;
    size_t capacity, snapshot_size;
    uint64_t token;
    int reusable;
    // Tightly packed linear pixels start after the header. Small attachments
    // also have an encoded snapshot at the end; streams retain only pixels.
};

// Share an aligned allocation prefix so idle stream scratch can become a
// retained attachment without allocating or moving another set of pixels.
enum {
    RESOURCE_ALIGN = 16,
    RESOURCE_HEADER = (sizeof(struct dm_m4_retained_attachment) + RESOURCE_ALIGN - 1) & ~(RESOURCE_ALIGN - 1),
};
static_assert(RESOURCE_HEADER >= sizeof(struct dm_m4_resource_chunk), "shared resource allocation header");

enum snapshot_match { SNAPSHOT_UNCHECKED, SNAPSHOT_MATCH, SNAPSHOT_DIFFERS };

struct attachment_candidate {
    struct dm_m4_retained_attachment *entry;
    size_t encoded_size;
    uint64_t output_token;
    enum snapshot_match match;
    uint32_t stream_gobs, stream_block_rows;
};

struct dm_m4_resource_writeback {
    struct dm_m4_resource_writeback *next;
    struct texture_view view;
    void *pixels;
    size_t row_pitch;
    const uint8_t *depth, *stencil;
    unsigned target;
    struct attachment_candidate candidate;
};

enum range_kind { RANGE_OTHER, RANGE_STORAGE_BUFFER, RANGE_ATTACHMENT, RANGE_SAMPLED };
struct guest_range {
    struct guest_range *next;
    struct guest_range *next_write;
    uintptr_t at;
    size_t size, stride, layer_span;
    int write;
    enum range_kind kind;
};

struct staged_image {
    unsigned id, kind;
    struct dm_m4_draw_sampled_image image;
    struct dm_m4_draw_sample_upload upload;
    struct dm_m4_guest_resources *workspace;
    struct texture_view view;
};

struct bindless_slot {
    uint32_t id;
    union {
        struct dm_m4_draw_texture texture;
        struct dm_m4_draw_sampler sampler;
    };
};

struct dm_m4_bindless_pool {
    uint64_t generation;
    uint32_t limit, count, capacity;
    struct bindless_slot *slots;
};

struct preparation {
    struct dm_m4_guest_resources *workspace;
    const struct dm_m4_draw_state *state;
    const struct dm_m4_draw_resolver *resolve;
    struct guest_range *ranges;
    struct guest_range *writes;
    struct staged_image **images;
    size_t image_count, image_capacity;
    uint32_t first_instance;
    const uint32_t *groups; // non-NULL while preparing a compute dispatch
    uint32_t gmem;
};

static const char allocation_failed[] = "NVN draw resources: out of staging memory";

static void *take(struct dm_m4_guest_resources *workspace, size_t size) {
    // Never move live storage: bindings and the backend borrow its addresses.
    if (!size || size > SIZE_MAX - (RESOURCE_ALIGN - 1)) return NULL;
    size = (size + RESOURCE_ALIGN - 1) & ~(RESOURCE_ALIGN - 1);
    struct dm_m4_resource_chunk **link = &workspace->chunks;
    struct dm_m4_resource_chunk **unused = NULL;
    for (; *link; link = &(*link)->next) {
        struct dm_m4_resource_chunk *chunk = *link;
        if (size <= chunk->capacity - chunk->used) {
            void *out = (uint8_t *)chunk + RESOURCE_HEADER + chunk->used;
            chunk->used += size;
            return out;
        }
        if (!chunk->used && (!unused || chunk->capacity > (*unused)->capacity))
            unused = link;
    }
    const size_t capacity = size < 4096 ? 4096 : size;
    if (capacity > SIZE_MAX - RESOURCE_HEADER) return NULL;
    struct dm_m4_resource_chunk *chunk = malloc(RESOURCE_HEADER + capacity);
    if (!chunk) return NULL;
    if (unused) {
        // Replace an idle undersized block rather than retaining it or copying stale bytes.
        chunk->next = (*unused)->next;
        free(*unused);
        link = unused;
    } else {
        chunk->next = NULL;
    }
    chunk->used = size;
    chunk->capacity = capacity;
    *link = chunk;
    return (uint8_t *)chunk + RESOURCE_HEADER;
}

// Selected mips of an array and selected z planes of a volume may each form
// repeated spans separated by untouched bytes.
static int overlaps(const struct guest_range *r, uintptr_t at, size_t size) {
    if (at >= r->at + r->size || r->at >= at + size) return 0;
    if (!r->stride || at <= r->at) return 1;
    const size_t offset = (at - r->at) % r->stride;
    return offset < r->layer_span || size > r->stride - offset;
}

static int ranges_overlap(const struct guest_range *a, const struct guest_range *b) {
    if (a->at >= b->at + b->size || b->at >= a->at + a->size) return 0;
    if (!a->stride) return overlaps(b, a->at, a->size);
    if (!b->stride) return overlaps(a, b->at, b->size);
    if (a->at > b->at) { const struct guest_range *tmp = a; a = b; b = tmp; }
    if (a->stride == b->stride) return overlaps(a, b->at, b->layer_span);
    for (size_t offset = 0;; offset += b->stride) {
        if (overlaps(a, b->at + offset, b->layer_span)) return 1;
        if (b->size - offset <= b->stride) return 0;
    }
}

static const char *track_range(struct preparation *p, const void *bytes, size_t size,
                               size_t stride, size_t layer_span, int write, enum range_kind kind) {
    const uintptr_t at = (uintptr_t)bytes;
    if (!at || !size || size > UINTPTR_MAX - at) return "NVN draw resources: invalid live memory span";
    const struct guest_range current = {.next = p->ranges, .at = at, .size = size,
        .stride = stride, .layer_span = layer_span, .write = write, .kind = kind};
    struct guest_range *shared = NULL;
    const int storage = kind == RANGE_STORAGE_BUFFER, scan_all = write || storage;
    for (struct guest_range *r = scan_all ? p->ranges : p->writes; r; r = scan_all ? r->next : r->next_write) {
        if (storage && r->kind == RANGE_STORAGE_BUFFER && at == r->at && size == r->size)
            shared = r; // the backend shares identical SSBO spans
        else if (p->gmem && !layer_span && !r->layer_span)
            continue; // the backend shares live raw/descriptor buffer bytes
        else if ((kind == RANGE_SAMPLED && r->kind == RANGE_ATTACHMENT) ||
                 (kind == RANGE_ATTACHMENT && r->kind == RANGE_SAMPLED))
            // Samples are uploaded to a different VkImage before rendering.
            // They see the pre-draw contents of every mip/layer, including a
            // chain whose writable mip intersects the sampled view.
            continue;
        else if ((write || r->write) && ranges_overlap(r, &current)) {
            if (write || !p->gmem) {
                static unsigned reports;
                if (reports < 8) {
                    ++reports;
                    fprintf(stderr, "[nvn] overlapping %s span %p+%#zx (stride %#zx, occupied %#zx) with %s %p+%#zx "
                            "(stride %#zx, occupied %#zx)\n", write ? "write" : "read", bytes, size, stride, layer_span,
                            r->write ? "write" : "read", (void *)r->at, r->size, r->stride, r->layer_span);
                }
            }
            return "NVN draw resources: overlapping writable bindings have no shared Vulkan storage";
        }
    }
    if (shared) {
        if (write && !shared->write) {
            shared->write = 1;
            shared->next_write = p->writes;
            p->writes = shared;
        }
        return NULL;
    }
    struct guest_range *range = take(p->workspace, sizeof(*range));
    if (!range) return allocation_failed;
    *range = current;
    p->ranges = range;
    if (write) {
        range->next_write = p->writes;
        p->writes = range;
    }
    return NULL;
}

static const char *track(struct preparation *p, const void *bytes, size_t size, int write, int storage) {
    return track_range(p, bytes, size, 0, 0, write, storage ? RANGE_STORAGE_BUFFER : RANGE_OTHER);
}

static unsigned mip_dim(uint32_t dimension, unsigned level) {
    uint32_t n = dimension >> level;
    return n ? n : 1;
}

static int is_depth(int format) {
    return format >= NVN_FORMAT_STENCIL8 && format <= NVN_FORMAT_DEPTH32F_STENCIL8;
}

// The translated OpTypeImage has an FP32 sampled type (Declarations.cs).
static int integer_format(int format) {
#define INTEGER_VECTOR(N) \
    case NVN_FORMAT_##N##8UI: case NVN_FORMAT_##N##8I: \
    case NVN_FORMAT_##N##16UI: case NVN_FORMAT_##N##16I: \
    case NVN_FORMAT_##N##32UI: case NVN_FORMAT_##N##32I
    switch (format) {
    INTEGER_VECTOR(R): INTEGER_VECTOR(RG): INTEGER_VECTOR(RGB): INTEGER_VECTOR(RGBA):
    case NVN_FORMAT_RGB10A2UI: case NVN_FORMAT_RGB10A2I:
        return 1;
    default:
        return 0;
    }
#undef INTEGER_VECTOR
}

static VkFormat sampled_format(int format) {
    // Sample the depth plane, not the guest stencil or padding bytes.
    switch (format) {
    case NVN_FORMAT_DEPTH16: return VK_FORMAT_D16_UNORM;
    case NVN_FORMAT_DEPTH24: case NVN_FORMAT_DEPTH24_STENCIL8: return VK_FORMAT_X8_D24_UNORM_PACK32;
    case NVN_FORMAT_DEPTH32F: case NVN_FORMAT_DEPTH32F_STENCIL8: return VK_FORMAT_D32_SFLOAT;
    default: break;
    }
    if (is_depth(format)) return VK_FORMAT_UNDEFINED;
    static const VkFormat bc[] = {
        VK_FORMAT_BC1_RGB_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
        VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK,
        VK_FORMAT_BC1_RGB_SRGB_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK,
        VK_FORMAT_BC2_SRGB_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK,
        VK_FORMAT_BC4_UNORM_BLOCK, VK_FORMAT_BC4_SNORM_BLOCK,
        VK_FORMAT_BC5_UNORM_BLOCK, VK_FORMAT_BC5_SNORM_BLOCK,
        VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK,
        VK_FORMAT_BC6H_SFLOAT_BLOCK, VK_FORMAT_BC6H_UFLOAT_BLOCK,
    };
    if (format >= NVN_FORMAT_RGB_DXT1 && format <= NVN_FORMAT_BPTC_UFLOAT)
        return bc[format - NVN_FORMAT_RGB_DXT1];
    // Both format ranges use the same ordered footprints; Vulkan pairs UNORM/sRGB.
    if (format >= NVN_FORMAT_RGBA_ASTC_4x4 && format <= NVN_FORMAT_RGBA_ASTC_12x12_SRGB) {
        const int footprints = NVN_FORMAT_RGBA_ASTC_4x4_SRGB - NVN_FORMAT_RGBA_ASTC_4x4;
        const int index = format - NVN_FORMAT_RGBA_ASTC_4x4;
        return (VkFormat)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * (index % footprints) + index / footprints);
    }
    switch (format) {
    case NVN_FORMAT_RGBX8: return VK_FORMAT_R8G8B8A8_UNORM;
    case NVN_FORMAT_BGRX8: return VK_FORMAT_B8G8R8A8_UNORM;
    case NVN_FORMAT_RGBX8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case NVN_FORMAT_BGRX8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    default: return dm_m4_nvn_attachment_format((NVNformat)format);
    }
}

enum texture_view_use {
    TEXTURE_SINGLE_PLANE, TEXTURE_ATTACHMENT,
    TEXTURE_STORAGE_3D,
    TEXTURE_SAMPLED_2D, TEXTURE_SAMPLED_2D_ARRAY, TEXTURE_SAMPLED_3D, TEXTURE_SAMPLED_CUBE,
};

static const char *get_texture_view(struct preparation *p, const struct dm_m4_draw_texture *texture,
                                    enum texture_view_use use, int write, struct texture_view *out) {
    const nvn_texdesc *d = &texture->desc;
    const struct dm_m4_texture_view *v = &texture->view;
    const unsigned known = DM_M4_VIEW_LEVELS | DM_M4_VIEW_LAYERS | DM_M4_VIEW_FORMAT |
                           DM_M4_VIEW_TARGET | DM_M4_VIEW_SWIZZLE | DM_M4_VIEW_DEPTH_STENCIL;
    if (!texture->pool) return "NVN draw resources: texture has no backing pool";
    if (!p->resolve->texture_storage) return "NVN draw resources: missing texture storage resolver";
    if (v->set & ~known) return "NVN draw resources: unknown texture view field";
    if (d->samples < 0 || d->samples > 1)
        return "NVN draw resources: multisampled texture or attachment is unsupported";
    if (d->flags & NVN_TEXTURE_FLAGS_SPARSE)
        return "NVN draw resources: sparse texture storage is unsupported";
    if (!d->width || !d->height || d->width > 0x10000 || d->height > 0x10000 ||
        d->depth > 0x10000 || !d->levels || d->levels > 17 || d->pkg_layout > 0x1000 || d->gob_order > 1)
        return "NVN draw resources: invalid texture extent or layout";
    const int sampled = use >= TEXTURE_SAMPLED_2D;
    const int volume = use == TEXTURE_SAMPLED_3D || use == TEXTURE_STORAGE_3D;
    const int cube = use == TEXTURE_SAMPLED_CUBE;
    const int format = v->set & DM_M4_VIEW_FORMAT ? v->format : d->format;
    int sw, sh, bw, bh;
    const int bpb = nvn_fmt_bpb(format, &bw, &bh);
    const int storage_bpb = nvn_fmt_bpb(d->format, &sw, &sh);
    // NVN can attach the raw blocks of a BC texture through a texel-sized
    // color view. The staging image uses the view format, not the BC format.
    const int raw_blocks = use == TEXTURE_ATTACHMENT && d->format >= NVN_FORMAT_RGB_DXT1 &&
                           d->format <= NVN_FORMAT_BPTC_UFLOAT && bw == 1 && bh == 1 && !is_depth(format);
    const int volume_plane = use == TEXTURE_ATTACHMENT && d->target == NVN_TEXTURE_TARGET_3D;
    if (volume ? (d->target != NVN_TEXTURE_TARGET_3D || !d->depth) :
        (!volume_plane && d->target != NVN_TEXTURE_TARGET_2D && d->target != NVN_TEXTURE_TARGET_2D_ARRAY &&
         d->target != NVN_TEXTURE_TARGET_CUBEMAP && d->target != NVN_TEXTURE_TARGET_CUBEMAP_ARRAY)) {
        fprintf(stderr, "[nvn] texture view: use %u target %u view %u fields %#x format %#x/%#x "
                        "%ux%ux%u level %u+%u layer %u+%u\n", (unsigned)use, (unsigned)d->target, (unsigned)v->target,
                (unsigned)v->set, (unsigned)d->format, (unsigned)format, (unsigned)d->width, (unsigned)d->height,
                (unsigned)d->depth, (unsigned)v->base_level, (unsigned)v->levels, (unsigned)v->base_layer, (unsigned)v->layers);
        return "NVN draw resources: unsupported texture backing target or depth";
    }
    const int target = v->set & DM_M4_VIEW_TARGET ? v->target : d->target;
    if (volume ? target != NVN_TEXTURE_TARGET_3D :
        cube ? (target != NVN_TEXTURE_TARGET_CUBEMAP && target != NVN_TEXTURE_TARGET_CUBEMAP_ARRAY) :
        (target != NVN_TEXTURE_TARGET_2D && (sampled ? target != NVN_TEXTURE_TARGET_2D_ARRAY : target != d->target)))
        return "NVN draw resources: unsupported texture view target";

    const unsigned level = v->set & DM_M4_VIEW_LEVELS ? v->base_level : 0;
    if (level >= d->levels) return "NVN draw resources: invalid texture view mip range";
    const unsigned requested = (v->set & DM_M4_VIEW_LEVELS) && v->levels ? v->levels : d->levels - level;
    if (requested > d->levels - level) return "NVN draw resources: invalid texture view mip range";
    const unsigned levels = sampled ? requested : 1;
    const unsigned selected = v->set & DM_M4_VIEW_LAYERS ? v->base_layer : 0;
    unsigned layer = volume_plane ? 0 : selected;
    const unsigned slice = volume_plane ? selected : 0;
    unsigned requested_layers = v->set & DM_M4_VIEW_LAYERS ? v->layers : 0;
    if (cube && requested_layers % 6u) {
        // NVN cube views may express the range in cubes instead of faces.
        layer *= 6u;
        requested_layers *= 6u;
    }
    if (volume_plane && (!d->depth || slice >= mip_dim(d->depth, level) ||
                         ((v->set & DM_M4_VIEW_LAYERS) && v->layers > 1)))
        return "NVN draw resources: volume attachment must select one valid z slice";

    const uint32_t width = mip_dim(d->width, level), height = mip_dim(d->height, level);
    const uint32_t depth = volume ? mip_dim(d->depth, level) : 0;
    const uint32_t largest_xy = width > height ? width : height;
    if (!((depth > largest_xy ? depth : largest_xy) >> (levels - 1)))
        return "NVN draw resources: mip range exceeds the Vulkan image mip count";
    if (!bpb || bpb != storage_bpb || (!raw_blocks && (bw != sw || bh != sh)) ||
        is_depth(format) != is_depth(d->format) || (is_depth(format) && format != d->format))
        return "NVN draw resources: texture view format changes storage block/aspect layout";
    const int linear = d->flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET);
    if (linear && (d->levels != 1 || d->target != NVN_TEXTURE_TARGET_2D || raw_blocks))
        return "NVN draw resources: linear texture must be single-level 2D";

    const uint8_t *swizzle = v->set & DM_M4_VIEW_SWIZZLE ? v->swizzle : texture->swizzle;
    const VkComponentSwizzle map[] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE,
        VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    const int alpha_one = format == NVN_FORMAT_RGBX8 || format == NVN_FORMAT_BGRX8 ||
                          format == NVN_FORMAT_RGBX8_SRGB || format == NVN_FORMAT_BGRX8_SRGB;
    // Component selectors affect sampled reads, not framebuffer or storage-image accesses.
    VkComponentSwizzle component[4] = {VK_COMPONENT_SWIZZLE_IDENTITY};
    if (sampled) for (unsigned i = 0; i < 4; ++i) {
        if (swizzle[i] > NVN_TEXTURE_SWIZZLE_A) return "NVN draw resources: invalid texture component swizzle";
        component[i] = alpha_one && swizzle[i] == NVN_TEXTURE_SWIZZLE_A ? VK_COMPONENT_SWIZZLE_ONE : map[swizzle[i]];
    }

    nvn_texlayout layout;
    void *storage = p->resolve->texture_storage(p->resolve->context, texture, &layout, write);
    if (!storage) return "NVN draw resources: texture backing is unmapped or not writable";
    if (!layout.layers || ((volume || volume_plane) && layout.layers != 1) || layer >= layout.layers ||
        !layout.size || layout.size > SIZE_MAX ||
        layout.size > UINTPTR_MAX - (uintptr_t)storage || layout.bpb != (unsigned)bpb)
        return "NVN draw resources: invalid texture backing size or layer";
    unsigned layers = requested_layers ? requested_layers : layout.layers - layer;
    if (layers > layout.layers - layer) return "NVN draw resources: invalid texture view layer range";
    if (!sampled && layers != 1) return "NVN draw resources: texture view must select one layer";
    if (sampled && target == NVN_TEXTURE_TARGET_2D) {
        if ((v->set & DM_M4_VIEW_LAYERS) && v->layers > 1)
            return "NVN draw resources: 2D texture view selects multiple layers";
        layers = 1;
    }
    // Maxwell's non-array instruction reads the first layer of the selected
    // view. An array instruction always keeps all its layers, even just one.
    if (use == TEXTURE_SAMPLED_2D) layers = 1;
    if (cube) {
        if (width != height || layer % 6u || layers < 6u || layers % 6u)
            return "NVN draw resources: cube view requires six square faces in a complete cube";
        layers = 6;
    }
    size_t pitch = 0;
    uint64_t start = 0, end = layout.size, stride = 0, layer_span = layout.size;
    if (linear) {
        if (layout.layers != 1 || layout.size % d->height ||
            layout.size / d->height < (d->width + bw - 1) / bw * (unsigned)bpb)
            return "NVN draw resources: invalid linear texture stride";
        pitch = (size_t)(layout.size / d->height);
    } else {
        if (!layout.layer_size || layout.layers > layout.size / layout.layer_size)
            return "NVN draw resources: invalid texture layer stride";
        const uint64_t first = nvn_tex_level_offset(d, level);
        const uint64_t last = level + levels == d->levels ? layout.layer_size : nvn_tex_level_offset(d, level + levels);
        if (first >= last || last > layout.layer_size) return "NVN draw resources: invalid texture mip offsets";
        start = layer * layout.layer_size + first;
        end = (layer + layers - 1) * layout.layer_size + last;
        layer_span = last - first;
        if (layers > 1 && layer_span != layout.layer_size) stride = layout.layer_size;
        if (volume_plane) {
            unsigned th, td;
            nvn_tex_level_tiles(d, level, &th, &td);
            const uint32_t pitch_bytes = nvn_tex_level_gobs(d, level) * 64u;
            const uint32_t rows = (height + sh - 1) / sh;
            const uint64_t blocks = (uint64_t)(pitch_bytes / 64u) * ((rows + (8u << th) - 1u) / (8u << th));
            const uint64_t plane_at = nvn_bl_offset(0, 0, slice, pitch_bytes, rows, th, td, d->gob_order);
            layer_span = 512ull << th;
            stride = layer_span << td;
            if (!blocks || plane_at >= last - first || layer_span > last - first - plane_at ||
                blocks - 1 > (last - first - plane_at - layer_span) / stride)
                return "NVN draw resources: volume slice exceeds guest mip";
            start = first + plane_at;
            end = start + (blocks - 1) * stride + layer_span;
            if (blocks == 1 || stride == layer_span) stride = 0;
        }
    }
    if (p->workspace) {
        const char *err = track_range(p, (uint8_t *)storage + (size_t)start, (size_t)(end - start),
                                      (size_t)stride, (size_t)layer_span, write,
                                      sampled ? RANGE_SAMPLED : use == TEXTURE_ATTACHMENT ? RANGE_ATTACHMENT : RANGE_OTHER);
        if (err) return err;
    }
    *out = (struct texture_view){.desc = *d, .storage = storage, .storage_size = (size_t)layout.size,
        .level = level, .levels = levels, .layer = layer, .layers = layers, .slice = slice,
        .width = width, .height = height, .depth = depth,
        .view_offset = (size_t)start, .view_stride = (size_t)stride, .view_layer_bytes = (size_t)layer_span,
        .pitch = pitch, .format = format, .bpb = bpb, .bw = sw, .bh = sh,
        .components = {component[0], component[1], component[2], component[3]}};
    return NULL;
}

// Encode one clear texel and its bit mask before walking the scissor. Packed
// depth/stencil and disabled color channels share storage with preserved bits.
static unsigned clear_unorm(float v, unsigned max) {
    return (unsigned)nearbyint((double)fminf(1, fmaxf(0, v)) * max);
}
static unsigned clear_ufloat(float v, unsigned mantissa) {
    if (isnan(v)) return (31u << mantissa) | 1u;
    if (v <= 0) return 0;
    if (isinf(v)) return 31u << mantissa;
    int exp;
    const float fraction = frexpf(v, &exp);
    int biased = exp + 14;
    if (biased >= 31) return 31u << mantissa;
    if (biased < 1) return (unsigned)nearbyintf(ldexpf(v, (int)mantissa + 14));
    return ((unsigned)(biased - 1) << mantissa) + (unsigned)nearbyintf(ldexpf(fraction, (int)mantissa + 1));
}
static const char *clear_texel(int fmt, const struct dm_m4_clear *c, uint8_t value[16], uint8_t mask[16]) {
    float f[4]; int32_t iv[4];
    memcpy(f, c->value, sizeof(f)); memcpy(iv, c->value, sizeof(iv));
    if (c->type == DM_M4_CLEAR_DEPTH_STENCIL) {
        float z = f[0] > 0.0f ? fminf(f[0], 1.0f) : 0.0f;
        unsigned bytes = 0, offset = 0, stencil = 0;
        uint32_t bits = 0;
        switch (fmt) {
        case NVN_FORMAT_STENCIL8: break;
        case NVN_FORMAT_DEPTH16: bytes = 2; bits = clear_unorm(z, UINT16_MAX); break;
        case NVN_FORMAT_DEPTH24: case NVN_FORMAT_DEPTH24_STENCIL8:
            bytes = 3; offset = 1; bits = clear_unorm(z, 0xffffff); break;
        case NVN_FORMAT_DEPTH32F: case NVN_FORMAT_DEPTH32F_STENCIL8:
            bytes = 4; memcpy(&bits, &z, sizeof(bits)); stencil = 4; break;
        default: return "unsupported depth clear format";
        }
        if (c->mask) { memcpy(value + offset, &bits, bytes); memset(mask + offset, 255, bytes); }
        if (fmt == NVN_FORMAT_STENCIL8 || fmt == NVN_FORMAT_DEPTH24_STENCIL8 || fmt == NVN_FORMAT_DEPTH32F_STENCIL8) {
            mask[stencil] = c->stencil_mask;
            value[stencil] = (uint8_t)c->value[1] & c->stencil_mask;
        }
        return NULL;
    }
    // The renderable BGR5 family stores blue in the low bits, green next
    // and red above it. BGR5's high bit has no channel.
    if (fmt == NVN_FORMAT_BGR565 || fmt == NVN_FORMAT_BGR5 || fmt == NVN_FORMAT_BGR5A1) {
        const unsigned widths[] = {5, fmt == NVN_FORMAT_BGR565 ? 6u : 5u, 5, fmt == NVN_FORMAT_BGR5A1 ? 1u : 0u};
        uint16_t v = 0, m = 0;
        for (unsigned i = 0, shift = 0; i < 4; ++i) {
            unsigned j = i < 3 ? 2 - i : i;
            unsigned limit = (1u << widths[i]) - 1;
            if (limit && (c->mask & (1u << j))) {
                v |= (uint16_t)(clear_unorm(f[j], limit) << shift);
                m |= (uint16_t)(limit << shift);
            }
            shift += widths[i];
        }
        memcpy(value, &v, sizeof(v));
        memcpy(mask, &m, sizeof(m));
        return NULL;
    }
    if (fmt == NVN_FORMAT_RGB10A2 || fmt == NVN_FORMAT_RGB10A2UI || fmt == NVN_FORMAT_R11G11B10F) {
        unsigned v = 0, m = 0, shift = 0;
        for (unsigned i = 0; i < (fmt == NVN_FORMAT_R11G11B10F ? 3u : 4u); ++i) {
            unsigned width = fmt == NVN_FORMAT_R11G11B10F ? (i < 2 ? 11u : 10u) : (i < 3 ? 10u : 2u);
            unsigned limit = (1u << width) - 1;
            if (c->mask & (1u << i)) {
                unsigned n = fmt == NVN_FORMAT_R11G11B10F ? clear_ufloat(f[i], width - 5) :
                    fmt == NVN_FORMAT_RGB10A2 ? clear_unorm(f[i], limit) : min(c->value[i], limit);
                v |= n << shift; m |= limit << shift;
            }
            shift += width;
        }
        memcpy(value, &v, 4); memcpy(mask, &m, 4);
        return NULL;
    }
    int bgra = fmt == NVN_FORMAT_BGRA8 || fmt == NVN_FORMAT_BGRA8_SRGB || fmt == NVN_FORMAT_BGRX8 || fmt == NVN_FORMAT_BGRX8_SRGB;
    int srgb = fmt == NVN_FORMAT_RGBA8_SRGB || fmt == NVN_FORMAT_BGRA8_SRGB || fmt == NVN_FORMAT_RGBX8_SRGB || fmt == NVN_FORMAT_BGRX8_SRGB;
    int rgbx = fmt == NVN_FORMAT_RGBX8 || fmt == NVN_FORMAT_BGRX8 || fmt == NVN_FORMAT_RGBX8_SRGB || fmt == NVN_FORMAT_BGRX8_SRGB;
    if (bgra || srgb || rgbx) fmt = NVN_FORMAT_RGBA8;
    if (fmt >= NVN_FORMAT_RGBX8 && fmt <= NVN_FORMAT_RGBX32I) {
        rgbx = 1;
        fmt += NVN_FORMAT_RGBA8 - NVN_FORMAT_RGBX8;
    }
    if (fmt < NVN_FORMAT_R8 || fmt > NVN_FORMAT_RGBA32I) return "unsupported color clear format";
    unsigned count = (unsigned)(fmt - 1) / 12 + 1, kind = (unsigned)(fmt - 1) % 12;
    unsigned bytes = kind < 4 ? 1 : kind < 9 ? 2 : 4;
    if (kind == 4) {
        const float16x4_t converted = vcvt_f16_f32(vld1q_f32(f));
        memcpy(value, &converted, sizeof(converted));
        for (unsigned i = 0; i < count; ++i)
            if ((c->mask & (1u << i)) && !(rgbx && i == 3)) memset(mask + i * 2, 255, 2);
        return NULL;
    }
    for (unsigned i = 0; i < count; ++i) {
        unsigned j = bgra && i < 3 ? 2 - i : i;
        if (!(c->mask & (1u << j)) || (rgbx && j == 3)) continue;
        unsigned n;
        switch (kind) {
        case 0: case 5: {
            float v = f[j];
            if (srgb && j < 3) v = v <= 0.0031308f ? 12.92f * v : 1.055f * powf(v, 1.f / 2.4f) - 0.055f;
            n = clear_unorm(v, bytes == 1 ? UINT8_MAX : UINT16_MAX); break;
        }
        case 1: case 6: n = (unsigned)(int)nearbyintf(fmaxf(-1, fminf(1, f[j])) * (bytes == 1 ? 127 : 32767)); break;
        case 2: case 7: n = min(c->value[j], (bytes == 1 ? 255u : 65535u)); break;
        case 3: case 8: n = (unsigned)max(bytes == 1 ? INT8_MIN : INT16_MIN, min(iv[j], (bytes == 1 ? INT8_MAX : INT16_MAX))); break;
        default: n = c->value[j]; break;
        }
        memcpy(value + i * bytes, &n, bytes);
        memset(mask + i * bytes, 255, bytes);
    }
    return NULL;
}

struct clear_pattern {
    uint8_t value[32], mask[32];
    unsigned bpb;
    int full, zero;
};

// One contiguous span (a linear row segment or a 16-byte piece of a GOB).
static inline void clear_span(uint8_t *dst, unsigned size, const struct clear_pattern *p, unsigned phase) {
    if (p->full) memcpy(dst, p->value + phase, size);
    else for (unsigned i = 0; i < size; ++i) {
        uint8_t mask = p->mask[phase + i];
        if (mask) dst[i] = (uint8_t)((dst[i] & ~mask) | p->value[phase + i]);
    }
}

struct clear_gob {
    unsigned row_bytes, height, th, td, order, z;
};

static uint64_t clear_gob_offset(const struct clear_gob *g, unsigned x, unsigned y) {
    return nvn_bl_offset(x, y, g->z, g->row_bytes, g->height, g->th, g->td, (int)g->order);
}

static void clear_gob_rect(uint8_t *dst, const struct clear_gob *g, const struct clear_pattern *p,
                           unsigned left, unsigned top, unsigned right, unsigned bottom) {
    if (left >= right) return;
    for (unsigned y = top; y < bottom; ++y) {
        for (unsigned x = left, phase = 0; x < right;) {
            unsigned n = min(16u - (x & 15u), right - x);
            clear_span(dst + clear_gob_offset(g, x, y), n, p, phase);
            x += n;
            phase = (phase + n) % p->bpb;
        }
    }
}

static void clear_block_linear(uint8_t *dst, const struct clear_gob *g, const struct clear_pattern *p,
                               unsigned left, unsigned top, unsigned right, unsigned bottom) {
    if (p->zero) {
        const unsigned rows = 8u << g->th;
        const unsigned x0 = (left + 63u) & ~63u, x1 = right & ~63u;
        const unsigned y0 = (top + 7u) & ~7u, y1 = bottom & ~7u;
        if (x0 < x1 && y0 < y1) {
            // A run of GOBs within one column stays in the selected slice.
            // Across columns, only depth-one blocks are contiguous.
            for (unsigned y = y0; y < y1;) {
                const unsigned next = min((y & ~(rows - 1)) + rows, y1);
                const size_t bytes = (size_t)((next - y) >> 3) * 512;
                if (!g->td && next - y == rows) {
                    memset(dst + clear_gob_offset(g, x0, y), 0, bytes * ((x1 - x0) >> 6));
                } else for (unsigned x = x0; x < x1; x += 64)
                    memset(dst + clear_gob_offset(g, x, y), 0, bytes);
                y = next;
            }
            clear_gob_rect(dst, g, p, left, top, right, y0);
            clear_gob_rect(dst, g, p, left, y1, right, bottom);
            clear_gob_rect(dst, g, p, left, y0, x0, y1);
            clear_gob_rect(dst, g, p, x1, y0, right, y1);
            return;
        }
    }
    clear_gob_rect(dst, g, p, left, top, right, bottom);
}

static unsigned clear_level(const struct dm_m4_draw_texture *t) {
    return t->view.set & DM_M4_VIEW_LEVELS ? t->view.base_level : 0;
}

static void clear_clip_target(const struct dm_m4_draw_texture *t, unsigned *width, unsigned *height) {
    if (!t->pool) return;
    unsigned level = clear_level(t);
    if (level >= 32) return;
    const int one_d = t->desc.target == NVN_TEXTURE_TARGET_1D || t->desc.target == NVN_TEXTURE_TARGET_1D_ARRAY;
    *width = min(*width, mip_dim(t->desc.width, level));
    *height = min(*height, one_d ? 1u : mip_dim(t->desc.height, level));
}

static int clear_2d_target(int target) {
    return target == NVN_TEXTURE_TARGET_2D || target == NVN_TEXTURE_TARGET_2D_ARRAY ||
        target == NVN_TEXTURE_TARGET_CUBEMAP || target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY ||
        target == NVN_TEXTURE_TARGET_RECTANGLE;
}

static const char *clear_surface(const struct dm_m4_draw_texture *t, const struct dm_m4_draw_state *state,
                                 const struct dm_m4_draw_scissor *region, const struct dm_m4_draw_resolver *resolve,
                                 const struct dm_m4_clear *clear) {
    const int depth = clear->type == DM_M4_CLEAR_DEPTH_STENCIL;
    if (!t->pool || (!clear->mask && !clear->stencil_mask)) return NULL;
    const nvn_texdesc *d = &t->desc;
    const struct dm_m4_texture_view *v = &t->view;
    const int volume = d->target == NVN_TEXTURE_TARGET_3D;
    const int target = v->set & DM_M4_VIEW_TARGET ? v->target : d->target;
    const unsigned level = clear_level(t);
    if (!d->width || !d->height || d->width > 0x10000 || d->height > 0x10000 ||
        (volume && (!d->depth || d->depth > 0x10000)) ||
        level >= d->levels || d->levels > 17 || ((v->set & DM_M4_VIEW_LEVELS) && v->levels > d->levels - level) ||
        d->pkg_layout > 0x1000 || d->gob_order > 1)
        return "invalid clear texture extent or layout";
    if (d->samples < 0 || d->samples > 1 || (d->flags & NVN_TEXTURE_FLAGS_SPARSE) ||
        (volume ? target != NVN_TEXTURE_TARGET_3D && target != NVN_TEXTURE_TARGET_2D :
                  !clear_2d_target(d->target) || !clear_2d_target(target)))
        return "clear requires a nonsparse single-sample 2D or 3D texture";

    // A framebuffer uses the first mip in each view and clips to the shared extent.
    const unsigned width = mip_dim(d->width, level), height = mip_dim(d->height, level);
    unsigned clip_width = width, clip_height = height;
    if (state) {
        for (unsigned i = 0; i < state->color_count && i < DM_M4_DRAW_TARGETS; ++i)
            if (&state->colors[i] != t) clear_clip_target(&state->colors[i], &clip_width, &clip_height);
        if (!depth) clear_clip_target(&state->depth, &clip_width, &clip_height);
        if (state->scissor_set & 1u) region = &state->scissors[0];
    }
    struct dm_m4_draw_scissor r = region ? *region : (struct dm_m4_draw_scissor){0, 0, (int)clip_width, (int)clip_height};
    const int x0 = max(r.x, 0), y0 = max(r.y, 0);
    const int64_t x1 = min((int64_t)clip_width, (int64_t)r.x + r.width);
    const int64_t y1 = min((int64_t)clip_height, (int64_t)r.y + r.height);
    if (x0 >= x1 || y0 >= y1) return NULL;

    // Swizzle and depth/stencil mode select shader reads, not render writes.
    const int format = v->set & DM_M4_VIEW_FORMAT ? v->format : d->format;
    int bw, bh, sw, sh;
    const int bpb = nvn_fmt_bpb(format, &bw, &bh);
    if (bpb <= 0 || bpb > 16 || bw != 1 || bh != 1 || bpb != nvn_fmt_bpb(d->format, &sw, &sh) ||
        sw != 1 || sh != 1 || depth != is_depth(format) || depth != is_depth(d->format) || (depth && format != d->format))
        return "invalid clear target format";
    struct clear_pattern pattern = {.bpb = (unsigned)bpb, .full = 1, .zero = 1};
    const char *error = clear_texel(format, clear, pattern.value, pattern.mask);
    if (error) return error;
    unsigned active = 0;
    for (int i = 0; i < bpb; ++i) {
        active |= pattern.mask[i];
        pattern.full &= pattern.mask[i] == 255;
        pattern.zero &= pattern.value[i] == 0;
    }
    if (!active) return NULL;
    pattern.zero &= pattern.full;
    for (int i = bpb; i < 32; ++i) {
        pattern.value[i] = pattern.value[i - bpb];
        pattern.mask[i] = pattern.mask[i - bpb];
    }

    if (!resolve->texture_storage) return "missing clear texture storage resolver";
    nvn_texlayout layout;
    uint8_t *storage = resolve->texture_storage(resolve->context, t, &layout, 1);
    if (!storage) return "clear texture backing is unmapped or not writable";
    if (!layout.layer_size || (volume && layout.layers != 1) || layout.layers > layout.size / layout.layer_size ||
        layout.size > UINTPTR_MAX - (uintptr_t)storage || layout.bpb != (unsigned)bpb)
        return "invalid clear texture backing";
    // A volume has one backing layer; a view of its mip selects depth slices.
    const unsigned images = volume ? mip_dim(d->depth, level) : layout.layers;
    const unsigned first = v->set & DM_M4_VIEW_LAYERS ? v->base_layer : 0;
    if (first >= images) return "invalid clear base layer or slice";
    const unsigned count = (v->set & DM_M4_VIEW_LAYERS) && v->layers ? v->layers : images - first;
    if (count > images - first) return "invalid clear layer or slice range";
    const unsigned start = (unsigned)x0 * pattern.bpb, end = (unsigned)x1 * pattern.bpb;
    if (d->flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) {
        const size_t pitch = (size_t)(layout.layer_size / height);
        if (volume || d->levels != 1 || layout.layers != 1 || pitch < width * pattern.bpb || layout.layer_size % height)
            return "invalid linear clear layout";
        if (pattern.zero) {
            if (end - start == pitch)
                memset(storage + (size_t)y0 * pitch, 0, (size_t)(y1 - y0) * pitch);
            else for (int y = y0; y < y1; ++y)
                memset(storage + (size_t)y * pitch + start, 0, end - start);
            return NULL;
        }
        for (int y = y0; y < y1; ++y) {
            uint8_t *row = storage + (size_t)y * pitch;
            for (unsigned x = start, phase = 0; x < end;) {
                unsigned n = min(16u, end - x);
                clear_span(row + x, n, &pattern, phase);
                x += n;
                phase = (phase + n) % pattern.bpb;
            }
        }
        return NULL;
    }

    struct clear_gob g = {.row_bytes = nvn_tex_level_gobs(d, level) * 64u, .height = height, .order = d->gob_order};
    nvn_tex_level_tiles(d, level, &g.th, &g.td);
    const uint64_t offset = nvn_tex_level_offset(d, level);
    const uint64_t limit = level + 1 == d->levels ? layout.layer_size : nvn_tex_level_offset(d, level + 1);
    // Check the highest touched address within the mip before writing any layer.
    g.z = volume ? first + count - 1 : 0;
    if (g.row_bytes < width * pattern.bpb || limit > layout.layer_size || offset >= limit ||
        clear_gob_offset(&g, end - 1, (unsigned)y1 - 1) >= limit - offset)
        return "clear exceeds guest texture subresource";
    const uint64_t stride = volume ? 0 : layout.layer_size;
    for (unsigned i = first; i < first + count; ++i) {
        g.z = volume ? i : 0;
        clear_block_linear(storage + i * stride + offset, &g, &pattern, start, (unsigned)y0, end, (unsigned)y1);
    }
    return NULL;
}

const char *dm_m4_clear_attachment(const struct dm_m4_draw_state *state,
                                   const struct dm_m4_draw_resolver *resolve, const struct dm_m4_clear *clear) {
    const int depth = clear->type == DM_M4_CLEAR_DEPTH_STENCIL;
    if (!depth && clear->index >= DM_M4_DRAW_TARGETS) return "invalid clear target index";
    return clear_surface(depth ? &state->depth : &state->colors[clear->index], state, NULL, resolve, clear);
}

const char *dm_m4_clear_texture(const struct dm_m4_draw_texture *texture, const struct dm_m4_draw_scissor *region,
                                const struct dm_m4_draw_resolver *resolve, const struct dm_m4_clear *clear) {
    return clear_surface(texture, NULL, region, resolve, clear);
}

static const char *mip_planes(struct preparation *p, const struct texture_view *view, unsigned mip,
                              struct dm_m4_draw_pixels *pixels) {
    if (view->pitch) {
        *pixels = (struct dm_m4_draw_pixels){view->storage, view->storage_size, view->pitch};
        return NULL;
    }
    const uint32_t width = mip_dim(view->width, mip), height = mip_dim(view->height, mip);
    const size_t pitch = (width + view->bw - 1) / view->bw * (size_t)view->bpb;
    const size_t rows = (height + view->bh - 1) / view->bh;
    const uint32_t slices = mip_dim(view->depth, mip);
    if (rows > SIZE_MAX / pitch / slices) return "NVN draw resources: texture mip byte count overflows";
    const size_t size = pitch * rows * slices;
    if (view->layers > SIZE_MAX / size) return "NVN draw resources: texture layer byte count overflows";
    uint8_t *data = take(p->workspace, size * view->layers);
    if (!data) return allocation_failed;
    // Use the original NVN layout. One decode emits all z slices of a volume
    // mip; array layers instead use separate full mip chains in guest storage.
    for (unsigned layer = 0; layer < view->layers; ++layer, data += size) {
        const unsigned level = view->level + mip, index = view->layer + layer;
        const uint64_t phase_start = m4_phase_ticks();
        const int error = view->desc.target == NVN_TEXTURE_TARGET_3D && !view->depth ?
            nvn_deswizzle_slice(&view->desc, level, index, view->slice, view->storage, data, pitch) :
            nvn_deswizzle(&view->desc, level, index, view->storage, data, pitch);
        m4_phase_add(M4_PHASE_DESWIZZLE, phase_start);
        m4_phase_bytes(M4_BYTES_DESWIZZLE, size);
        if (error)
            return "NVN draw resources: failed to deswizzle guest texture mip";
        pixels[(size_t)layer * view->levels] = (struct dm_m4_draw_pixels){data, size, pitch};
    }
    return NULL;
}

// Small slots retain both byte representations; oversized streams can use
// the shared budget for just the linear attachment rows.
enum { ATTACHMENT_SLOT_BYTES = 224u * 1024u * 1024u, ATTACHMENT_CACHE_BYTES = 384u * 1024u * 1024u };

static uint64_t next_attachment_token(void) {
    // Different queue workspaces can use the same backend. Never wrap, even
    // after cache eviction or workspace destruction; zero disables GPU reuse.
    static volatile LONG64 serial;
    LONG64 current = InterlockedCompareExchange64(&serial, 0, 0);
    while (current < INT64_MAX) {
        const LONG64 previous = InterlockedCompareExchange64(&serial, current + 1, current);
        if (previous == current) return (uint64_t)(current + 1);
        current = previous;
    }
    return 0;
}

static int attachment_layout_equal(const struct attachment_layout *a, const struct attachment_layout *b) {
    return a->width == b->width && a->height == b->height && a->row_bytes == b->row_bytes &&
           a->gobs == b->gobs && a->tile_height == b->tile_height && a->gob_order == b->gob_order &&
           a->format == b->format;
}

static uint8_t *attachment_data(struct dm_m4_retained_attachment *entry) {
    return (uint8_t *)entry + RESOURCE_HEADER;
}

static uint8_t *attachment_snapshot(struct dm_m4_retained_attachment *entry, size_t size) {
    return attachment_data(entry) + (entry->capacity - size);
}

static struct dm_m4_retained_attachment *retain_attachment(struct dm_m4_guest_resources *workspace,
        struct dm_m4_retained_attachment **slot, size_t capacity, int stream) {
    struct dm_m4_retained_attachment *entry = *slot;
    if (entry && entry->capacity == capacity) return entry;
    const size_t old_size = entry ? RESOURCE_HEADER + entry->capacity : 0;
    const size_t required = RESOURCE_HEADER + capacity;
    if (required > ATTACHMENT_CACHE_BYTES - (workspace->attachment_bytes - old_size)) return NULL;

    workspace->attachment_bytes -= old_size;
    free(entry);
    entry = NULL;
    if (stream) {
        // Only steal an exact-size idle chunk. Other allocations can still
        // borrow the live scratch chunks for the rest of this preparation.
        for (struct dm_m4_resource_chunk **at = &workspace->chunks; *at; at = &(*at)->next) {
            struct dm_m4_resource_chunk *chunk = *at;
            if (chunk->used || chunk->capacity != capacity) continue;
            *at = chunk->next;
            entry = (void *)chunk;
            break;
        }
    }
    if (!entry) entry = malloc(required);
    *slot = entry;
    if (entry) {
        *entry = (struct dm_m4_retained_attachment){.capacity = capacity};
        workspace->attachment_bytes += required;
    }
    return entry;
}

enum attachment_move { ATTACHMENT_LOAD, ATTACHMENT_REFRESH, ATTACHMENT_STORE };

static FORCEINLINE void attachment_gob_pair(uint8_t *dst, uint8x16_t first, uint8x16_t second) {
    const uint8x16x2_t pair = {{first, second}};
    vst1q_u8_x2(dst, pair);
}

static FORCEINLINE uint8x16_t attachment_gob_diff(const uint8_t *dst, uint8x16_t first, uint8x16_t second) {
    const uint8x16x2_t old = vld1q_u8_x2(dst);
    return vorrq_u8(veorq_u8(old.val[0], first), veorq_u8(old.val[1], second));
}

// A 32-byte by four-row quadrant is contiguous in either GOB order. Each
// adjacent pair of vectors belongs to one column of two adjacent linear rows.
static FORCEINLINE int move_attachment_gob_quad(uint8_t *gob, uint8_t *linear, size_t pitch,
                                               size_t x16, size_t y2, enum attachment_move move) {
    uint8_t *r0 = linear, *r1 = r0 + pitch, *r2 = r1 + pitch, *r3 = r2 + pitch;
    if (move != ATTACHMENT_STORE) {
        const uint8x16x2_t a = vld1q_u8_x2(gob), b = vld1q_u8_x2(gob + x16);
        const uint8x16x2_t c = vld1q_u8_x2(gob + y2), d = vld1q_u8_x2(gob + y2 + x16);
        if (move == ATTACHMENT_REFRESH) {
            const uint8x16_t top = vorrq_u8(attachment_gob_diff(r0, a.val[0], b.val[0]),
                                           attachment_gob_diff(r1, a.val[1], b.val[1]));
            const uint8x16_t bottom = vorrq_u8(attachment_gob_diff(r2, c.val[0], d.val[0]),
                                              attachment_gob_diff(r3, c.val[1], d.val[1]));
            // On mismatch the loaded vectors already hold the live bytes.
            if (!vmaxvq_u8(vorrq_u8(top, bottom))) return 0;
        }
        attachment_gob_pair(r0, a.val[0], b.val[0]);
        attachment_gob_pair(r1, a.val[1], b.val[1]);
        attachment_gob_pair(r2, c.val[0], d.val[0]);
        attachment_gob_pair(r3, c.val[1], d.val[1]);
        return move == ATTACHMENT_REFRESH;
    }
    const uint8x16x2_t a = vld1q_u8_x2(r0), b = vld1q_u8_x2(r1);
    const uint8x16x2_t c = vld1q_u8_x2(r2), d = vld1q_u8_x2(r3);
    const uint8x16_t top = vorrq_u8(attachment_gob_diff(gob, a.val[0], b.val[0]),
                                   attachment_gob_diff(gob + x16, a.val[1], b.val[1]));
    const uint8x16_t bottom = vorrq_u8(attachment_gob_diff(gob + y2, c.val[0], d.val[0]),
                                      attachment_gob_diff(gob + y2 + x16, c.val[1], d.val[1]));
    // Reuse the source vectors for a changed quadrant. Equal quadrants never
    // take write ownership or dirty the guest bytes.
    if (!vmaxvq_u8(vorrq_u8(top, bottom))) return 0;
    attachment_gob_pair(gob, a.val[0], b.val[0]);
    attachment_gob_pair(gob + x16, a.val[1], b.val[1]);
    attachment_gob_pair(gob + y2, c.val[0], d.val[0]);
    attachment_gob_pair(gob + y2 + x16, c.val[1], d.val[1]);
    return 0;
}

// Only complete, single-slice GOBs. Walk one eight-row band at a time, then
// skip the other columns when advancing to the next block row.
static FORCEINLINE int move_attachment_gobs(uint8_t *gob, uint8_t *linear, size_t pitch,
                                           uint32_t rows, uint32_t gobs, uint32_t block_rows,
                                           int order, enum attachment_move move) {
    const size_t x16 = (size_t)nvn_bl_offset(16, 0, 0, 64, 8, 0, 0, order);
    const size_t y2 = (size_t)nvn_bl_offset(0, 2, 0, 64, 8, 0, 0, order);
    const size_t block = (size_t)block_rows * 64u, jump = (gobs - 1u) * block + 512u;
    int changed = 0;
    for (uint32_t y = 0; y < rows; y += 8) {
        size_t column = 0;
        uint8_t *row = linear + (size_t)y * pitch;
        for (size_t x = 0; x < pitch; x += 64, column += block) {
            uint8_t *tile = gob + column, *at = row + x;
            changed |= move_attachment_gob_quad(tile,       at,                  pitch, x16, y2, move);
            changed |= move_attachment_gob_quad(tile + 128, at + 4 * pitch,      pitch, x16, y2, move);
            changed |= move_attachment_gob_quad(tile + 256, at + 32,             pitch, x16, y2, move);
            changed |= move_attachment_gob_quad(tile + 384, at + 4 * pitch + 32, pitch, x16, y2, move);
        }
        if (y + 8 < rows) gob += ((y + 8) & (block_rows - 1u)) ? 512u : jump;
    }
    return changed;
}

static const char *attachment_pixels(struct preparation *p, const struct texture_view *view, unsigned target,
                                     struct dm_m4_draw_pixels *pixels, struct attachment_candidate *candidate) {
    if (view->pitch || view->view_stride || view->desc.target == NVN_TEXTURE_TARGET_3D)
        return mip_planes(p, view, 0, pixels);

    uint32_t row_bytes, rows, slices, bpb;
    uint64_t base;
    unsigned th, td;
    if (nvn_deswizzle_geom(&view->desc, view->level, view->layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb) ||
        base != view->view_offset || slices != 1 || td || bpb != (unsigned)view->bpb ||
        row_bytes != (view->width + view->bw - 1) / view->bw * (unsigned)view->bpb ||
        rows != (view->height + view->bh - 1) / view->bh)
        return mip_planes(p, view, 0, pixels);

    const uint32_t gobs = nvn_tex_level_gobs(&view->desc, view->level);
    if (gobs < (row_bytes + 63u) / 64u) return mip_planes(p, view, 0, pixels);
    // Include the tiles (and their padding), not the rest of a mip chain or
    // the alignment gap after the last mip of an array layer.
    const uint32_t block_rows = 8u << th;
    const uint64_t encoded = (uint64_t)gobs * ((rows + block_rows - 1u) / block_rows) * (64u * block_rows);
    const uint64_t linear = (uint64_t)row_bytes * rows;
    const int oversized = RESOURCE_HEADER + encoded + linear > ATTACHMENT_SLOT_BYTES;
    const int stream = oversized && !(row_bytes & 63u) && !(rows & 7u);
    if (encoded > view->view_layer_bytes || (oversized && !stream))
        return mip_planes(p, view, 0, pixels);

    struct dm_m4_guest_resources *workspace = p->workspace;
    const unsigned index = target - WRITEBACK_DEPTH_STENCIL;
    // Smaller attachments interleave with the atlas on the same target slot.
    // Keep their entries separate but charge them to the same cache budget.
    struct dm_m4_retained_attachment **slot = stream ? &workspace->stream_attachments[index] :
        &workspace->attachments[index];
    const size_t snapshot_size = stream ? 0 : (size_t)encoded;
    const size_t capacity = (size_t)linear + snapshot_size;
    // Raw shader writes may alias the live texture during submission. Stream
    // its normal pixels, but neither consume nor publish a proof for this draw.
    struct dm_m4_retained_attachment *entry = (p->gmem & 2u) ? NULL :
        retain_attachment(workspace, slot, capacity, stream);
    struct dm_m4_retained_attachment *staging = *slot;
    const int reusable = staging && staging->reusable;
    // Submission can mutate these borrowed pixels and then fail. Only a
    // successful writeback may republish a token for the staging entry.
    if (staging) staging->reusable = 0;
    if (!entry && !stream) return mip_planes(p, view, 0, pixels);

    uint8_t *data = staging && staging->capacity >= linear ?
        attachment_data(staging) : take(workspace, (size_t)linear);
    if (!data) return allocation_failed;

    const struct attachment_layout layout = {.width = view->width, .height = view->height, .row_bytes = row_bytes,
        .gobs = gobs, .tile_height = th, .gob_order = view->desc.gob_order, .format = view->format};
    uint8_t *guest = (uint8_t *)view->storage + view->view_offset;
    const int compare = entry && reusable && entry->snapshot_size == snapshot_size &&
        attachment_layout_equal(&layout, &entry->layout);
    enum snapshot_match match = SNAPSHOT_DIFFERS;
    if (stream) {
        const int order = nvn_gob_order(&view->desc);
        if (compare) {
            if (!move_attachment_gobs(guest, data, row_bytes, rows, gobs, block_rows, order, ATTACHMENT_REFRESH))
                match = SNAPSHOT_MATCH;
        } else {
            move_attachment_gobs(guest, data, row_bytes, rows, gobs, block_rows, order, ATTACHMENT_LOAD);
        }
        candidate->stream_gobs = gobs;
        candidate->stream_block_rows = block_rows;
    } else {
        if (entry->snapshot_size != snapshot_size) entry->snapshot_size = 0;
        if (entry->snapshot_size) {
            match = !compare ? SNAPSHOT_UNCHECKED :
                m4_bytes_equal(attachment_snapshot(entry, snapshot_size), guest, snapshot_size) ?
                SNAPSHOT_MATCH : SNAPSHOT_DIFFERS;
        }
        if (match != SNAPSHOT_MATCH) {
            const uint64_t phase_start = m4_phase_ticks();
            const int failed = nvn_deswizzle(&view->desc, view->level, view->layer, view->storage, data, row_bytes);
            m4_phase_add(M4_PHASE_DESWIZZLE, phase_start);
            m4_phase_bytes(M4_BYTES_DESWIZZLE, (uint64_t)rows * row_bytes);
            if (failed)
                return "NVN draw resources: failed to deswizzle guest texture mip";
        }
    }
    if (entry) {
        entry->layout = layout;
        candidate->entry = entry;
        candidate->encoded_size = snapshot_size;
        candidate->match = match;
    }
    *pixels = (struct dm_m4_draw_pixels){data, (size_t)linear, row_bytes};
    return NULL;
}

static uint32_t rate_palette_entry(const struct dm_m4_draw_state *state, unsigned table, unsigned entry) {
    return state->rate_palette_mask & (1u << table) ? state->rate_palette[table][entry] : DM_M4_RATE_1X1;
}

static const char *shading_rate(struct preparation *p, struct dm_m4_draw_info *info) {
    static_assert((unsigned)DM_M4_RATE_TABLES == (unsigned)DM_M4_RATE_MAX_PALETTES &&
                  (unsigned)DM_M4_RATE_ENTRIES == (unsigned)DM_M4_RATE_PALETTE_SIZE, "guest and Vulkan palette sizes");
    const struct dm_m4_draw_state *state = p->state;
    const int bound = state->rate_image.pool != NULL;
    info->rate_image = NULL;
    info->pipeline.viewport_count = 1;
    if (!bound && !state->rate_palette_mask) return NULL;
    // NVN's rate nibbles match NV shading-rate palette entries. An unbound
    // image selects entry zero, so other entries cannot require more viewports.
    const unsigned relevant = bound ? DM_M4_RATE_ENTRIES : 1;
    const unsigned last_stage = info->ms.shader ? 1u << DM_M4_NV_MESH_STAGE : NVN_SHADER_STAGE_VERTEX_BIT;
    unsigned count = 1;
    if (state->rate_palette_mask && state->rate_palette_mode == DM_M4_RATE_PALETTE_PRIMITIVE &&
        (state->rate_index_outputs & last_stage)) {
        for (unsigned v = 1; v < DM_M4_RATE_TABLES && count == 1; ++v) {
            for (unsigned i = 0; i < relevant; ++i) {
                if (rate_palette_entry(state, v, i) != rate_palette_entry(state, 0, i)) {
                    count = DM_M4_RATE_TABLES;
                    break;
                }
            }
        }
    }
    if (count > 1 && (info->ms.shader || !info->vs.shader || !info->vs.shader->vprs_viewport))
        return "NVN draw resources: per-primitive shading-rate palette selection";
    if (count == 1 && !bound && rate_palette_entry(state, 0, 0) == DM_M4_RATE_1X1) return NULL;

    struct dm_m4_rate_image *out = take(p->workspace, sizeof *out + (count - 1u) * sizeof out->palette);
    if (!out) return allocation_failed;
    VkShadingRatePaletteEntryNV (*additional)[DM_M4_RATE_PALETTE_SIZE] = count > 1 ? (void *)(out + 1) : NULL;
    out->additional_palettes = (const VkShadingRatePaletteEntryNV (*)[DM_M4_RATE_PALETTE_SIZE])additional;
    for (unsigned v = 0; v < count; ++v) {
        VkShadingRatePaletteEntryNV *palette = v ? additional[v - 1u] : out->palette;
        for (unsigned i = 0; i < DM_M4_RATE_PALETTE_SIZE; ++i) {
            const uint32_t entry = rate_palette_entry(state, v, i);
            if (entry > VK_SHADING_RATE_PALETTE_ENTRY_1_INVOCATION_PER_4X4_PIXELS_NV)
                return "NVN draw resources: invalid shading-rate palette entry";
            palette[i] = (VkShadingRatePaletteEntryNV)entry;
        }
    }
    if (bound) {
        struct texture_view view;
        const char *err = get_texture_view(p, &state->rate_image, TEXTURE_SINGLE_PLANE, 0, &view);
        if (err) return err;
        if (view.bpb != 1 || view.bw != 1 || view.bh != 1 || is_depth(view.format))
            return "NVN draw resources: shading-rate surface must contain one byte per texel";
        out->width = view.width;
        out->height = view.height;
        if ((err = mip_planes(p, &view, 0, &out->texels))) return err;
    } else {
        out->width = out->height = 0;
        out->texels = (struct dm_m4_draw_pixels){0};
    }
    info->rate_image = out;
    info->pipeline.viewport_count = count;
    return NULL;
}

static const char *attachment(struct preparation *p, const struct dm_m4_draw_texture *texture,
                              unsigned target, struct dm_m4_draw_attachment *out) {
    struct texture_view view;
    const char *err = get_texture_view(p, texture, TEXTURE_ATTACHMENT, 1, &view);
    if (err) return err;
    VkFormat format = dm_m4_nvn_attachment_format((NVNformat)view.format);
    if (format == VK_FORMAT_UNDEFINED || (target == WRITEBACK_DEPTH_STENCIL) != is_depth(view.format))
        return "NVN draw resources: unsupported color or depth render attachment format";
    struct dm_m4_draw_pixels pixels;
    struct attachment_candidate candidate = {0};
    if ((err = attachment_pixels(p, &view, target, &pixels, &candidate))) return err;
    *out = (struct dm_m4_draw_attachment){.format = format,
        .width = (view.width + view.bw - 1) / view.bw, .height = (view.height + view.bh - 1) / view.bh,
        .pixels = {(void *)pixels.data, pixels.size, pixels.row_pitch}};
    if (candidate.entry) {
        if (candidate.match == SNAPSHOT_MATCH) out->input_token = candidate.entry->token;
        out->output_token = candidate.output_token = next_attachment_token();
    }

    const int packed = format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    const int depth24 = format == VK_FORMAT_X8_D24_UNORM_PACK32 || format == VK_FORMAT_D24_UNORM_S8_UINT;
    if (view.pitch && !packed && !depth24) return NULL; // backend copies rows directly; padding is untouched
    struct dm_m4_resource_writeback *writeback = take(p->workspace, sizeof(*writeback));
    if (!writeback) return allocation_failed;
    *writeback = (struct dm_m4_resource_writeback){.next = p->workspace->writeback, .view = view,
        .pixels = (void *)pixels.data, .row_pitch = pixels.row_pitch, .target = target,
        .candidate = candidate};
    if (packed || depth24) {
        const size_t count = (size_t)view.width * view.height;
        if (count > SIZE_MAX / (packed ? 5 : 4)) return "NVN draw resources: depth plane byte count overflows";
        uint8_t *planar = take(p->workspace, count * (packed ? 5 : 4));
        if (!planar) return allocation_failed;
        // Only the native linear rows are retained. Keep the split Vulkan
        // planes transient; the packed rows preserve stencil and padding.
        uint8_t *stencil = packed ? planar + count * 4 : NULL;
        for (unsigned y = 0; y < view.height; ++y) {
            const uint8_t *src = (const uint8_t *)pixels.data + y * pixels.row_pitch;
            size_t i = (size_t)y * view.width;
            for (unsigned x = 0; x < view.width; ++x, ++i, src += view.bpb) {
                // NVN Z24S8 stores S8 (or padding) in the low byte, unlike
                // Vulkan's X8_D24 copy plane. Z32F_S8 stores Z first.
                memcpy(planar + 4 * i, src + depth24, depth24 ? 3 : 4);
                if (depth24) planar[4 * i + 3] = 0;
                if (stencil) stencil[i] = src[depth24 ? 0 : 4];
            }
        }
        out->pixels = (struct dm_m4_draw_mutable_pixels){planar, count * 4, (size_t)view.width * 4};
        if (stencil) out->stencil = (struct dm_m4_draw_mutable_pixels){stencil, count, view.width};
        writeback->depth = planar;
        writeback->stencil = stencil;
    }
    p->workspace->writeback = writeback;
    return NULL;
}

static VkSamplerAddressMode wrap_mode(unsigned wrap) {
    switch (wrap) {
    case NVN_WRAP_MODE_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case NVN_WRAP_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case NVN_WRAP_MODE_CLAMP_TO_EDGE: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case NVN_WRAP_MODE_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_MAX_ENUM;
    }
}

static const char *sampler_info(const struct dm_m4_draw_sampler *s, int integer, VkSamplerCreateInfo *out) {
    const unsigned min = (s->modes >> 16) & 15, mag = (s->modes >> 20) & 15;
    const unsigned compare = (s->modes >> 24) & 3u, compare_func = s->modes >> 26;
    if (min > NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR || mag > NVN_MAG_FILTER_LINEAR ||
        compare > NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE ||
        (compare && (compare_func < NVN_COMPARE_FUNC_NEVER || compare_func > NVN_COMPARE_FUNC_ALWAYS)) ||
        (s->modes >> 12 & 15) != NVN_SAMPLER_REDUCTION_AVERAGE || s->lod_snap != 0)
        return "NVN draw resources: unsupported sampler filter, compare, reduction or LOD snap";
    VkSamplerAddressMode u = wrap_mode(s->modes & 15), v = wrap_mode(s->modes >> 4 & 15);
    VkSamplerAddressMode w = wrap_mode(s->modes >> 8 & 15);
    // 2D and 2D-array filtering do not use R; the array index is not wrapped.
    if (w == VK_SAMPLER_ADDRESS_MODE_MAX_ENUM) w = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (u == VK_SAMPLER_ADDRESS_MODE_MAX_ENUM || v == VK_SAMPLER_ADDRESS_MODE_MAX_ENUM)
        return "NVN draw resources: sampler clamp/mirror mode is not representable in Vulkan";
    if (!isfinite(s->lod_min) || !isfinite(s->lod_max) || !isfinite(s->lod_bias) || !isfinite(s->aniso) ||
        s->lod_max < 0 || s->lod_max < s->lod_min || s->aniso < 1 || s->aniso > 16)
        return "NVN draw resources: invalid sampler LOD or anisotropy";
    VkBorderColor border = integer ? VK_BORDER_COLOR_INT_TRANSPARENT_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (u == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || v == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
        (integer && w == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER)) {
        if (integer) {
            const uint32_t *b = s->border;
            if (!b[0] && !b[1] && !b[2] && !b[3]) border = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
            else if (!b[0] && !b[1] && !b[2] && b[3] == 1) border = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
            else if (b[0] == 1 && b[1] == 1 && b[2] == 1 && b[3] == 1) border = VK_BORDER_COLOR_INT_OPAQUE_WHITE;
            else return "NVN draw resources: sampler needs a custom integer Vulkan border color";
        } else {
            float b[4];
            memcpy(b, s->border, sizeof(b));
            if (compare && b[0] == 1) border = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
            else if (compare && b[0] == 0) border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
            else if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0) border = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
            else if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 1) border = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
            else if (b[0] == 1 && b[1] == 1 && b[2] == 1 && b[3] == 1) border = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
            else return "NVN draw resources: sampler needs a custom Vulkan border color";
        }
    }
    *out = (VkSamplerCreateInfo){.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = mag == NVN_MAG_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = min & 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = min >= NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = u, .addressModeV = v, .addressModeW = w,
        .compareEnable = compare != 0, .compareOp = compare ? (VkCompareOp)(compare_func - 1u) : VK_COMPARE_OP_NEVER,
        .mipLodBias = s->lod_bias, .anisotropyEnable = s->aniso > 1, .maxAnisotropy = s->aniso,
        .minLod = s->lod_min < 0 ? 0 : s->lod_min, .maxLod = s->lod_max, .borderColor = border};
    if (min < NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST) {
        // Use the base mip without forcing Vulkan to use the magnification filter.
        out->minLod = 0;
        out->maxLod = 0.25f;
    }
    return NULL;
}

static const char *uniform_range(struct preparation *p, unsigned stage, unsigned slot,
                                 NVNbufferRange *range) {
    if (slot >= DMSH_CBSIZE_SLOTS) return "NVN draw resources: uniform bank is outside guest c[3..16]";
    *range = p->state->uniform[stage][slot];
    if (!range->address && !range->size && !(p->state->uniform_set[stage] & (1u << slot)))
        return "NVN draw resources: reflected uniform slot was never bound";
    if ((!range->address != !range->size) || range->size > 65536)
        return "NVN draw resources: invalid uniform address or NVN byte count";
    return NULL;
}

static const char *get_buffer(struct preparation *p, NVNbufferRange range, int write, void **data) {
    if (!range.address || !range.size || range.size > SIZE_MAX || range.size > UINT64_MAX - range.address)
        return "NVN draw resources: missing or invalid guest buffer range";
    if (!p->resolve->buffer) return "NVN draw resources: missing guest buffer resolver";
    *data = p->resolve->buffer(p->resolve->context, range.address, (size_t)range.size, write);
    return *data ? NULL : "NVN draw resources: reflected guest buffer range is unmapped or not writable";
}

static const char *handle_word(struct preparation *p, const struct dm_m4_shader *shader, unsigned stage,
                               unsigned bank, unsigned word, int image, uint32_t *out) {
    const unsigned offset = word * 4;
    if (bank == 2) {
        if (image) {
            if (offset < DMSH_C2_IMG || (offset - DMSH_C2_IMG) % DMSH_C2_ROW ||
                (offset - DMSH_C2_IMG) / DMSH_C2_ROW >= DMSH_C2_IMG_SLOTS)
                return "NVN draw resources: c2 handle does not name an image slot";
            *out = p->state->images[stage][(offset - DMSH_C2_IMG) / DMSH_C2_ROW];
            return *out ? NULL : "NVN draw resources: reflected c2 image slot is unbound";
        }
        if (offset >= DMSH_C2_TEX && offset < DMSH_C2_SEPTEX && !((offset - DMSH_C2_TEX) % DMSH_C2_ROW)) {
            // The reflection gives a word offset, not a guest binding slot.
            // Layout2's handle 24 reads c[2]+0x60: BindTexture slot 8.
            *out = p->state->textures[stage][(offset - DMSH_C2_TEX) / DMSH_C2_ROW];
            return NULL; // The reserved texture-zero descriptor swizzles all four components to zero.
        }
        return "NVN draw resources: c2 handle does not name a captured combined texture slot";
    }
    if (bank == 1) {
        if (!shader->c1 || offset > shader->c1_size || shader->c1_size - offset < sizeof(*out))
            return "NVN draw resources: handle lies outside shader c[1]";
        memcpy(out, shader->c1 + offset, sizeof(*out));
        return NULL;
    }
    if (bank == DMSH_PUSH_BANK && stage == NVN_SHADER_STAGE_COMPUTE) {
        if (offset > sizeof(p->state->compute_push) - sizeof(*out))
            return "NVN compute resources: handle lies outside push constants";
        memcpy(out, p->state->compute_push + offset, sizeof(*out));
        return NULL;
    }
    NVNbufferRange range;
    const char *err = uniform_range(p, stage, bank >= 3 ? bank - 3 : UINT32_MAX, &range);
    if (err) return err;
    const uint64_t bound = (range.size + 255u) & ~UINT64_C(255);
    if (offset > bound || bound - offset < sizeof(*out) || range.address > UINT64_MAX - offset)
        return "NVN draw resources: texture/sampler handle lies outside its bound uniform range";
    void *data;
    if ((err = get_buffer(p, (NVNbufferRange){range.address + offset, sizeof(*out)}, 0, &data)) ||
        (err = track(p, data, sizeof(*out), 0, 0))) return err;
    memcpy(out, data, sizeof(*out));
    return NULL;
}

static const char *image_handle(struct preparation *p, const struct dm_m4_shader *shader, unsigned stage,
                                const struct dm_m4_shader_binding *recipe, uint32_t *out) {
    enum { COMBINED, SEPARATE_HANDLE, SEPARATE_ID, CONSTANT_SAMPLER, DIRECT };
    const unsigned mode = recipe->image.handle >> 28;
    if (mode >= DIRECT || recipe->image.cbuf_slot < -1)
        return "NVN draw resources: dynamic pool or unknown texture handle recipe is unsupported";
    const uint32_t slots = (uint32_t)recipe->image.cbuf_slot;
    const unsigned texture_bank = recipe->image.cbuf_slot == -1 ? 2 : slots & 0xffffu;
    const unsigned sampler_bank = recipe->image.cbuf_slot == -1 || !(slots >> 16) ? texture_bank : (slots >> 16) - 1;
    const unsigned sampler_word = recipe->image.handle >> 14 & 0x3fffu;
    const int image = recipe->source == DM_M4_SHADER_IMAGE;
    const char *err = handle_word(p, shader, stage, texture_bank, recipe->image.handle & 0x3fffu, image, out);
    if (err || mode == COMBINED) return err;
    uint32_t sampler = sampler_word;
    if (mode != CONSTANT_SAMPLER && (err = handle_word(p, shader, stage, sampler_bank, sampler_word, 0, &sampler)))
        return err;
    if (mode != SEPARATE_HANDLE) {
        if (sampler > 0xfff) return "NVN draw resources: separate sampler ID exceeds hardware handle width";
        sampler <<= 20;
    }
    *out |= sampler;
    return NULL;
}

enum {
    RYUJINX_SAMPLER_2D = 3, RYUJINX_SAMPLER_3D = 4, RYUJINX_SAMPLER_CUBE = 5,
    RYUJINX_SAMPLER_2D_ARRAY = RYUJINX_SAMPLER_2D | 0x100, RYUJINX_SAMPLER_SHADOW = 0x400,
};

static const char *sample_depth_planes(struct preparation *p, const struct texture_view *view, unsigned mip,
                                       struct dm_m4_draw_pixels *pixels) {
    const int depth24 = view->format == NVN_FORMAT_DEPTH24 || view->format == NVN_FORMAT_DEPTH24_STENCIL8;
    if (!depth24 && view->format != NVN_FORMAT_DEPTH32F_STENCIL8) return NULL;
    const uint32_t width = mip_dim(view->width, mip);
    const size_t rows = (size_t)mip_dim(view->height, mip) * mip_dim(view->depth, mip);
    const size_t pitch = (size_t)width * 4;
    // Deswizzled planes are already private staging memory. Compact there,
    // but leave storage-backed direct linear rows and their padding intact.
    for (unsigned layer = 0; layer < view->layers; ++layer) {
        struct dm_m4_draw_pixels *plane = &pixels[(size_t)layer * view->levels];
        uint8_t *out = view->pitch ? take(p->workspace, rows * pitch) : (uint8_t *)plane->data;
        if (!out) return allocation_failed;
        for (size_t y = 0; y < rows; ++y) {
            const uint8_t *src = (const uint8_t *)plane->data + y * plane->row_pitch;
            uint8_t *dst = out + y * pitch;
            for (uint32_t x = 0; x < width; ++x, src += view->bpb, dst += 4) {
                uint32_t word;
                memcpy(&word, src, 4);
                // NVN packs stencil/padding in the low byte of Z24.
                if (depth24) word >>= 8;
                memcpy(dst, &word, 4);
            }
        }
        *plane = (struct dm_m4_draw_pixels){out, rows * pitch, pitch};
    }
    return NULL;
}

static enum texture_view_use sampled_use(uint32_t reflected) {
    switch (reflected & ~(uint32_t)RYUJINX_SAMPLER_SHADOW) {
    case RYUJINX_SAMPLER_2D: return TEXTURE_SAMPLED_2D;
    case RYUJINX_SAMPLER_2D_ARRAY: return TEXTURE_SAMPLED_2D_ARRAY;
    case RYUJINX_SAMPLER_3D:
        return reflected & RYUJINX_SAMPLER_SHADOW ? TEXTURE_SINGLE_PLANE : TEXTURE_SAMPLED_3D;
    case RYUJINX_SAMPLER_CUBE: return TEXTURE_SAMPLED_CUBE;
    default: return TEXTURE_SINGLE_PLANE;
    }
}

static const char *sample_description(const struct dm_m4_shader_binding *recipe,
                                      const struct dm_m4_draw_texture *texture, enum texture_view_use use,
                                      const struct texture_view *view, struct dm_m4_draw_sampled_image *out) {
    const VkFormat format = sampled_format(view->format);
    const int depth = is_depth(view->format), shadow = (recipe->image.type & RYUJINX_SAMPLER_SHADOW) != 0;
    const unsigned aspect_mode = texture->view.set & DM_M4_VIEW_DEPTH_STENCIL ?
        texture->view.depth_stencil_mode : texture->depth_stencil_mode;
    if (shadow && !depth) return "NVN draw resources: comparison sampler needs a depth texture";
    if (depth && aspect_mode != NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH)
        return "NVN draw resources: stencil views cannot be sampled by a float shader";
    if (format == VK_FORMAT_UNDEFINED) return "NVN draw resources: unsupported sampled texture format";
    if (recipe->image.format != VK_FORMAT_UNDEFINED && recipe->image.format != format)
        return "NVN draw resources: sampled texture view disagrees with the shader format";
    *out = (struct dm_m4_draw_sampled_image){.format = format, .width = view->width, .height = view->height,
        .level_count = view->levels, .components = shadow ? (VkComponentMapping){0} : view->components,
        .array_layers = use == TEXTURE_SAMPLED_2D_ARRAY ? view->layers : 0,
        .depth = view->depth, .cube = use == TEXTURE_SAMPLED_CUBE};
    return NULL;
}

static const char *decode_sample(struct preparation *p, const struct texture_view *view,
                                 const struct dm_m4_draw_pixels **out) {
    const size_t plane_count = (size_t)view->layers * view->levels;
    struct dm_m4_draw_pixels *levels = take(p->workspace, plane_count * sizeof(*levels));
    if (!levels) return allocation_failed;
    const int depth = is_depth(view->format);
    for (unsigned i = 0; i < view->levels; ++i) {
        const char *err = mip_planes(p, view, i, &levels[i]);
        if (err) return err;
        if (depth && (err = sample_depth_planes(p, view, i, &levels[i]))) return err;
    }
    *out = levels;
    return NULL;
}

static const char *materialize_sample(void *context, const struct dm_m4_draw_pixels **planes) {
    struct staged_image *sample = context;
    struct preparation p = {.workspace = sample->workspace};
    return decode_sample(&p, &sample->view, planes);
}

static const char *write_sample_mip(void *context, uint32_t mip, struct dm_m4_draw_mutable_pixels dst) {
    const struct staged_image *sample = context;
    const struct texture_view *view = &sample->view;
    if (mip >= view->levels) return "NVN draw resources: invalid sampled mip index";
    if (!mip) {
        // Resolve the enclosing span independently of the storage resolver:
        // the decoder addresses the descriptor's native layer stride.
        const nvn_texdesc *desc = &view->desc;
        nvn_texlayout layout;
        nvn_tex_layout(desc, &layout);
        if (view->layer >= layout.layers || view->layers > layout.layers - view->layer ||
            !layout.layer_size || layout.layer_size > view->storage_size / layout.layers)
            return "NVN draw resources: sampled texture backing is smaller than its native layout";
        const uint64_t first = nvn_tex_level_offset(desc, view->level);
        const unsigned end_level = view->level + view->levels;
        const uint64_t end = end_level == desc->levels ? layout.layer_size : nvn_tex_level_offset(desc, end_level);
        if (first >= end || end > layout.layer_size || view->view_offset != view->layer * layout.layer_size + first ||
            view->view_layer_bytes != end - first ||
            (view->layers > 1 && sample->upload.layer_stride != layout.layer_size))
            return "NVN draw resources: sampled texture backing disagrees with native view range";
    }
    const size_t pitch = (mip_dim(view->width, mip) + view->bw - 1) / view->bw * (size_t)view->bpb;
    const size_t rows = (mip_dim(view->height, mip) + view->bh - 1) / view->bh;
    const uint32_t slices = mip_dim(view->depth, mip);
    if (!dst.data || dst.row_pitch != pitch || rows > SIZE_MAX / pitch / slices)
        return "NVN draw resources: sampled mip rows disagree with staging";
    const size_t layer_bytes = pitch * rows * slices;
    if (view->layers > SIZE_MAX / layer_bytes || dst.size != layer_bytes * view->layers)
        return "NVN draw resources: sampled mip layers disagree with staging";
    uint8_t *data = dst.data;
    for (unsigned layer = 0; layer < view->layers; ++layer, data += layer_bytes) {
        const uint64_t phase_start = m4_phase_ticks();
        const int failed = nvn_deswizzle_from(&view->desc, view->level + mip, view->layer + layer,
                                              sample->upload.encoded, view->view_offset, data, pitch);
        m4_phase_add(M4_PHASE_DESWIZZLE, phase_start);
        m4_phase_bytes(M4_BYTES_DESWIZZLE, layer_bytes);
        if (failed)
            return "NVN draw resources: failed to deswizzle guest texture mip";
    }
    return NULL;
}

static void defer_sample(struct preparation *p, struct staged_image *sample, const struct texture_view *view,
                         uint64_t generation, unsigned id, unsigned kind) {
    sample->workspace = p->workspace;
    sample->view = *view;
    // Raw depth and color blocks need no intermediate plane. Packed depth
    // must still be extracted/repacked; linear textures retain their pitch.
    const int direct = !view->pitch && (!is_depth(view->format) ||
                       view->format == NVN_FORMAT_DEPTH16 || view->format == NVN_FORMAT_DEPTH32F);
    sample->upload = (struct dm_m4_draw_sample_upload){
        .pool_generation = generation, .id = id, .kind = kind, .layers = view->layers,
        .layout = {.desc = view->desc, .offset = view->view_offset, .pitch = view->pitch,
                   .base_level = view->level, .base_layer = view->layer, .format = view->format},
        .encoded = (const uint8_t *)view->storage + view->view_offset,
        .encoded_size = view->storage_size - view->view_offset,
        .layer_bytes = view->view_layer_bytes,
        .layer_stride = view->view_stride ? view->view_stride : view->view_layer_bytes,
        .context = sample, .materialize = direct ? NULL : materialize_sample,
        .write_mip = direct ? write_sample_mip : NULL,
    };
    sample->image.upload = &sample->upload;
}

static struct staged_image **fixed_sample_slot(struct preparation *p, unsigned id, unsigned kind) {
    const uint32_t key = id | (uint32_t)kind << 20;
    const size_t mask = p->image_capacity - 1;
    for (size_t i = (key ^ (key >> 13)) * UINT32_C(0x9e3779b1) & mask; ; i = (i + 1) & mask) {
        struct staged_image **slot = &p->images[i];
        if (!*slot || ((*slot)->id == id && (*slot)->kind == kind)) return slot;
    }
}

static const char *reserve_fixed_sample(struct preparation *p) {
    if (p->image_count < p->image_capacity / 2) return NULL;
    const size_t old_capacity = p->image_capacity;
    struct staged_image **old = p->images;
    if (old_capacity > SIZE_MAX / 2 / sizeof *old) return allocation_failed;
    const size_t capacity = old_capacity ? old_capacity * 2 : 16;
    struct staged_image **images = take(p->workspace, capacity * sizeof *images);
    if (!images) return allocation_failed;
    memset(images, 0, capacity * sizeof *images);
    p->images = images;
    p->image_capacity = capacity;
    for (size_t i = 0; i < old_capacity; ++i)
        if (old[i]) *fixed_sample_slot(p, old[i]->id, old[i]->kind) = old[i];
    return NULL;
}

static const char *sampled_image(struct preparation *p, const struct dm_m4_shader *shader, unsigned stage,
                                 const struct dm_m4_shader_binding *recipe, struct dm_m4_draw_binding *out) {
    const enum texture_view_use use = sampled_use(recipe->image.type);
    const int volume = use == TEXTURE_SAMPLED_3D, shadow = (recipe->image.type & RYUJINX_SAMPLER_SHADOW) != 0;
    if (recipe->descriptor_type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || use == TEXTURE_SINGLE_PLANE ||
        recipe->image.row_bias || recipe->image.tbx != -1)
        return "NVN draw resources: unsupported sampled binding or image shape";
    uint32_t handle;
    const char *err = image_handle(p, shader, stage, recipe, &handle);
    if (err) return err;
    const unsigned tid = handle & 0xfffffu, sid = handle >> 20;
    if (!tid && handle) return "NVN draw resources: noncanonical handle has a null texture ID";
    struct staged_image **slot = tid && p->image_capacity ? fixed_sample_slot(p, tid, recipe->image.type) : NULL;
    struct dm_m4_draw_texture texture = {0};
    int integer = 0;
    if (slot && *slot) integer = integer_format((*slot)->view.format);
    else if (tid) {
        if (!p->resolve->texture || !p->resolve->texture(p->resolve->context, tid, &texture))
            return "NVN draw resources: sampled handle names an unregistered texture in the active pool";
        integer = integer_format(texture.view.set & DM_M4_VIEW_FORMAT ? texture.view.format : texture.desc.format);
    }

    struct dm_m4_draw_sampler sampler;
    if (!p->resolve->sampler || !p->resolve->sampler(p->resolve->context, sid, &sampler))
        return "NVN draw resources: sampled handle names an unregistered sampler in the active pool";
    // An ordinary TEX has no reference operand. N1X returns the filtered
    // color/depth texel even when this same sampler enables comparison for
    // Dref instructions. Derive the Vulkan sampler before mapping its border.
    if (!shadow && ((sampler.modes >> 24) & 3u) == NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE)
        sampler.modes &= ~(3u << 24);
    VkSamplerCreateInfo *sampler_vk = take(p->workspace, sizeof *sampler_vk);
    if (!sampler_vk) return allocation_failed;
    if ((err = sampler_info(&sampler, integer, sampler_vk))) return err;
    if (integer && shader->unfiltered_image_ops) {
        // Texel fetch and gather never interpolate. Nearest is legal for the
        // integer VkFormat and leaves coordinates, address modes and LOD intact.
        sampler_vk->minFilter = sampler_vk->magFilter = VK_FILTER_NEAREST;
        sampler_vk->mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler_vk->anisotropyEnable = VK_FALSE;
        sampler_vk->maxAnisotropy = 1.0f;
    }
    out->sampler = sampler_vk;
    if (!handle) {
        if (!p->resolve->pool_generation || !p->resolve->pool_generation(p->resolve->context, 0))
            return "NVN draw resources: null texture without a live texture pool";
        // The null TIC supplies zero, including Dref with either compare
        // function. Use the operand kind required by Vulkan for this read.
        sampler_vk->compareEnable = shadow ? VK_TRUE : VK_FALSE;
        return NULL;
    }
    if ((sampler_vk->compareEnable != 0) != shadow) {
        fprintf(stderr, "[nvn] sampler mismatch: stage %u binding %u shader type %#x, texture %u sampler %u modes %#x\n",
                stage, recipe->binding, recipe->image.type, tid, sid, sampler.modes);
        return "NVN draw resources: sampler compare mode disagrees with the shader";
    }
    if (slot && *slot) {
        if (recipe->image.format != VK_FORMAT_UNDEFINED && recipe->image.format != (*slot)->image.format)
            return "NVN draw resources: sampled texture view disagrees with the shader format";
        out->texture = &(*slot)->image;
        return NULL;
    }
    const int target = texture.view.set & DM_M4_VIEW_TARGET ? texture.view.target : texture.desc.target;
    // A 2D TEX against a 3D descriptor, or 3D TEX against 2D, returns
    // (0,0,0,0) on N1X, even for depth-one volumes with identical bytes.
    // This is not a claim about TXQ results or a missing registration.
    if (!shadow && !shader->queries_images && texture.desc.samples <= 1 &&
        ((volume && target == NVN_TEXTURE_TARGET_2D && texture.desc.target == NVN_TEXTURE_TARGET_2D) ||
         (use == TEXTURE_SAMPLED_2D && target == NVN_TEXTURE_TARGET_3D && texture.desc.target == NVN_TEXTURE_TARGET_3D)))
        return NULL;
    struct texture_view view;
    struct dm_m4_draw_sampled_image image = {0};
    err = get_texture_view(p, &texture, use, 0, &view);
    if (!err) err = sample_description(recipe, &texture, use, &view, &image);
    // Compute's alias checks inspect the original linear planes, not the
    // GPU copy. Keep those available, as with resolvers without a generation.
    const uint64_t generation = !err && !p->groups && p->resolve->pool_generation ?
        p->resolve->pool_generation(p->resolve->context, 0) : 0;
    if (!err && !generation) err = decode_sample(p, &view, &image.levels);
    if (err) {
        fprintf(stderr, "[nvn] sampled texture %u sampler %u LOD %.2f..%.2f: format %d %ux%ux%u level %u+%u/%u layer %u+%u\n",
                tid, sid, sampler_vk->minLod, sampler_vk->maxLod, texture.desc.format, texture.desc.width,
                texture.desc.height, texture.desc.depth, texture.view.base_level, texture.view.levels,
                texture.desc.levels, texture.view.base_layer, texture.view.layers);
        return err;
    }
    if ((err = reserve_fixed_sample(p))) return err;
    struct staged_image *staged = take(p->workspace, sizeof(*staged));
    if (!staged) return allocation_failed;
    staged->view.format = view.format;
    staged->id = tid;
    staged->kind = recipe->image.type;
    staged->image = image;
    if (generation) defer_sample(p, staged, &view, generation, tid, recipe->image.type);
    *fixed_sample_slot(p, tid, recipe->image.type) = staged;
    ++p->image_count;
    out->texture = &staged->image;
    return NULL;
}

static const char *storage_image(struct preparation *p, const struct dm_m4_shader *shader, unsigned stage,
                                 const struct dm_m4_shader_binding *recipe, struct dm_m4_draw_binding *out) {
    enum { RYUJINX_IMAGE_STORE = 4 };
    const int volume = recipe->image.type == RYUJINX_SAMPLER_3D;
    if (recipe->descriptor_type != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
        (!volume && recipe->image.type != RYUJINX_SAMPLER_2D) || recipe->image.row_bias || recipe->image.tbx != -1)
        return "NVN compute resources: storage image requires a plain 2D or 3D descriptor";
    uint32_t handle;
    const char *err = image_handle(p, shader, stage, recipe, &handle);
    if (err) return err;
    struct dm_m4_draw_texture texture;
    const unsigned id = handle & 0xfffffu;
    if (!id || !p->resolve->texture || !p->resolve->texture(p->resolve->context, id, &texture))
        return "NVN compute resources: image handle names no registered descriptor in the active texture pool";

    const int write = (recipe->image.flags & RYUJINX_IMAGE_STORE) != 0;
    struct texture_view view;
    if ((err = get_texture_view(p, &texture, volume ? TEXTURE_STORAGE_3D : TEXTURE_SINGLE_PLANE, write, &view))) return err;
    VkFormat format = dm_m4_nvn_attachment_format((NVNformat)view.format);
    if (format == VK_FORMAT_UNDEFINED || is_depth(view.format) || view.bw != 1 || view.bh != 1)
        return "NVN compute resources: unsupported storage image format";
    // NVN word image atomics operate on the raw 32-bit colour texel, not
    // its component values. Stage those same bytes as the shader's R32 view.
    if (recipe->image.atomic_word && view.bpb == 4) format = recipe->image.format;
    if (recipe->image.format != VK_FORMAT_UNDEFINED && recipe->image.format != format)
        return "NVN compute resources: storage image view disagrees with the shader format";

    struct dm_m4_draw_storage_image *image = take(p->workspace, sizeof(*image));
    if (!image) return allocation_failed;
    *image = (struct dm_m4_draw_storage_image){.format = format, .width = view.width, .height = view.height,
        .depth = view.depth};
    if ((err = mip_planes(p, &view, 0, &image->pixels))) return err;
    if (write) {
        image->writeback = (void *)image->pixels.data;
        if (!view.pitch) {
            struct dm_m4_resource_writeback *w = take(p->workspace, sizeof(*w));
            if (!w) return allocation_failed;
            *w = (struct dm_m4_resource_writeback){.next = p->workspace->writeback, .view = view,
                .pixels = image->writeback, .row_pitch = image->pixels.row_pitch};
            p->workspace->writeback = w;
        }
    }
    out->image = image;
    return NULL;
}

static unsigned storage_base(unsigned stage) {
    if (stage == NVN_SHADER_STAGE_COMPUTE) return DMSH_C0_SSBO_CS;
    if (stage == DM_M4_NV_TASK_STAGE) return DMSH_C0_SSBO_TS;
    if (stage == DM_M4_NV_MESH_STAGE) return DMSH_C0_SSBO_MS;
    return stage == NVN_SHADER_STAGE_VERTEX ? DMSH_C0_SSBO_VS : DMSH_C0_SSBO_PS;
}

static const char *storage_buffer(struct preparation *p, unsigned stage, const struct dm_m4_shader_binding *recipe,
                                  struct dm_m4_draw_binding *out) {
    const unsigned base = storage_base(stage);
    if (recipe->sbuf.cb_slot != 0 || recipe->sbuf.cb_offset > UINT32_MAX / 4 || (recipe->sbuf.flags & ~1u))
        return "NVN draw resources: storage address requires an unsupported indirect, push or global-memory range";
    const unsigned offset = recipe->sbuf.cb_offset * 4;
    const int from_uniform = stage == NVN_SHADER_STAGE_COMPUTE && offset >= DMSH_C0_CS_UBO && offset < base;
    const unsigned row = from_uniform ? DMSH_C0_CS_UBO : base;
    static_assert(DMSH_C0_CS_UBO_SLOTS == DM_M4_DRAW_UBOS && DMSH_C0_SSBO_SLOTS == DM_M4_DRAW_SSBOS,
                  "guest buffer arrays must match the shader driver tables");
    if (offset < row || (offset - row) % DMSH_C0_ROW)
        return "NVN draw resources: storage address is outside the stage's c0 buffer tables";
    const unsigned slot = (offset - row) / DMSH_C0_ROW;
    NVNbufferRange range;
    if (from_uniform) {
        if (slot >= DM_M4_DRAW_UBOS) return "NVN draw resources: uniform slot outside driver table";
        range = p->state->uniform[NVN_SHADER_STAGE_COMPUTE][slot];
    } else {
        if (slot >= DM_M4_DRAW_SSBOS) return "NVN draw resources: storage slot outside driver table";
        range = p->state->storage[stage][slot];
    }
    const int write = recipe->sbuf.flags & 1;
    void *data;
    const char *err = get_buffer(p, range, write, &data);
    if (err) return err;
    if ((err = track(p, data, (size_t)range.size, write, 1))) return err;
    out->buffer = (struct dm_m4_draw_span){data, (size_t)range.size};
    if (write) out->writeback = data;
    return NULL;
}

static const char *constant_buffer(struct preparation *p, unsigned stage, const struct dm_m4_shader_binding *recipe,
                                   struct dm_m4_draw_binding *out) {
    const unsigned bank = recipe->cbuf.slot;
    if (bank == 1) return NULL; // c1 comes from the immutable loaded shader
    if (bank == DMSH_PUSH_BANK && stage == NVN_SHADER_STAGE_COMPUTE) {
        out->buffer = (struct dm_m4_draw_span){p->state->compute_push, sizeof(p->state->compute_push)};
        return NULL;
    }
    if (bank == 0 || bank == 2 || bank == DMSH_CBSIZE_BANK) {
        const size_t size = bank == DMSH_CBSIZE_BANK ? (DMSH_CBSIZE_SLOTS + 3) / 4 * 16 : DMSH_DRV_SIZE;
        uint8_t *data = take(p->workspace, size);
        if (!data) return allocation_failed;
        memset(data, 0, size);
        if (bank == 0) {
            for (unsigned i = 0; i < DMSH_C0_SSBO_SLOTS; ++i) {
                NVNbufferRange range = p->state->storage[stage][i];
                if (range.size > UINT32_MAX) return "NVN draw resources: storage size exceeds its c0 field";
                const uint32_t n = (uint32_t)range.size;
                uint8_t *row = data + storage_base(stage) + i * DMSH_C0_ROW;
                memcpy(row, &range.address, sizeof(range.address));
                memcpy(row + sizeof(range.address), &n, sizeof(n));
            }
            if (stage == NVN_SHADER_STAGE_COMPUTE) {
                memcpy(data + DMSH_C0_DISPATCH, p->groups, sizeof(uint32_t) * 3);
                memcpy(data + DMSH_C0_DISPATCH_NVN2, p->groups, sizeof(uint32_t) * 3);
                for (unsigned i = 0; i < DMSH_C0_CS_UBO_SLOTS; ++i) {
                    const NVNbufferRange range = p->state->uniform[stage][i];
                    if (range.size > 65536) return "NVN compute resources: uniform size exceeds its NVN bank limit";
                    // NVN2's bindless uniform handle stores a 16-byte address
                    // unit in 45 bits and a size in the remaining 19 bits.
                    const uint32_t n = (uint32_t)range.size;
                    const uint64_t handle = ((range.address >> 4) & ((UINT64_C(1) << 45) - 1)) |
                                            (((uint64_t)n + 15) >> 4 << 45);
                    uint8_t *row = data + DMSH_C0_CS_UBO + i * DMSH_C0_ROW;
                    memcpy(row, &handle, sizeof(handle));
                    memcpy(row + sizeof(handle), &n, sizeof(n));
                }
            } else {
                const float point_size = 2047.f;
                memcpy(data + DMSH_C0_POINT_SIZE, &point_size, sizeof(point_size));
                memcpy(data + DMSH_C0_BASE_INSTANCE, &p->first_instance, sizeof(p->first_instance));
                // NVN2's centered sample has XOR 0x88 applied and zero index masks.
            }
        } else if (bank == 2) {
            for (unsigned i = 0; i < DMSH_C2_TEX_SLOTS; ++i)
                memcpy(data + DMSH_C2_TEX + i * DMSH_C2_ROW, &p->state->textures[stage][i], sizeof(uint32_t));
            for (unsigned i = 0; i < DMSH_C2_IMG_SLOTS; ++i) {
                const uint32_t image = p->state->images[stage][i];
                if (image) memcpy(data + DMSH_C2_IMG + i * DMSH_C2_ROW, &image, sizeof(image));
            }
        } else {
            for (unsigned i = 0; i < DMSH_CBSIZE_SLOTS; ++i) {
                const NVNbufferRange range = p->state->uniform[stage][i];
                if (range.address && range.size > 65536) return "NVN draw resources: uniform size exceeds its NVN bank limit";
                const uint32_t n = range.address ? ((uint32_t)range.size + 255u) & ~255u : 0;
                memcpy(data + i * sizeof(n), &n, sizeof(n));
            }
        }
        out->buffer = (struct dm_m4_draw_span){data, size};
        return NULL;
    }
    if (bank < 3 || bank >= 3 + DMSH_CBSIZE_SLOTS)
        return "NVN draw resources: unsupported push, texel bias or storage-length constant bank";
    const unsigned fixed = recipe->cbuf.fixed_size;
    if (fixed > 65536) return "NVN draw resources: reflected uniform read exceeds its NVN bank limit";
    NVNbufferRange range;
    const char *err = uniform_range(p, stage, bank - 3, &range);
    if (err) return err;
    void *data = NULL;
    size_t live = (size_t)range.size;
    if (live) {
        // NVN2 rounds the hardware bound up to 256 bytes: if the rounded
        // span is mapped, the additional bytes are real guest memory.
        const size_t rounded = (live + 255u) & ~(size_t)255u;
        if (rounded != live && !get_buffer(p, (NVNbufferRange){range.address, rounded}, 0, &data))
            live = rounded;
        else if ((err = get_buffer(p, range, 0, &data)))
            return err;
    }
    size_t size = ((size_t)range.size + 255u) & ~(size_t)255u;
    if (size < fixed) size = fixed;
    if (!size) size = 16; // explicit null binding, not an unobserved resource
    if (live && (err = track_range(p, data, live, 0, p->gmem && live != size ? live : 0, 0, RANGE_OTHER)))
        return err;
    if (size != live) {
        uint8_t *padded = take(p->workspace, size);
        if (!padded) return allocation_failed;
        if (live) memcpy(padded, data, live);
        memset(padded + live, 0, size - live);
        data = padded;
    }
    out->buffer = (struct dm_m4_draw_span){data, size};
    return NULL;
}

// A shared pool can contain 2D, array, cube and volume descriptors. Compact
// each image class independently; the checked shader faults if it actually
// uses an ID in the wrong class. Never substitute a stand-in image.
static int direct_texture_shape(const struct dm_m4_draw_texture *t, enum texture_view_use use) {
    const int target = t->view.set & DM_M4_VIEW_TARGET ? t->view.target : t->desc.target;
    switch (use) {
    case TEXTURE_SAMPLED_2D: return target == NVN_TEXTURE_TARGET_2D || target == NVN_TEXTURE_TARGET_2D_ARRAY;
    case TEXTURE_SAMPLED_2D_ARRAY: return target == NVN_TEXTURE_TARGET_2D_ARRAY;
    case TEXTURE_SAMPLED_3D: return target == NVN_TEXTURE_TARGET_3D;
    case TEXTURE_SAMPLED_CUBE:
        return target == NVN_TEXTURE_TARGET_CUBEMAP || target == NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    default: return 0;
    }
}

static struct dm_m4_bindless_pool *snapshot_pool(struct preparation *p, int sampler, uint64_t generation,
                                                 uint32_t limit) {
    struct dm_m4_bindless_pool **cache = &p->workspace->bindless[sampler], *pool = *cache;
    if (pool && generation && pool->generation == generation && pool->limit == limit) return pool;
    if (!pool) {
        pool = calloc(1, sizeof *pool);
        if (!pool) return NULL;
        *cache = pool;
    }
    pool->generation = 0;
    uint32_t count = 0;
    for (uint32_t id = sampler ? 0 : 1; id < limit; ++id) {
        struct bindless_slot slot = {.id = id};
        const int present = sampler ? p->resolve->sampler && p->resolve->sampler(p->resolve->context, id, &slot.sampler)
                                    : p->resolve->texture && p->resolve->texture(p->resolve->context, id, &slot.texture);
        if (!present) continue;
        if (count == pool->capacity) {
            const size_t capacity = count ? (size_t)count * 2u : 64u;
            if (capacity > UINT32_MAX || capacity > SIZE_MAX / sizeof slot) return NULL;
            struct bindless_slot *slots = realloc(pool->slots, capacity * sizeof slot);
            if (!slots) return NULL;
            pool->slots = slots;
            pool->capacity = (uint32_t)capacity;
        }
        pool->slots[count++] = slot;
    }
    pool->generation = generation;
    pool->limit = limit;
    pool->count = count;
    return pool;
}

static const char *direct_descriptor(struct preparation *p, const struct dm_m4_shader_binding *recipe,
                                     enum texture_view_use use, const struct bindless_slot *slot,
                                     const struct dm_m4_draw_sampled_image **textures, VkSamplerCreateInfo *samplers,
                                     uint64_t generation, uint32_t *count, uint32_t capacity, uint32_t *table) {
    const char *err;
    if (recipe->descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLER) {
        VkSamplerCreateInfo state;
        if (sampler_info(&slot->sampler, 0, &state)) return NULL; // fault only if this ID is actually used
        if (*count == capacity) return "NVN draw resources: compact sampler capacity exceeded";
        samplers[*count] = state;
    } else {
        const struct dm_m4_draw_texture *guest = &slot->texture;
        if (!direct_texture_shape(guest, use)) return NULL;
        const int format = guest->view.set & DM_M4_VIEW_FORMAT ? guest->view.format : guest->desc.format;
        // One bindless variable has one declared numeric type. Fixed
        // bindings can select a typed variant; mixed runtime arrays cannot.
        if (integer_format(format)) return NULL;
        if ((recipe->image.type & RYUJINX_SAMPLER_SHADOW) && !is_depth(format)) return NULL;
        struct texture_view view;
        if ((err = get_texture_view(p, guest, use, 0, &view)))
            return err == allocation_failed ? err : NULL; // Checked lookup faults only for a consumed invalid view.
        struct dm_m4_draw_sampled_image image;
        if (sample_description(recipe, guest, use, &view, &image)) return NULL;
        if (*count == capacity) return "NVN draw resources: compact image capacity exceeded";
        struct staged_image *stored = take(p->workspace, sizeof *stored);
        if (!stored) return allocation_failed;
        stored->image = image;
        if (p->groups) {
            if ((err = decode_sample(p, &view, &stored->image.levels))) return err;
        } else {
            defer_sample(p, stored, &view, generation, slot->id, recipe->image.type);
        }
        textures[*count] = &stored->image;
    }
    table[DM_M4_BINDLESS_HEADER_WORDS + slot->id] = (*count)++;
    return NULL;
}

static const char *stage_direct(struct preparation *p, const struct dm_m4_shader_binding *recipe,
                                const struct dm_m4_shader_binding *map, struct dm_m4_draw_span material,
                                struct dm_m4_draw_binding *images, struct dm_m4_draw_binding *remap) {
    const int sampler = recipe->descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLER;
    const enum texture_view_use use = sampled_use(recipe->image.type);
    const uint32_t limit = sampler ? p->state->sampler_pool.count : p->state->texture_pool.count;
    if ((!sampler && (recipe->descriptor_type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || use == TEXTURE_SINGLE_PLANE)) ||
        limit > (sampler ? 1u << 12 : 1u << 20))
        return "NVN draw resources: unsupported direct descriptor shape or pool";
    const int full_pool = map->sbuf.slot == UINT32_MAX;
    const uint64_t generation = p->resolve->pool_generation ? p->resolve->pool_generation(p->resolve->context, sampler) : 0;
    const struct dm_m4_bindless_pool *pool = full_pool ? snapshot_pool(p, sampler, generation, limit) : NULL;
    if (full_pool && !pool) return allocation_failed;
    const unsigned stride = map->sbuf.record_stride;
    if (!full_pool && (!material.data || !material.size || stride < 4 || !map->sbuf.record_words))
        return "NVN draw resources: missing live bindless material records";
    const size_t bit_bytes = full_pool ? 0 : ((size_t)limit + 31u) / 32u * sizeof(uint32_t);
    uint32_t *bits = bit_bytes ? take(p->workspace, bit_bytes) : NULL;
    if (bit_bytes && !bits) return allocation_failed;
    if (bit_bytes) memset(bits, 0, bit_bytes);
    unsigned offsets[32], n_offsets = 0;
    for (unsigned i = 0; i < 32; ++i)
        if (map->sbuf.record_words & (1u << i)) offsets[n_offsets++] = 4u * i;
    uint32_t candidates = pool ? pool->count : 0;
    uint32_t guest_count = candidates ? pool->slots[candidates - 1].id + 1u : 0;
    const uint8_t *bytes = material.data;
    if (!pool) for (size_t at = 0; at < material.size; at += stride) {
        for (unsigned j = 0; j < n_offsets; ++j) {
            if (offsets[j] + 4u > material.size - at) break;
            uint32_t packed;
            memcpy(&packed, bytes + at + offsets[j], sizeof packed);
            const uint32_t id = sampler ? packed >> 20 : packed & 0xfffffu;
            if (id >= limit || (!sampler && !id)) continue;
            const uint32_t bit = UINT32_C(1) << (id & 31u);
            if (bits[id >> 5] & bit) continue;
            bits[id >> 5] |= bit;
            if (guest_count < id + 1u) guest_count = id + 1u;
            ++candidates;
        }
        if (stride > material.size - at) break;
    }
    // The shader's OOB test uses this trimmed table's actual size. Unseen
    // and out-of-pool IDs both fault without touching any Vulkan descriptor.
    const size_t table_bytes = (DM_M4_BINDLESS_HEADER_WORDS + (size_t)guest_count) * sizeof(uint32_t);
    uint32_t *table = take(p->workspace, table_bytes);
    if (!table) return allocation_failed;
    table[0] = 0;
    table[1] = guest_count;
    memset(table + DM_M4_BINDLESS_HEADER_WORDS, 0xff, guest_count * sizeof(uint32_t));
    const uint32_t capacity = candidates < recipe->count ? candidates : recipe->count;
    const struct dm_m4_draw_sampled_image **textures = NULL;
    VkSamplerCreateInfo *samplers = NULL;
    if (capacity) {
        if (sampler) samplers = take(p->workspace, (size_t)capacity * sizeof *samplers);
        else textures = take(p->workspace, (size_t)capacity * sizeof *textures);
        if (!textures && !samplers) return allocation_failed;
    }
    uint32_t count = 0;
    if (pool) {
        for (uint32_t i = 0; i < pool->count; ++i) {
            const char *err = direct_descriptor(p, recipe, use, &pool->slots[i], textures, samplers,
                                                generation, &count, capacity, table);
            if (err) return err;
        }
    } else {
        for (uint32_t first = 0; first < guest_count; first += 32u) {
            const uint32_t word = bits[first / 32u];
            if (!word) continue;
            for (uint32_t j = 0; j < 32u; ++j) {
                if (!(word & (UINT32_C(1) << j))) continue;
                struct bindless_slot slot = {.id = first + j};
                const int present = sampler ?
                    p->resolve->sampler && p->resolve->sampler(p->resolve->context, slot.id, &slot.sampler) :
                    p->resolve->texture && p->resolve->texture(p->resolve->context, slot.id, &slot.texture);
                if (!present) continue;
                const char *err = direct_descriptor(p, recipe, use, &slot, textures, samplers,
                                                    generation, &count, capacity, table);
                if (err) return err;
            }
        }
    }
    table[2] = count;
    if (sampler) images->sampler_array = samplers;
    else images->texture_array = textures;
    images->descriptor_count = count;
    remap->buffer = (struct dm_m4_draw_span){table, table_bytes};
    return NULL;
}

static const char *stage_bindings(struct preparation *p, struct dm_m4_draw_stage *stage, unsigned guest_stage) {
    const struct dm_m4_shader *shader = stage->shader;
    if (!shader || !shader->binding_count) return NULL;
    if (!shader->bindings) return "NVN draw resources: missing shader binding reflection";
    struct dm_m4_draw_binding *out = take(p->workspace, shader->binding_count * sizeof(*out));
    if (!out) return allocation_failed;
    stage->bindings = out;
    for (unsigned i = 0; i < shader->binding_count; ++i) {
        const struct dm_m4_shader_binding *recipe = &shader->bindings[i];
        out[i] = (struct dm_m4_draw_binding){0};
        if (dm_m4_shader_direct(recipe)) continue; // pair with its map after ordinary buffers are resolved
        if (recipe->source == DM_M4_SHADER_BINDLESS_MAP) continue;
        if (recipe->count != 1) return "NVN draw resources: shader needs a Vulkan descriptor array";
        const char *err;
        switch (recipe->source) {
        case DM_M4_SHADER_SUPPORT: continue; // draw backend owns this descriptor
        case DM_M4_SHADER_GMEM_MAP:
        case DM_M4_SHADER_GMEM_LOG:
            if (!p->gmem || !(shader->gmem & 1u))
                return "NVN draw resources: unavailable checked global memory map";
            continue; // draw backend owns the companion map and log
        case DM_M4_SHADER_CBUF: err = constant_buffer(p, guest_stage, recipe, &out[i]); break;
        case DM_M4_SHADER_SBUF: err = storage_buffer(p, guest_stage, recipe, &out[i]); break;
        case DM_M4_SHADER_TEXTURE: err = sampled_image(p, shader, guest_stage, recipe, &out[i]); break;
        case DM_M4_SHADER_IMAGE:
            if (guest_stage != NVN_SHADER_STAGE_COMPUTE)
                return "NVN draw resources: a graphics shader uses an unsupported storage image";
            err = storage_image(p, shader, guest_stage, recipe, &out[i]);
            break;
        default: return "NVN draw resources: unknown Vulkan descriptor source";
        }
        if (err) return err;
    }
    return NULL;
}

static const char *stage_direct_bindings(struct preparation *p, const struct dm_m4_draw_stage *stage) {
    const struct dm_m4_shader *shader = stage->shader;
    if (!shader || !shader->binding_count) return NULL;
    // Only called after stage_bindings initialized the mutable workspace array.
    struct dm_m4_draw_binding *out = (struct dm_m4_draw_binding *)stage->bindings;
    for (unsigned i = 0; i < shader->binding_count; ++i) {
        const struct dm_m4_shader_binding *map = &shader->bindings[i];
        if (map->source != DM_M4_SHADER_BINDLESS_MAP) continue;
        unsigned image = UINT_MAX;
        struct dm_m4_draw_span material = {0};
        for (unsigned j = 0; j < shader->binding_count; ++j) {
            if (shader->bindings[j].binding == map->sbuf.cb_offset) image = j;
            if (shader->bindings[j].binding == map->sbuf.slot) material = out[j].buffer;
        }
        if (image == UINT_MAX || !dm_m4_shader_direct(&shader->bindings[image]))
            return "NVN draw resources: invalid compact descriptor map association";
        const char *err = stage_direct(p, &shader->bindings[image], map, material, &out[image], &out[i]);
        if (err) return err;
    }
    return NULL;
}

static void reset_workspace(struct dm_m4_guest_resources *workspace) {
    workspace->writeback = NULL;
    for (struct dm_m4_resource_chunk *chunk = workspace->chunks; chunk; chunk = chunk->next) chunk->used = 0;
}

static const char *gmem_views(const struct preparation *p, struct dm_m4_gmem_source *source);

const char *dm_m4_guest_resources_prepare(struct dm_m4_guest_resources *workspace,
                                         const struct dm_m4_draw_state *state,
                                         const struct dm_m4_draw_resolver *resolve,
                                         struct dm_m4_draw_info *info) {
    if (!workspace || !state || !resolve || !info || (!info->vs.shader && !info->ms.shader))
        return "NVN draw resources: missing workspace, state, resolver or pre-raster shader";
    reset_workspace(workspace);
    info->vs.bindings = NULL;
    info->ps.bindings = NULL;
    info->ms.bindings = NULL;
    info->ts.bindings = NULL;
    info->gmem = (struct dm_m4_gmem_source){.visit = resolve->gmem_visit, .context = resolve->context};
    static_assert((unsigned)DM_M4_DRAW_TARGETS == (unsigned)DM_M4_NVN_COLOR_TARGETS, "guest and Vulkan color slot counts");
    if (state->color_count > DM_M4_DRAW_TARGETS)
        return "NVN draw resources: invalid color attachment count";
    info->color_count = 0;
    info->depth = (struct dm_m4_draw_attachment){0};
    struct preparation prep = {.workspace = workspace, .state = state, .resolve = resolve,
        .first_instance = info->first_instance,
        .gmem = (info->vs.shader ? info->vs.shader->gmem : 0) | (info->ps.shader ? info->ps.shader->gmem : 0) |
                (info->ms.shader ? info->ms.shader->gmem : 0) | (info->ts.shader ? info->ts.shader->gmem : 0)};
    const char *err = NULL;
    for (unsigned i = 0; i < state->color_count; ++i) {
        if (!state->colors[i].pool) {
            info->colors[i] = (struct dm_m4_draw_attachment){0};
            continue;
        }
        err = attachment(&prep, &state->colors[i], WRITEBACK_COLOR0 + i, &info->colors[i]);
        if (err) break;
        info->color_count = i + 1;
    }
    if (!err && state->depth.pool) err = attachment(&prep, &state->depth, WRITEBACK_DEPTH_STENCIL, &info->depth);
    if (!err) err = shading_rate(&prep, info);
    if (!err) err = stage_bindings(&prep, &info->vs, NVN_SHADER_STAGE_VERTEX);
    if (!err) err = stage_bindings(&prep, &info->ps, NVN_SHADER_STAGE_FRAGMENT);
    if (!err) err = stage_bindings(&prep, &info->ms, DM_M4_NV_MESH_STAGE);
    if (!err) err = stage_bindings(&prep, &info->ts, DM_M4_NV_TASK_STAGE);
    if (!err) err = stage_direct_bindings(&prep, &info->vs);
    if (!err) err = stage_direct_bindings(&prep, &info->ps);
    if (!err) err = stage_direct_bindings(&prep, &info->ms);
    if (!err) err = stage_direct_bindings(&prep, &info->ts);
    if (!err && (prep.gmem & 2u)) err = gmem_views(&prep, &info->gmem);
    if (err) workspace->writeback = NULL;
    return err;
}

static VkImageAspectFlags writeback_aspects(const struct dm_m4_resource_writeback *w,
                                          const struct dm_m4_draw_attachment_writes *writes) {
    if (w->target == WRITEBACK_STORAGE) return VK_IMAGE_ASPECT_COLOR_BIT;
    if (!writes) return 0;
    if (w->target == WRITEBACK_DEPTH_STENCIL) return writes->depth_stencil;
    return (writes->colors & (1u << (w->target - WRITEBACK_COLOR0))) ? VK_IMAGE_ASPECT_COLOR_BIT : 0;
}

const char *dm_m4_guest_resources_writeback(struct dm_m4_guest_resources *workspace,
                                           const struct dm_m4_draw_attachment_writes *writes) {
    for (const struct dm_m4_resource_writeback *w = workspace->writeback; w; w = w->next) {
        const VkImageAspectFlags aspects = writeback_aspects(w, writes);
        if (!aspects) continue;
        const struct texture_view *view = &w->view;
        m4_phase_bytes(M4_BYTES_WRITEBACK, (uint64_t)view->width * view->height * view->bpb);
        if (w->depth) {
            // The packed rows still hold the pre-draw bytes of the other plane and texel padding.
            const int depth24 = view->format == NVN_FORMAT_DEPTH24 || view->format == NVN_FORMAT_DEPTH24_STENCIL8;
            for (unsigned y = 0; y < view->height; ++y) {
                uint8_t *dst = (uint8_t *)w->pixels + y * w->row_pitch;
                size_t i = (size_t)y * view->width;
                for (unsigned x = 0; x < view->width; ++x, ++i, dst += view->bpb) {
                    if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT)
                        memcpy(dst + depth24, w->depth + 4 * i, depth24 ? 3 : 4);
                    if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) dst[depth24 ? 0 : 4] = w->stencil[i];
                }
            }
        }
        if (w->candidate.stream_gobs) {
            move_attachment_gobs((uint8_t *)view->storage + view->view_offset, w->pixels, w->row_pitch,
                                 (view->height + view->bh - 1) / view->bh, w->candidate.stream_gobs,
                                 w->candidate.stream_block_rows, nvn_gob_order(&view->desc), ATTACHMENT_STORE);
            continue;
        }
        if (!view->pitch && (view->depth ?
            nvn_swizzle(&view->desc, view->level, view->layer, w->pixels, w->row_pitch, view->storage) :
            nvn_swizzle_slice(&view->desc, view->level, view->layer, view->slice, w->pixels, w->row_pitch, view->storage))) {
            workspace->writeback = NULL;
            return "NVN draw resources: failed to reswizzle Vulkan output to guest storage";
        }
    }
    // Publish nothing on a partial writeback. No preparation or failed
    // submission can turn the retained bytes into a proof of reuse.
    for (const struct dm_m4_resource_writeback *w = workspace->writeback; w; w = w->next) {
        const struct attachment_candidate *c = &w->candidate;
        struct dm_m4_retained_attachment *entry = c->entry;
        if (!entry) continue;
        const int written = writeback_aspects(w, writes) != 0;
        // A retained stream's linear rows already describe the guest bytes:
        // the first loop committed the written planes, and other bytes stayed live.
        if (c->encoded_size && (written || c->match != SNAPSHOT_MATCH)) {
            uint8_t *snapshot = attachment_snapshot(entry, c->encoded_size);
            const uint8_t *guest = (const uint8_t *)w->view.storage + w->view.view_offset;
            if (entry->snapshot_size != c->encoded_size ||
                (!written && c->match == SNAPSHOT_DIFFERS) || !m4_bytes_equal(snapshot, guest, c->encoded_size)) {
                memcpy(snapshot, guest, c->encoded_size);
                entry->snapshot_size = c->encoded_size;
            }
        }
        entry->token = c->output_token;
        entry->reusable = 1;
    }
    workspace->writeback = NULL;
    return NULL;
}

void dm_m4_guest_resources_destroy(struct dm_m4_guest_resources *workspace) {
    for (struct dm_m4_resource_chunk *chunk = workspace->chunks, *next; chunk; chunk = next) {
        next = chunk->next;
        free(chunk);
    }
    for (unsigned i = 0; i < 2; ++i) {
        struct dm_m4_bindless_pool *pool = workspace->bindless[i];
        if (pool) free(pool->slots);
        free(pool);
    }
    for (unsigned i = 0; i < 1 + DM_M4_DRAW_TARGETS; ++i) {
        free(workspace->attachments[i]);
        free(workspace->stream_attachments[i]);
    }
    *workspace = (struct dm_m4_guest_resources){0};
}

static const char *gmem_views(const struct preparation *p, struct dm_m4_gmem_source *source) {
    if (!p->gmem) return NULL;
    size_t count = 0;
    for (const struct guest_range *r = p->ranges; r; r = r->next) {
        if (!r->layer_span || (!(p->gmem & 2u) && !r->write)) continue;
        const size_t layers = r->stride ? (r->size - r->layer_span) / r->stride + 1 : 1;
        if (layers > UINT32_MAX - count) return allocation_failed;
        count += layers;
    }
    if (!count) return NULL;
    if (count > SIZE_MAX / sizeof(struct dm_m4_gmem_view)) return allocation_failed;
    struct dm_m4_gmem_view *views = take(p->workspace, count * sizeof(*views));
    if (!views) return allocation_failed;
    size_t i = 0;
    for (const struct guest_range *r = p->ranges; r; r = r->next) {
        if (!r->layer_span || (!(p->gmem & 2u) && !r->write)) continue;
        if (!r->stride) views[i++] = (struct dm_m4_gmem_view){(const void *)r->at, r->size, r->write};
        else for (size_t at = 0; at <= r->size - r->layer_span; at += r->stride)
            views[i++] = (struct dm_m4_gmem_view){(const void *)(r->at + at), r->layer_span, r->write};
    }
    source->views = views;
    source->view_count = (uint32_t)count;
    return NULL;
}

const char *dm_m4_execute_nvn_compute(struct dm_m4_guest_resources *resources,
                                      const struct dm_m4_draw_state *state,
                                      const struct dm_m4_dispatch *dispatch,
                                      const struct dm_m4_draw_resolver *resolve) {
    if (!resources || !state || !dispatch || !resolve || !resolve->buffer || !dispatch->guest_device)
        return "compute dispatch is missing its workspace, guest device, state, or buffer resolver";
    if (!dispatch->groups[0] || !dispatch->groups[1] || !dispatch->groups[2]) return NULL;
    if (!(state->shader_mask & NVN_SHADER_STAGE_COMPUTE_BIT)) return "compute dispatch has no active shader";

    struct dm_m4_compute_session session;
    const char *error = dm_m4_replay_begin_compute(dispatch->guest_device, state->shader_keys[NVN_SHADER_STAGE_COMPUTE], &session);
    if (error) return error;
    struct dm_m4_compute_info info = {.cs = {.shader = session.cs},
        .gmem = {.visit = resolve->gmem_visit, .context = resolve->context}};
    memcpy(info.groups, dispatch->groups, sizeof(info.groups));
    dm_m4_draw_support_init(&info.support);
    reset_workspace(resources);
    struct preparation prep = {.workspace = resources, .state = state, .resolve = resolve,
        .groups = info.groups, .gmem = session.cs->gmem};
    error = stage_bindings(&prep, &info.cs, NVN_SHADER_STAGE_COMPUTE);
    if (!error) error = stage_direct_bindings(&prep, &info.cs);
    if (!error) error = gmem_views(&prep, &info.gmem);
    if (!error) {
        const char *step = NULL;
        if (dm_m4_compute_submit(session.draw, &info, &step) == VK_SUCCESS)
            error = dm_m4_guest_resources_writeback(resources, NULL);
        else
            error = step ? step : "Vulkan compute submission failed";
    }
    resources->writeback = NULL;
    dm_m4_replay_end();
    return error;
}
