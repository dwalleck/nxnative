// Shared software AV1 path. Apple calls its static library directly; Windows
// supplies the same calls through its version-checked DLL.
#ifndef MAV1_DAV1D
#define MAV1_DAV1D(name) dav1d_##name
#endif

struct mav1_dav1d {
    Dav1dContext *ctx; Dav1dPicture pic; int has_pic;
    Dav1dData in;                 // the temporal unit dav1d has not taken whole yet
    int draining;                 // a drain gave a picture and has not reached its end (mav1_d_step)
    uint8_t *y8, *uv; size_t ny8, nuv;
};

static void mav1_d_log(void *cookie, const char *fmt, va_list ap) {
    (void)cookie; char b[256]; vsnprintf(b, sizeof b, fmt, ap);
    size_t l = strlen(b); if (l && b[l - 1] == '\n') b[l - 1] = 0;
    MAV1_DAV1D_LOG("[movie] dav1d: %s", b);
}

// A temporal unit of a hierarchical stream holds several frames (the hidden ones the next units only show). Decoded one after the other, they made that
// unit many times longer than the others: Ocarina of Time's 1080p 60 fps title movie took 80 to 122 ms for one unit in 32 on an M2 Pro (two task threads,
// one frame at a time) and 15.4 ms a unit over its heaviest five seconds, at a budget of 16.7 ms. Frame threads decode them and later units side by side.
#define DAV1D_THREADS 4
#define DAV1D_FRAMES 3
static int mav1_d_open(struct mav1_dav1d *a) {
    Dav1dSettings st; MAV1_DAV1D(default_settings)(&st);
    st.n_threads = DAV1D_THREADS; st.max_frame_delay = DAV1D_FRAMES;
    st.logger.callback = mav1_d_log;
    if (MAV1_DAV1D(open)(&a->ctx, &st)) { a->ctx = NULL; return 0; }
    return 1;
}

static void mav1_d_unpin(struct mav1_dav1d *a) {
    if (a->has_pic) { MAV1_DAV1D(picture_unref)(&a->pic); a->has_pic = 0; }
}

static void mav1_d_clear(struct mav1_dav1d *a) {
    mav1_d_unpin(a);
    if (a->in.sz) MAV1_DAV1D(data_unref)(&a->in);
    a->draining = 0;
}

static void mav1_d_reset(struct mav1_dav1d *a) {
    mav1_d_clear(a);
    if (a->ctx) MAV1_DAV1D(flush)(a->ctx);
}

static void mav1_d_close(struct mav1_dav1d *a) {
    mav1_d_clear(a);
    if (a->ctx) MAV1_DAV1D(close)(&a->ctx);
    free(a->y8); free(a->uv);
}

static int mav1_d_grow(uint8_t **p, size_t *have, size_t want) {
    if (*have >= want) return 1;
    uint8_t *q = realloc(*p, want); if (!q) return 0;
    *p = q; *have = want; return 1;
}

// Borrow the 8-bit luma directly, or shift high bit depths down; interleave the chroma (subsampled from 4:2:2 or 4:4:4, grey for 4:0:0).
static int mav1_d_nv12(struct mav1_dav1d *a, const Dav1dPicture *p, mav1_picture *out) {
    int w = p->p.w, h = p->p.h, sh = p->p.bpc - 8, cw = (w + 1) / 2, ch = (h + 1) / 2;
    enum Dav1dPixelLayout l = p->p.layout;
    int sx = l != DAV1D_PIXEL_LAYOUT_I444, sy = l == DAV1D_PIXEL_LAYOUT_I420;
    size_t uvs = (size_t)cw * 2;
    if (!mav1_d_grow(&a->uv, &a->nuv, uvs * (size_t)ch) || (sh && !mav1_d_grow(&a->y8, &a->ny8, (size_t)w * (size_t)h))) return 0;
    const uint8_t *y = p->data[0]; size_t ys = (size_t)p->stride[0];
    if (sh) {
        for (int r = 0; r < h; r++) { const uint16_t *s = (const uint16_t *)((const uint8_t *)p->data[0] + (ptrdiff_t)r * p->stride[0]); uint8_t *o = a->y8 + (size_t)r * (size_t)w; for (int x = 0; x < w; x++) o[x] = (uint8_t)(s[x] >> sh); }
        y = a->y8; ys = (size_t)w;
    }
    for (int r = 0; r < ch; r++) {
        uint8_t *o = a->uv + (size_t)r * uvs;
        if (l == DAV1D_PIXEL_LAYOUT_I400) { memset(o, 128, uvs); continue; }
        const uint8_t *u = (const uint8_t *)p->data[1] + (ptrdiff_t)(sy ? r : 2 * r) * p->stride[1], *v = (const uint8_t *)p->data[2] + (ptrdiff_t)(sy ? r : 2 * r) * p->stride[1];
        for (int x = 0; x < cw; x++) {
            int c = sx ? x : 2 * x;
            o[2 * x] = sh ? (uint8_t)(((const uint16_t *)u)[c] >> sh) : u[c]; o[2 * x + 1] = sh ? (uint8_t)(((const uint16_t *)v)[c] >> sh) : v[c];
        }
    }
    enum Dav1dMatrixCoefficients mc = p->seq_hdr->mtrx;   // when unspecified, match VideoToolbox's BT.601 below 720 lines and BT.709 from there
    if (mc == DAV1D_MC_UNKNOWN) mc = h < 720 ? DAV1D_MC_BT601 : DAV1D_MC_BT709;
    *out = (mav1_picture){y, a->uv, ys, uvs, w, h, mc == DAV1D_MC_BT709 ? 2 : mc == DAV1D_MC_BT2020_NCL || mc == DAV1D_MC_BT2020_CL ? 5 : 1, p->seq_hdr->color_range, p->m.timestamp};
    return 1;
}

