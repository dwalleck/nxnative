// Diagnostic only: correlate a selected color draw with an accepted WSI upload.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "m4_overlay.h"
#include "../../gfx/tex/nvn_deswizzle.h"
#include "m4_shader.h"

// Retain only the last selected pane and one verified blit per guest frame.
// A handheld or 2560x1440 plane fits, but arbitrary render targets need not.
enum { MAX_PLANE_BYTES = 16 * 1024 * 1024, MAX_INPUTS = 8 };
enum image_order { UNSUPPORTED, RGBA, BGRA, RGB11F };
enum match_status { NO_DRAW, DRAW_FAILED, TARGET_UNAVAILABLE, UNLINKED_TARGET,
                    AMBIGUOUS_WINDOW, SELECTOR_UNAVAILABLE, MATCHED, PAIRED, WRITE_FAILED };
enum evidence_link { LINK_NONE, LINK_DIRECT, LINK_BLIT };

struct input_selector { unsigned binding, count, offsets[MAX_INPUTS]; };
struct input_sample { unsigned count; float values[MAX_INPUTS]; };
struct sample { struct input_sample ps, vs; };
struct image { uint8_t *data; size_t capacity; VkExtent2D size; enum image_order order; };
struct record {
    uint64_t present, guest_frame;
    enum match_status status;
    enum evidence_link link;
    int fade_blend;
    struct sample inputs;
    int black;
};

struct raster_bounds { double left, top, right, bottom; };

struct dm_m4_overlay {
    wchar_t directory[MAX_PATH], arm_path[MAX_PATH];
    unsigned window;
    int configured, armed, previous_black, done;
    uint64_t presents;
    uint8_t vs[8], ps[8], blit[8];
    int has_blit;
    unsigned blit_binding;
    struct input_selector ps_inputs, vs_inputs;
    struct sample inputs;
    const void *target, *sink;
    struct raster_bounds blit_bounds;
    enum match_status candidate;
    int fade_blend;
    struct image before, after, output;
    // Keep metadata, not the old frame's pixels, while waiting for the next present.
    struct record first;
    int have_pair;
    enum image_order pair_order;
    VkExtent2D pair_size;
    size_t before_nonblack, after_nonblack;
};

static int read_text(const wchar_t *name, wchar_t *out, DWORD capacity) {
    DWORD count = GetEnvironmentVariableW(name, out, capacity);
    return count && count < capacity;
}

