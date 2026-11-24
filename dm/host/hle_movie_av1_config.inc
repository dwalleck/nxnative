// AV1 sequence header (AV1 5.5), read as far as the colour config for an av1C record.
typedef struct { const uint8_t *p; size_t n, bit; int bad; } av1_bits;
static uint32_t rb(av1_bits *b, unsigned n) {
    uint32_t v = 0;
    if (b->bit + n > b->n * 8) { b->bad = 1; return 0; }
    while (n--) { v = v << 1 | ((b->p[b->bit / 8] >> (7 - b->bit % 8)) & 1); b->bit++; }
    return v;
}
static void uvlc(av1_bits *b) { unsigned z = 0; while (!b->bad && !rb(b, 1)) if (++z >= 32) return; if (z < 32) rb(b, z); }
static size_t leb128(const uint8_t *p, size_t n, uint64_t *v) {
    *v = 0;
    for (size_t i = 0; i < n && i < 8; i++) { *v |= (uint64_t)(p[i] & 0x7f) << (i * 7); if (!(p[i] & 0x80)) return i + 1; }
    return 0;
}
int mav1_av1c(const uint8_t *obu, size_t n, uint8_t c[4], int *w, int *h) {
    if (n < 2 || (obu[0] >> 3 & 15) != 1 || !(obu[0] & 2)) return 0;   // a sequence header with a size field
    size_t at = obu[0] & 4 ? 2 : 1; uint64_t size;
    if (at >= n) return 0;
    size_t l = leb128(obu + at, n - at, &size);
    if (!l || size > n - at - l) return 0;
    av1_bits b = {obu + at + l, (size_t)size, 0, 0};
    unsigned profile = rb(&b, 3); rb(&b, 1); unsigned reduced = rb(&b, 1), level = 0, tier = 0;
    unsigned decoder_model = 0, delay_len = 0;
    if (reduced) level = rb(&b, 5);
    else {
        if (rb(&b, 1)) {   // timing_info
            rb(&b, 32); rb(&b, 32); if (rb(&b, 1)) uvlc(&b);
            if ((decoder_model = rb(&b, 1))) { delay_len = rb(&b, 5) + 1; rb(&b, 32); rb(&b, 5); rb(&b, 5); }
        }
        unsigned display_delay = rb(&b, 1), points = rb(&b, 5) + 1;
        for (unsigned i = 0; i < points && !b.bad; i++) {
            rb(&b, 12); unsigned lv = rb(&b, 5), t = lv > 7 ? rb(&b, 1) : 0;
            if (!i) { level = lv; tier = t; }
            if (decoder_model && rb(&b, 1)) { rb(&b, delay_len); rb(&b, delay_len); rb(&b, 1); }
            if (display_delay && rb(&b, 1)) rb(&b, 4);
        }
    }
    unsigned wb = rb(&b, 4) + 1, hb = rb(&b, 4) + 1;
    *w = (int)rb(&b, wb) + 1; *h = (int)rb(&b, hb) + 1;
    if (!reduced && rb(&b, 1)) { rb(&b, 4); rb(&b, 3); }   // frame ids
    rb(&b, 3);   // 128x128 superblocks, filter intra, intra edge filter
    if (!reduced) {
        rb(&b, 4);   // inter-intra, masked compound, warped motion, dual filter
        unsigned order_hint = rb(&b, 1);
        if (order_hint) rb(&b, 2);   // jnt_comp, ref_frame_mvs
        unsigned tools = rb(&b, 1) ? 2 : rb(&b, 1);
        if (tools && !rb(&b, 1)) rb(&b, 1);   // force_integer_mv
        if (order_hint) rb(&b, 3);
    }
    rb(&b, 3);   // superres, cdef, restoration
    unsigned hbd = rb(&b, 1), twelve = profile == 2 && hbd ? rb(&b, 1) : 0, mono = profile == 1 ? 0 : rb(&b, 1), cp = 2, tc = 2, mc = 2, ssx = 1, ssy = 1, pos = 0;
    if (rb(&b, 1)) { cp = rb(&b, 8); tc = rb(&b, 8); mc = rb(&b, 8); }
    if (!mono) {
        if (cp == 1 && tc == 13 && mc == 0) ssx = ssy = 0;   // sRGB
        else {
            rb(&b, 1);   // colour range
            if (profile == 1) ssx = ssy = 0;
            else if (profile == 2) { if (twelve) { ssx = rb(&b, 1); ssy = ssx ? rb(&b, 1) : 0; } else { ssx = 1; ssy = 0; } }
            if (ssx && ssy) pos = rb(&b, 2);
        }
    }
    if (b.bad) return 0;
    c[0] = 0x81;   // marker, version 1
    c[1] = (uint8_t)(profile << 5 | (level & 31));
    c[2] = (uint8_t)(tier << 7 | hbd << 6 | twelve << 5 | mono << 4 | ssx << 3 | ssy << 2 | pos);
    c[3] = 0;
    return 1;
}
