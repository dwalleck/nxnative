// Address only the copy region's blocks. Mips, layers and 3D GOB slices
// come from the same layout used for texture initialization and sampling.
struct texture_transfer {
    uint8_t *storage;
    size_t size, base, layer_pitch, image_bytes;
    uint32_t x, y, z, row, rows, images, pitch, mip_rows;
    unsigned block_width, block_height, block_bytes, tile_h, tile_d;
    NVNformat format;
    int linear, volume, gob_order;
};

static struct texture_transfer texture_transfer_open(const struct headless_texture *t, const struct texture_view *v,
        struct texture_region r, int write, const char *operation) {
    const nvn_texdesc *d = &t->desc;
    const int buffer = d->target == NVN_TEXTURE_TARGET_BUFFER;
    const int linear = buffer || (d->flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET));
    const int volume = d->target == NVN_TEXTURE_TARGET_3D;
    if (d->samples > 1 || d->gob_order > 1)
        dm_fail("%s: unsupported texture storage layout", operation);
    if (v && (v->set & ~(VIEW_LEVELS | VIEW_LAYERS | VIEW_FORMAT | VIEW_TARGET | VIEW_SWIZZLE | VIEW_DEPTH_STENCIL)))
        dm_fail("%s: invalid texture view", operation);
    const NVNformat format = (NVNformat)(v && (v->set & VIEW_FORMAT) ? v->format : d->format);
    int bw, bh, vw, vh;
    const int bpb = nvn_fmt_bpb(d->format, &bw, &bh);
    if (!bpb) dm_fail("%s: unsupported format", operation);
    if (nvn_fmt_bpb(format, &vw, &vh) != bpb || vw != bw || vh != bh)
        dm_fail("%s: view changes the block geometry", operation);
    if (r.x % bw || r.y % bh || (r.w % bw && r.w != r.width - r.x) || (r.h % bh && r.h != r.height - r.y))
        dm_fail("%s: region is not block aligned", operation);
    struct texture_transfer s = {
        .storage = pool_storage(t->pool, t->offset, (size_t)t->layout.size, t->layout.align, write),
        .size = (size_t)t->layout.size, .format = format, .volume = volume, .linear = linear,
        .x = r.x / bw * (unsigned)bpb, .y = r.y / bh, .z = volume ? r.z : 0,
        .row = ((r.w + bw - 1) / bw) * (unsigned)bpb, .rows = (r.h + bh - 1) / bh, .images = r.images,
        .block_width = bw, .block_height = bh, .block_bytes = bpb, .gob_order = nvn_gob_order(d),
    };
    if (!s.storage) dm_fail("%s: invalid texture storage", operation);
    if (linear) {
        if ((!buffer && d->target != NVN_TEXTURE_TARGET_2D) || r.level || r.layer || r.z || r.images != 1 || bw != 1 || bh != 1)
            dm_fail("%s: unsupported linear subresource", operation);
        const size_t pitch = (size_t)t->layout.layer_size / r.height;
        if (pitch > UINT32_MAX || t->layout.layer_size % r.height || s.x + s.row > pitch ||
            (uint64_t)(s.y + s.rows) * pitch > s.size)
            dm_fail("%s: invalid linear pitch", operation);
        s.pitch = (uint32_t)pitch;
    } else {
        uint32_t row_bytes, slices, bytes_per_block;
        uint64_t base;
        const unsigned first_layer = volume ? 0 : r.layer + r.z;
        if (nvn_deswizzle_geom(d, r.level, first_layer, &row_bytes, &s.mip_rows, &slices,
                               &base, &s.tile_h, &s.tile_d, &bytes_per_block) || bytes_per_block != s.block_bytes)
            dm_fail("%s: unsupported block-linear subresource", operation);
        s.pitch = nvn_tex_level_gobs(d, r.level) * 64;
        s.layer_pitch = volume ? 0 : (size_t)t->layout.layer_size;
        if (s.pitch < row_bytes || s.x + s.row > row_bytes || s.y + s.rows > s.mip_rows ||
            s.z >= slices || (volume && s.images > slices - s.z) || base >= s.size)
            dm_fail("%s: invalid block-linear geometry", operation);
        s.base = (size_t)base;
    }
    if (s.rows > SIZE_MAX / s.row || s.images > SIZE_MAX / ((size_t)s.rows * s.row))
        dm_fail("%s: texture copy span overflows", operation);
    s.image_bytes = (size_t)s.rows * s.row;
    return s;
}

struct texture_span { uint8_t *data; uint32_t bytes; };
static struct texture_span texture_transfer_span(const struct texture_transfer *s, unsigned image, unsigned row,
        uint32_t x, uint32_t remaining, const char *operation) {
    const uint32_t bx = s->x + x, by = s->y + row;
    uint64_t at;
    if (s->linear) {
        at = (uint64_t)image * s->layer_pitch + (uint64_t)by * s->pitch + bx;
    } else {
        at = s->base + (uint64_t)image * s->layer_pitch +
            nvn_bl_offset(bx, by, s->volume ? s->z + image : 0, s->pitch, s->mip_rows,
                          s->tile_h, s->tile_d, s->gob_order);
        const uint32_t contiguous = 16 - (bx & 15);
        if (remaining > contiguous) remaining = contiguous;
    }
    if (at >= s->size || remaining > s->size - at)
        dm_fail("%s: invalid texture byte offset", operation);
    return (struct texture_span){s->storage + (size_t)at, remaining};
}