static int hex_digit(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

static int parse_key(const wchar_t *text, uint8_t out[8]) {
    for (unsigned i = 0; i < 8; ++i) {
        const int high = hex_digit(*text++);
        if (high < 0) return 0;
        const int low = hex_digit(*text++);
        if (low < 0) return 0;
        out[i] = (uint8_t)(16 * high + low);
    }
    return 1;
}

static int parse_uint(const wchar_t **text, unsigned *value) {
    const wchar_t *p = *text;
    if (*p < L'0' || *p > L'9') return 0;
    unsigned n = 0;
    do {
        const unsigned digit = (unsigned)(*p++ - L'0');
        if (n > (UINT32_MAX - digit) / 10u) return 0;
        n = n * 10u + digit;
    } while (*p >= L'0' && *p <= L'9');
    *text = p;
    *value = n;
    return 1;
}

static int configure_inputs(const wchar_t *name, struct input_selector *selector) {
    wchar_t text[128];
    const DWORD count = GetEnvironmentVariableW(name, text, ARRAYSIZE(text));
    if (!count) return 1; // Images alone do not require a known opacity recipe.
    if (count >= ARRAYSIZE(text)) return 0;
    const wchar_t *p = text;
    if (!parse_uint(&p, &selector->binding) || *p++ != L':') return 0;
    for (;;) {
        if (selector->count == MAX_INPUTS || !parse_uint(&p, &selector->offsets[selector->count])) return 0;
        ++selector->count;
        if (!*p) return 1;
        if (*p++ != L',') return 0;
    }
}

static int configure(struct dm_m4_overlay *c) {
    wchar_t vs[17], ps[17], blit[32];
    if (!read_text(L"DM_OVERLAY_VS", vs, ARRAYSIZE(vs)) || !parse_key(vs, c->vs) || vs[16] ||
        !read_text(L"DM_OVERLAY_PS", ps, ARRAYSIZE(ps)) || !parse_key(ps, c->ps) || ps[16] ||
        !configure_inputs(L"DM_OVERLAY_PS_INPUTS", &c->ps_inputs) ||
        !configure_inputs(L"DM_OVERLAY_VS_INPUTS", &c->vs_inputs)) return 0;
    const DWORD count = GetEnvironmentVariableW(L"DM_OVERLAY_BLIT", blit, ARRAYSIZE(blit));
    if (count) {
        if (count >= ARRAYSIZE(blit) || !parse_key(blit, c->blit) || blit[16] != L':') return 0;
        const wchar_t *binding = blit + 17;
        if (!parse_uint(&binding, &c->blit_binding) || *binding) return 0;
        c->has_blit = 1;
    }
    return 1;
}

struct dm_m4_overlay *dm_m4_overlay_open(void) {
    wchar_t value[2], dir[MAX_PATH], absolute[MAX_PATH];
    if (!read_text(L"DM_OVERLAY_CAPTURE", value, ARRAYSIZE(value)) || value[0] != L'1') return NULL;
    DWORD length = read_text(L"DM_CAPTURE_DIR", dir, ARRAYSIZE(dir)) ?
                   GetFullPathNameW(dir, ARRAYSIZE(absolute), absolute, NULL) : 0;
    if (length && length < ARRAYSIZE(absolute) - 75) {
        const DWORD attrs = GetFileAttributesW(absolute);
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            static volatile LONG ids;
            struct dm_m4_overlay *c = calloc(1, sizeof(*c));
            if (!c) {
                fprintf(stderr, "[m4 overlay] cannot allocate capture state\n");
                return NULL;
            }
            if (absolute[length - 1] != L'\\') absolute[length++] = L'\\';
            absolute[length] = L'\0';
            memcpy(c->directory, absolute, (length + 1) * sizeof(*absolute));
            swprintf_s(c->arm_path, ARRAYSIZE(c->arm_path), L"%lsoverlay.arm", c->directory);
            c->window = (unsigned)InterlockedIncrement(&ids);
            c->configured = configure(c);
            c->previous_black = -1;
            return c;
        }
    }
    fprintf(stderr, "[m4 overlay] DM_CAPTURE_DIR must be an existing directory with room for overlay filenames\n");
    return NULL;
}

void dm_m4_overlay_close(struct dm_m4_overlay *c) {
    if (!c) return;
    free(c->before.data);
    free(c->after.data);
    free(c->output.data);
    free(c);
}

void dm_m4_overlay_discard(struct dm_m4_overlay *c) {
    c->candidate = NO_DRAW;
    c->target = c->sink = NULL;
    c->inputs = (struct sample){0};
}

static int armed(struct dm_m4_overlay *c) {
    if (!c->armed) {
        const DWORD attr = GetFileAttributesW(c->arm_path);
        c->armed = attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
    }
    return c->armed;
}

enum dm_m4_overlay_role dm_m4_overlay_select(struct dm_m4_overlay *c,
                                              const struct dm_m4_replay_keys *keys) {
    if (!c->configured || c->done || !keys->vs || !keys->ps) return DM_M4_OVERLAY_NONE;
    if (!memcmp(keys->ps, c->ps, sizeof(c->ps)) && !memcmp(keys->vs, c->vs, sizeof(c->vs)))
        return armed(c) ? DM_M4_OVERLAY_PANE : DM_M4_OVERLAY_NONE;
    if (c->candidate == MATCHED && c->has_blit && !memcmp(keys->ps, c->blit, sizeof(c->blit)))
        return DM_M4_OVERLAY_BLIT;
    return DM_M4_OVERLAY_NONE;
}

static enum image_order order(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: return RGBA;
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: return BGRA;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return RGB11F;
    default: return UNSUPPORTED;
    }
}

static const char *order_name(enum image_order encoding) {
    switch (encoding) {
    case RGBA: return "RGBA8";
    case BGRA: return "BGRA8";
    case RGB11F: return "R11G11B10F";
    default: return "unavailable";
    }
}