// Gives dav1d what it has not taken of the unit, then asks it for one picture: 1 with it, 0 none now, -1 the unit was refused. One dav1d_get_picture for each dav1d_send_data:
// a second one in a row is dav1d's drain, which waits for the frames in flight and so decodes them one after the other. With drain, and the unit taken, that is what is asked.
// A drain that has given a picture must go on to its end (EAGAIN) before the next unit: dav1d (1.5.4, lib.c drain_picture) steps over its empty frame contexts on the way to
// the oldest frame, which leaves frame_thread.next on a frame still in flight if data comes then. The next frame waits for that one and retires it with task_thread.first one
// context past the oldest frame for good, and retiring a frame cancels the task rescan asked for task_thread.first: a live frame's from then on. Its task is never run, the
// workers sleep and dav1d_submit_frame waits for ever (found when the drain stopped as soon as a packet arrived: Ocarina of Time's title movie froze in 3 runs of 6)
static int mav1_d_step(struct mav1_dav1d *a, int drain, mav1_picture *out, int *red) {
    for (int tries = 0; tries < 64; tries++) {
        int draining = !a->in.sz;   // nothing to give: this get is the second in a row
        if (a->in.sz) {
            int r = MAV1_DAV1D(send_data)(a->ctx, &a->in);   // EAGAIN: a picture has to be taken first
            if (r && r != DAV1D_ERR(EAGAIN)) { MAV1_DAV1D(data_unref)(&a->in); if (!(*red)++) MAV1_DAV1D_LOG("[RED] movie: dav1d refused a temporal unit (%d, first case)", r); return -1; }
        } else if (!drain) return 0;
        Dav1dPicture p = {0};
        int g = MAV1_DAV1D(get_picture)(a->ctx, &p);
        if (!g) {
            if (draining) a->draining = 1;
            mav1_d_unpin(a);
            a->pic = p; a->has_pic = 1;
            if (!mav1_d_nv12(a, &a->pic, out)) { if (!(*red)++) MAV1_DAV1D_LOG("[RED] movie: no memory for a %dx%d AV1 picture", a->pic.p.w, a->pic.p.h); return -1; }
            return 1;
        }
        if (g == DAV1D_ERR(EAGAIN)) { if (draining) a->draining = 0; }
        else if (!(*red)++) MAV1_DAV1D_LOG("[RED] movie: dav1d could not decode a frame (%d, first case)", g);
        if (!a->in.sz && (g == DAV1D_ERR(EAGAIN) || !drain)) return 0;   // the unit is in and no picture is ready; a frame in error is passed over while draining
    }
    if (a->in.sz) { MAV1_DAV1D(data_unref)(&a->in); if (!(*red)++) MAV1_DAV1D_LOG("[RED] movie: dav1d took no more data, a temporal unit dropped (first case)"); return -1; }
    return 0;
}

static int mav1_d_decode(struct mav1_dav1d *a, const uint8_t *data, size_t n, int64_t ts, mav1_picture *out, int *red) {
    if (a->in.sz) MAV1_DAV1D(data_unref)(&a->in);   // a unit left unfinished (its caller stopped): dropped
    if (a->draining) {   // a drain its caller left: ended here, its pictures lost
        int lost = 0;
        for (int tries = 0; tries < 64; tries++) { Dav1dPicture p = {0}; int g = MAV1_DAV1D(get_picture)(a->ctx, &p); if (!g) { MAV1_DAV1D(picture_unref)(&p); lost++; } else if (g == DAV1D_ERR(EAGAIN)) break; }
        a->draining = 0;
        MAV1_DAV1D_LOG("[RED] movie: an AV1 drain was left before its end: %d picture(s) dropped", lost);
    }
    uint8_t *buf = MAV1_DAV1D(data_create)(&a->in, n);
    if (!buf) return -1;
    memcpy(buf, data, n); a->in.m.timestamp = ts;
    return mav1_d_step(a, 0, out, red);
}

#undef MAV1_DAV1D
#undef MAV1_DAV1D_LOG