static int texture_spans_overlap(const void *a, size_t a_size, const void *b, size_t b_size) {
    const uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < a_size : x - y < b_size;
}

static void texture_transfer_linear(const struct texture_transfer *s, void *bytes, size_t row_pitch,
        size_t image_pitch, int upload, const char *operation) {
    for (unsigned z = 0; z < s->images; ++z) {
        uint8_t *row = (uint8_t *)bytes + z * image_pitch;
        for (unsigned y = 0; y < s->rows; ++y) {
            for (uint32_t x = 0; x < s->row;) {
                const struct texture_span span = texture_transfer_span(s, z, y, x, s->row - x, operation);
                if (upload) memcpy(span.data, row + x, span.bytes);
                else memcpy(row + x, span.data, span.bytes);
                x += span.bytes;
            }
            if (y + 1 != s->rows) row += row_pitch;
        }
    }
}

static void texture_write_region(const struct headless_texture *t, const struct texture_view *v,
        struct texture_region r, const uint8_t *src, size_t row_stride, size_t image_stride, const char *operation) {
    const struct texture_transfer d = texture_transfer_open(t, v, r, 1, operation);
    const size_t row_pitch = row_stride ? row_stride : d.row;
    if (row_pitch < d.row || d.rows > SIZE_MAX / row_pitch)
        dm_fail("%s: invalid source row stride", operation);
    const size_t image_bytes = (d.rows - 1) * row_pitch + d.row;
    const size_t image_pitch = image_stride ? image_stride : d.rows * row_pitch;
    if (d.images > 1 && (image_pitch < image_bytes || d.images - 1 > (SIZE_MAX - image_bytes) / image_pitch))
        dm_fail("%s: invalid source image stride", operation);
    const size_t source_size = (d.images - 1) * image_pitch + image_bytes;
    if (!dm_guest_range(src, source_size, 0)) dm_fail("%s: invalid source span", operation);
    if (texture_spans_overlap(src, source_size, d.storage, d.size))
        dm_fail("%s: overlapping source and texture storage", operation);
    texture_transfer_linear(&d, (void *)src, row_pitch, image_pitch, 1, operation);
}

static uint32_t texture_nearest(uint32_t out, uint32_t source, uint32_t dest, int mirror) {
    if (mirror) out = dest - 1 - out;
    return source == dest ? out : (uint32_t)(((uint64_t)out * 2 + 1) * source / ((uint64_t)dest * 2));
}

static void texture_copy_raw(struct texture_transfer s, const struct texture_transfer *d,
        uint32_t flags, const char *operation) {
    void *snapshot = NULL;
    if (texture_spans_overlap(s.storage, s.size, d->storage, d->size)) {
        const size_t size = s.image_bytes * s.images;
        snapshot = malloc(size);
        if (!snapshot) dm_fail("%s: source snapshot allocation failed", operation);
        texture_transfer_linear(&s, snapshot, s.row, s.image_bytes, 0, operation);
        s.storage = snapshot;
        s.size = size;
        s.x = s.y = s.z = 0;
        s.base = 0;
        s.linear = 1;
        s.layer_pitch = s.image_bytes;
        s.pitch = s.row;
    }
    const int mirror_x = flags & NVN_COPY_FLAGS_MIRROR_X_BIT;
    const int row_copy = s.row == d->row && !mirror_x;
    const uint32_t sw = s.row / s.block_bytes, dw = d->row / d->block_bytes;
    for (unsigned z = 0; z < d->images; ++z) {
        const unsigned sz = texture_nearest(z, s.images, d->images, flags & NVN_COPY_FLAGS_MIRROR_Z_BIT);
        for (unsigned y = 0; y < d->rows; ++y) {
            const unsigned sy = texture_nearest(y, s.rows, d->rows, flags & NVN_COPY_FLAGS_MIRROR_Y_BIT);
            for (uint32_t x = 0; x < d->row;) {
                const uint32_t sx = row_copy ? x : texture_nearest(x / d->block_bytes, sw, dw, mirror_x) * s.block_bytes;
                const uint32_t bytes = row_copy ? d->row - x : d->block_bytes;
                for (uint32_t i = 0; i < bytes;) {
                    const struct texture_span src = texture_transfer_span(&s, sz, sy, sx + i, bytes - i, operation);
                    const struct texture_span dst = texture_transfer_span(d, z, y, x + i, src.bytes, operation);
                    memcpy(dst.data, src.data, dst.bytes);
                    i += dst.bytes;
                }
                x += bytes;
            }
        }
    }
    free(snapshot);
}