static int snapshot(struct image *image, const struct dm_m4_draw_attachment *attachment) {
    const enum image_order encoding = order(attachment->format);
    const uint32_t width = attachment->width, height = attachment->height;
    const size_t row = (size_t)width * 4;
    size_t pitch = attachment->pixels.row_pitch;
    if (!pitch) pitch = row;
    if (!encoding || !row || !height || height > MAX_PLANE_BYTES / row || pitch < row ||
        !attachment->pixels.data || attachment->pixels.size < row ||
        (height > 1 && pitch > (attachment->pixels.size - row) / (height - 1))) return 0;
    const size_t size = row * height;
    if (image->capacity < size) {
        uint8_t *data = realloc(image->data, size);
        if (!data) return 0;
        image->data = data;
        image->capacity = size;
    }
    const uint8_t *src = attachment->pixels.data;
    if (pitch == row) memcpy(image->data, src, size);
    else for (uint32_t y = 0; y < height; ++y)
        memcpy(image->data + y * row, src + y * pitch, row);
    image->size = (VkExtent2D){width, height};
    image->order = encoding;
    return 1;
}

static const struct dm_m4_draw_binding *binding(const struct dm_m4_draw_stage *stage, unsigned number) {
    if (!stage->shader || !stage->shader->bindings || !stage->bindings) return NULL;
    for (unsigned i = 0; i < stage->shader->binding_count; ++i)
        if (stage->shader->bindings[i].binding == number) return &stage->bindings[i];
    return NULL;
}

static struct input_sample read_inputs(const struct dm_m4_draw_stage *stage, const struct input_selector *selector) {
    struct input_sample result = {0};
    const struct dm_m4_draw_binding *input = selector->count ? binding(stage, selector->binding) : NULL;
    if (!input || !input->buffer.data || input->buffer.size < sizeof(float)) return result;
    const uint8_t *base = input->buffer.data;
    for (unsigned i = 0; i < selector->count; ++i) {
        if (selector->offsets[i] > input->buffer.size - sizeof(float)) return result;
        memcpy(&result.values[i], base + selector->offsets[i], sizeof(float));
        if (!isfinite(result.values[i])) return result;
    }
    result.count = selector->count;
    return result;
}

static int same_sample(const struct dm_m4_overlay *c, const struct dm_m4_draw_sampled_image *sample) {
    const struct image *after = &c->after;
    if (!sample || !sample->upload || sample->array_layers || sample->depth || sample->cube ||
        sample->level_count != 1 || sample->width != after->size.width || sample->height != after->size.height ||
        order(sample->format) != after->order ||
        (sample->components.r && sample->components.r != VK_COMPONENT_SWIZZLE_R) ||
        (sample->components.g && sample->components.g != VK_COMPONENT_SWIZZLE_G) ||
        (sample->components.b && sample->components.b != VK_COMPONENT_SWIZZLE_B)) return 0;
    const struct dm_m4_draw_sample_upload *upload = sample->upload;
    const struct dm_m4_draw_sample_layout *layout = &upload->layout;
    if (upload->encoded != c->target || layout->base_level || layout->base_layer) return 0;
    const uint32_t row = after->size.width * 4, height = after->size.height;
    const uint8_t *raw = upload->encoded;
    if (layout->pitch) {
        if (layout->pitch < row || upload->encoded_size < row ||
            (height > 1 && layout->pitch > (upload->encoded_size - row) / (height - 1))) return 0;
        if (layout->pitch == row) return !memcmp(raw, after->data, (size_t)row * height);
        for (uint32_t y = 0; y < height; ++y)
            if (memcmp(raw + y * layout->pitch, after->data + (size_t)y * row, row)) return 0;
    } else {
        unsigned th, td;
        nvn_tex_level_tiles(&layout->desc, 0, &th, &td);
        const uint32_t pitch = nvn_tex_level_gobs(&layout->desc, 0) * 64u;
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < row; x += 16) {
                const size_t n = min(row - x, 16u);
                const uint64_t at = nvn_bl_offset(x, y, 0, pitch, height, th, td, layout->desc.gob_order);
                if (at > upload->encoded_size || n > upload->encoded_size - at ||
                    memcmp(raw + at, after->data + (size_t)y * row + x, n)) return 0;
            }
        }
    }
    return 1;
}

static struct raster_bounds raster_bounds(const struct dm_m4_draw_info *info) {
    const VkViewport v = info->viewport;
    const VkRect2D s = info->scissor;
    const double x1 = (double)v.x + v.width, y1 = (double)v.y + v.height;
    return (struct raster_bounds){
        fmax(v.x, s.offset.x), fmax(fmin(v.y, y1), s.offset.y),
        fmin(x1, fmin(info->extent.width, (double)s.offset.x + s.extent.width)),
        fmin(fmax(v.y, y1), fmin(info->extent.height, (double)s.offset.y + s.extent.height))};
}

static int covers(struct raster_bounds bounds, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
    return bounds.left <= x && bounds.top <= y &&
           bounds.right >= (double)x + width && bounds.bottom >= (double)y + height;
}

static const VkPipelineColorBlendAttachmentState *rgb_output(const struct dm_m4_draw_info *info) {
    const VkPipelineColorBlendStateCreateInfo *blend = info->pipeline.blend;
    const VkPipelineDepthStencilStateCreateInfo *depth = info->pipeline.depth_stencil;
    const VkPipelineMultisampleStateCreateInfo *samples = info->pipeline.multisample;
    const VkColorComponentFlags rgb = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    if (info->pipeline.viewport_count > 1 ||
        !blend || !blend->attachmentCount || !blend->pAttachments || blend->logicOpEnable ||
        (blend->pAttachments[0].colorWriteMask & rgb) != rgb ||
        (depth && (depth->depthBoundsTestEnable || depth->stencilTestEnable ||
                   (depth->depthTestEnable && depth->depthCompareOp != VK_COMPARE_OP_ALWAYS))) ||
        (samples && samples->alphaToCoverageEnable)) return NULL;
    return &blend->pAttachments[0];
}

int dm_m4_overlay_draw_begin(struct dm_m4_overlay *c, enum dm_m4_overlay_role role,
                             const struct dm_m4_draw_info *info, const void *storage) {
    if (role == DM_M4_OVERLAY_PANE) {
        dm_m4_overlay_discard(c);
        c->candidate = TARGET_UNAVAILABLE;
        if (!storage || !info->color_count || !snapshot(&c->before, &info->colors[0])) return 0;
        c->target = storage;
        c->inputs.ps = read_inputs(&info->ps, &c->ps_inputs);
        c->inputs.vs = read_inputs(&info->vs, &c->vs_inputs);
        const VkPipelineColorBlendAttachmentState *blend = rgb_output(info);
        c->fade_blend = covers(raster_bounds(info), 0, 0, info->colors[0].width, info->colors[0].height) &&
            blend && blend->blendEnable && blend->colorBlendOp == VK_BLEND_OP_ADD &&
            blend->srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA &&
            blend->dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        c->candidate = DRAW_FAILED;
        return 1;
    }
    if (role == DM_M4_OVERLAY_BLIT) {
        // A later selected blit supersedes the prior bridge, even if it no longer
        // samples this pane. Do not inherit an earlier draw's sink.
        c->sink = NULL;
        const struct dm_m4_draw_binding *src = binding(&info->ps, c->blit_binding);
        if (!storage || !info->color_count || !src) return 0;
        const VkPipelineColorBlendAttachmentState *blend = rgb_output(info);
        if (!blend || (blend->blendEnable && !(blend->colorBlendOp == VK_BLEND_OP_ADD &&
            blend->srcColorBlendFactor == VK_BLEND_FACTOR_ONE && blend->dstColorBlendFactor == VK_BLEND_FACTOR_ZERO)) ||
            !same_sample(c, src->texture)) return 0;
        // A handheld blit may fill the presented crop of a larger swapchain texture.
        // Its required extent is known at present, not when the draw is submitted.
        c->blit_bounds = raster_bounds(info);
        c->sink = storage;
        return 1;
    }
    return 0;
}

void dm_m4_overlay_draw_end(struct dm_m4_overlay *c, enum dm_m4_overlay_role role,
                             const struct dm_m4_draw_info *info, int success) {
    if (role == DM_M4_OVERLAY_PANE) {
        if (success) c->candidate = snapshot(&c->after, &info->colors[0]) ? MATCHED : TARGET_UNAVAILABLE;
    } else if (role == DM_M4_OVERLAY_BLIT) {
        if (!success || !snapshot(&c->output, &info->colors[0])) c->sink = NULL;
    }
}

static uint32_t pixel_word(const uint8_t *p) {
    uint32_t word;
    memcpy(&word, p, sizeof(word));
    return word;
}

static int rgb_nonzero(const uint8_t *pixels) { return (pixel_word(pixels) & 0x00ffffffu) != 0; }

static int same_upload(const struct image *image, const struct dm_m4_overlay_frame *frame,
                        const uint8_t *pixels, VkExtent2D size, int bgra) {
    if (image->order != (bgra ? BGRA : RGBA) || frame->x > image->size.width || frame->y > image->size.height ||
        size.width > image->size.width - frame->x || size.height > image->size.height - frame->y) return 0;
    for (uint32_t y = 0; y < size.height; ++y) {
        const uint8_t *row = image->data + ((size_t)(frame->y + y) * image->size.width + frame->x) * 4;
        for (uint32_t x = 0; x < size.width; ++x, row += 4, pixels += 4)
            if ((pixel_word(row) ^ pixel_word(pixels)) & 0x00ffffffu) return 0;
    }
    return 1;
}

static enum match_status match(const struct dm_m4_overlay *c, const struct dm_m4_overlay_frame *frame,
                               const void *pixels, VkExtent2D size, int bgra, enum evidence_link *link) {
    if (frame->ambiguous) return AMBIGUOUS_WINDOW;
    if (!c->configured) return SELECTOR_UNAVAILABLE;
    if (c->candidate != MATCHED) return c->candidate;
    if (frame->storage == c->target && same_upload(&c->after, frame, pixels, size, bgra)) *link = LINK_DIRECT;
    else if (c->sink && frame->storage == c->sink &&
             covers(c->blit_bounds, frame->x, frame->y, size.width, size.height) &&
             same_upload(&c->output, frame, pixels, size, bgra)) *link = LINK_BLIT;
    if (*link != LINK_NONE) return MATCHED;
    return UNLINKED_TARGET;
}

// The source is the mapped RGB accepted by WSI. A nearest blit can omit
// nonblack source pixels when shrinking. At an exact tie, require every
// permitted nearest texel to agree (otherwise report unavailable).
static int black_upload(const uint8_t *pixels, VkExtent2D src, VkExtent2D dst) {
    const size_t count = (size_t)src.width * src.height;
    size_t i = 0;
    while (i < count && !rgb_nonzero(pixels + i * 4)) ++i;
    if (i == count) return 1;
    if (src.width <= dst.width && src.height <= dst.height) return 0;
    int uncertain = 0;
    const uint64_t dx = 2ull * dst.width, dy = 2ull * dst.height;
    for (uint32_t y = 0; y < dst.height; ++y) {
        const uint64_t sy = (2ull * y + 1) * src.height;
        const uint32_t y1 = (uint32_t)(sy / dy), y0 = y1 - (sy % dy == 0 && y1 > 0);
        for (uint32_t x = 0; x < dst.width; ++x) {
            const uint64_t sx = (2ull * x + 1) * src.width;
            const uint32_t x1 = (uint32_t)(sx / dx), x0 = x1 - (sx % dx == 0 && x1 > 0);
            int all = 1, any = 0;
            for (uint32_t v = y0; v <= y1; ++v) for (uint32_t u = x0; u <= x1; ++u) {
                const int nonzero = rgb_nonzero(pixels + ((size_t)v * src.width + u) * 4);
                all &= nonzero;
                any |= nonzero;
            }
            if (all) return 0;
            uncertain |= any;
        }
    }
    return uncertain ? -1 : 1;
}

static size_t nonblack_count(const struct image *image) {
    const uint32_t mask = image->order == RGB11F ? UINT32_MAX : 0x00ffffffu;
    const size_t count = (size_t)image->size.width * image->size.height;
    size_t nonblack = 0;
    for (size_t i = 0; i < count; ++i) nonblack += (pixel_word(image->data + i * 4) & mask) != 0;
    return nonblack;
}

static uint8_t float_rgb8(unsigned bits, unsigned fraction) {
    const unsigned exponent = bits >> fraction;
    if (exponent >= 15) return 255; // Clip highlights; raw nonblack counts remain exact.
    const unsigned mantissa = bits & ((1u << fraction) - 1);
    const float value = exponent ? ldexpf((float)((1u << fraction) | mantissa), (int)exponent - 15 - (int)fraction) :
                                   ldexpf((float)mantissa, -14 - (int)fraction);
    return (uint8_t)(value * 255.0f + 0.5f);
}

static int path_for(wchar_t path[MAX_PATH], const struct dm_m4_overlay *c, const wchar_t *suffix) {
    return swprintf_s(path, MAX_PATH, L"%lsoverlay-w%u-p%llu%ls", c->directory, c->window,
                      (unsigned long long)c->first.present, suffix) > 0;
}

static FILE *open_temp(const wchar_t *path, wchar_t temp[MAX_PATH]) {
    FILE *file = NULL;
    if (swprintf_s(temp, MAX_PATH, L"%ls.tmp", path) > 0 && !_wfopen_s(&file, temp, L"wb")) return file;
    return NULL;
}

static int publish_file(FILE *file, const wchar_t *temp, const wchar_t *path) {
    const int ok = !ferror(file);
    if (!fclose(file) && ok && MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING)) return 1;
    DeleteFileW(temp);
    return 0;
}

static int write_image(const struct dm_m4_overlay *c, const struct image *image,
                       const wchar_t *suffix, const char *label) {
    wchar_t path[MAX_PATH], temp[MAX_PATH];
    if (!path_for(path, c, suffix)) return 0;
    FILE *file = open_temp(path, temp);
    if (!file) return 0;
    fprintf(file, "P6\n# Vulkan present %llu, %s overlay; %s (RGB8 preview)\n%u %u\n255\n",
            (unsigned long long)c->first.present, label, order_name(image->order), image->size.width, image->size.height);
    unsigned char rgb[3 * 4096];
    const uint8_t *p = image->data;
    for (size_t left = (size_t)image->size.width * image->size.height; left;) {
        const size_t n = min(left, sizeof(rgb) / 3);
        for (size_t i = 0; i < n; ++i, p += 4) {
            if (image->order == RGB11F) {
                const uint32_t word = pixel_word(p);
                rgb[3 * i] = float_rgb8(word & 0x7ffu, 6);
                rgb[3 * i + 1] = float_rgb8((word >> 11) & 0x7ffu, 6);
                rgb[3 * i + 2] = float_rgb8(word >> 22, 5);
            } else {
                const unsigned r = image->order == BGRA ? 2 : 0;
                rgb[3 * i] = p[r];
                rgb[3 * i + 1] = p[1];
                rgb[3 * i + 2] = p[2 - r];
            }
        }
        if (fwrite(rgb, 3, n, file) != n) { fclose(file); DeleteFileW(temp); return 0; }
        left -= n;
    }
    return publish_file(file, temp, path);
}

static const char *status_name(enum match_status status) {
    switch (status) {
    case NO_DRAW: return "no_matching_draw";
    case DRAW_FAILED: return "draw_failed";
    case TARGET_UNAVAILABLE: return "target_unavailable";
    case UNLINKED_TARGET: return "unlinked_target";
    case AMBIGUOUS_WINDOW: return "ambiguous_window";
    case SELECTOR_UNAVAILABLE: return "selector_unavailable";
    case MATCHED: return "matched";
    case PAIRED: return "paired";
    case WRITE_FAILED: return "capture_write_failed";
    }
    return "unavailable";
}

static const char *black_name(int black) { return black < 0 ? "unavailable" : black ? "1" : "0"; }

static const char *link_name(enum evidence_link link) {
    return link == LINK_DIRECT ? "direct" : link == LINK_BLIT ? "blit" : "unavailable";
}

static const char *opacity_scope(const struct record *record) {
    if (!record || !record->inputs.ps.count || !record->inputs.vs.count) return "unavailable";
    return record->fade_blend ? "source_alpha_blend" : "fragment_alpha_only";
}

static void write_key(FILE *file, const char *name, const uint8_t key[8]) {
    fprintf(file, "%s\t", name);
    for (unsigned i = 0; i < 8; ++i) fprintf(file, "%02x", (unsigned)key[i]);
    fputc('\n', file);
}

static void write_inputs(FILE *file, const char *key, const struct input_sample *sample) {
    fprintf(file, "%s\t", key);
    if (!sample || !sample->count) fputs("unavailable", file);
    else for (unsigned i = 0; i < sample->count; ++i)
        fprintf(file, "%s%.9g", i ? "," : "", (double)sample->values[i]);
    fputc('\n', file);
}

static int write_manifest(const struct dm_m4_overlay *c, const struct record *next) {
    wchar_t path[MAX_PATH], temp[MAX_PATH];
    if (!path_for(path, c, L".tsv")) return 0;
    FILE *file = open_temp(path, temp);
    if (!file) return 0;
    const struct record *first = &c->first;
    const unsigned long long present = (unsigned long long)first->present;
    fprintf(file, "window\t%u\npresent\t%llu\nguest_frame\t%llu\nblack\t1\nstatus\t%s\n",
            c->window, present, (unsigned long long)first->guest_frame, status_name(first->status));
    fprintf(file, "link\t%s\nopacity_scope\t%s\n", link_name(first->link), opacity_scope(first));
    if (c->configured) {
        write_key(file, "vs", c->vs);
        write_key(file, "ps", c->ps);
        if (c->has_blit) write_key(file, "blit_ps", c->blit);
    }
    write_inputs(file, "ps_inputs", &first->inputs.ps);
    write_inputs(file, "vs_inputs", &first->inputs.vs);
    if (c->have_pair) {
        fprintf(file, "before\toverlay-w%u-p%llu-before.ppm\nafter\toverlay-w%u-p%llu-after.ppm\n"
                "attachment_format\t%s\nattachment_width\t%u\nattachment_height\t%u\n"
                "before_nonblack_pixels\t%zu\nafter_nonblack_pixels\t%zu\n",
                c->window, present, c->window, present, order_name(c->pair_order), c->pair_size.width,
                c->pair_size.height, c->before_nonblack, c->after_nonblack);
    } else fputs("before\tunavailable\nafter\tunavailable\n", file);
    if (next) {
        fprintf(file, "next_present\t%llu\nnext_guest_frame\t%llu\nnext_status\t%s\nnext_black\t%s\n",
                (unsigned long long)next->present, (unsigned long long)next->guest_frame,
                status_name(next->status), black_name(next->black));
    } else fputs("next_present\tunavailable\nnext_guest_frame\tunavailable\nnext_status\tpending\nnext_black\tunavailable\n", file);
    fprintf(file, "next_link\t%s\nnext_opacity_scope\t%s\n",
            link_name(next ? next->link : LINK_NONE), opacity_scope(next));
    write_inputs(file, "next_ps_inputs", next ? &next->inputs.ps : NULL);
    write_inputs(file, "next_vs_inputs", next ? &next->inputs.vs : NULL);
    return publish_file(file, temp, path);
}

static void save_pair(struct dm_m4_overlay *c) {
    if (!write_image(c, &c->before, L"-before.ppm", "before") ||
        !write_image(c, &c->after, L"-after.ppm", "after")) {
        wchar_t path[MAX_PATH];
        if (path_for(path, c, L"-before.ppm")) DeleteFileW(path);
        if (path_for(path, c, L"-after.ppm")) DeleteFileW(path);
        c->first.status = WRITE_FAILED;
        return;
    }
    c->first.status = PAIRED;
    c->have_pair = 1;
    c->pair_order = c->before.order;
    c->pair_size = c->before.size;
    c->before_nonblack = nonblack_count(&c->before);
    c->after_nonblack = nonblack_count(&c->after);
}

void dm_m4_overlay_present(struct dm_m4_overlay *c, const struct dm_m4_overlay_frame *frame,
                           const void *pixels, VkExtent2D source, VkExtent2D surface, int bgra) {
    if (c->done) return;
    const uint64_t present = ++c->presents;
    if (!armed(c)) { dm_m4_overlay_discard(c); return; }
    const int black = black_upload(pixels, source, surface);
    printf("[m4 overlay] window=%u present=%llu guest_frame=%llu black=%s armed=1\n",
           c->window, (unsigned long long)present, (unsigned long long)frame->guest_frame, black_name(black));
    if (c->first.present || (black == 1 && c->previous_black == 0)) {
        struct record observed = {.present = present, .guest_frame = frame->guest_frame, .black = black};
        observed.status = match(c, frame, pixels, source, bgra, &observed.link);
        if (observed.status == MATCHED) {
            observed.inputs = c->inputs;
            observed.fade_blend = c->fade_blend;
        }
        if (!c->first.present) {
            c->first = observed;
            if (observed.status == MATCHED) save_pair(c);
            if (!write_manifest(c, NULL)) fprintf(stderr, "[m4 overlay] could not publish first-present manifest\n");
        } else {
            if (write_manifest(c, &observed)) {
                printf("[m4 overlay] complete window=%u present=%llu next_present=%llu\n", c->window,
                       (unsigned long long)c->first.present, (unsigned long long)present);
            } else fprintf(stderr, "[m4 overlay] could not publish next-present manifest\n");
            c->done = 1;
            free(c->before.data); c->before = (struct image){0};
            free(c->after.data); c->after = (struct image){0};
            free(c->output.data); c->output = (struct image){0};
        }
    }
    c->previous_black = black;
    dm_m4_overlay_discard(c);
    fflush(stdout);
}
