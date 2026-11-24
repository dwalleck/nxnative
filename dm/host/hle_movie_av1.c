// AV1 for the nn::codec decoders (hle_codec_video.c): Fire Emblem Fortune's Weave's 82 movies are AV1 in WebM (2560x1440). Two backends behind one interface: Apple's VideoToolbox, whose AV1
// decoder is hardware only (Apple M3 and A17 Pro on), and dav1d (dm/third_party/dav1d) on older chips, for a stream VideoToolbox refuses, or with DM_AV1_DAV1D=1 on any chip. The movie module
// aborts on a decoder that does not initialize (movie::detail::InitializeDecoderByTransferMemory, subsdk2+0xf9b4), so without one a game with AV1 movies could not run there.
// VideoToolbox wants a format description holding the stream's av1C record (ISO-BMFF AV1 binding, 2.3.3): four bytes read from the sequence header, then that header's OBU. The decoder API takes no
// configuration for AV1, so the sequence header comes from the stream, where every key frame carries one (AV1 5.5); a changed one makes a new session. A temporal unit goes in whole, its OBUs in
// the low-overhead format WebM and the SDK's demuxer keep them in (each with its size). Decoding is synchronous and a temporal unit shows at most one frame, in presentation order, so no queue.
// dav1d decodes several frames at once (frame threads), so its pictures come out later than their units go in, each with its unit's time (mav1_next)
#include "hle.h"
#include <VideoToolbox/VideoToolbox.h>
#include "dav1d/dav1d.h"
#include "hle_movie_av1.h"
#include "hle_movie_vt.h"

#define MAV1_DAV1D_LOG LOG
#include "hle_movie_av1_dav1d.inc"
#include "hle_movie_av1_config.inc"

struct mav1 {
    uint8_t seq[512]; size_t nseq;
    CMVideoFormatDescriptionRef fd; VTDecompressionSessionRef vs;
    CVPixelBufferRef got, locked;
    int soft;                     // dav1d decodes: no hardware decoder, DM_AV1_DAV1D=1, or a stream VideoToolbox refused (until mav1_reset)
    struct mav1_dav1d dav1d;
    int red;
    movie_vt_history history;
};
static int hw_av1(void) { const char *e = getenv("DM_AV1_DAV1D"); return !(e && *e && *e != '0') && VTIsHardwareDecodeSupported(kCMVideoCodecType_AV1); }
mav1 *mav1_new(movie_vt_control *control) { mav1 *a = calloc(1, sizeof *a); if (!a) die("movie: AV1 decoder allocation failed"); a->soft = !hw_av1(); a->history.control = control; return a; }
const char *mav1_backend(const mav1 *a) { return a->soft ? "dav1d" : "VideoToolbox"; }
static void unlock_picture(mav1 *a) {
    if (a->locked) { CVPixelBufferUnlockBaseAddress(a->locked, kCVPixelBufferLock_ReadOnly); CVPixelBufferRelease(a->locked); a->locked = NULL; }
}
static void av1_close(mav1 *a) {
    if (a->vs) { VTDecompressionSessionWaitForAsynchronousFrames(a->vs); VTDecompressionSessionInvalidate(a->vs); CFRelease(a->vs); a->vs = NULL; }
    if (a->fd) { CFRelease(a->fd); a->fd = NULL; }
    if (a->got) { CVPixelBufferRelease(a->got); a->got = NULL; }
}
void mav1_reset(mav1 *a) { unlock_picture(a); av1_close(a); movie_vt_clear(&a->history); a->nseq = 0; mav1_d_reset(&a->dav1d); a->soft = !hw_av1(); }
void mav1_free(mav1 *a) { if (a) { mav1_reset(a); mav1_d_close(&a->dav1d); free(a); } }

// The sequence header OBU of a temporal unit, or none
static const uint8_t *find_seq(const uint8_t *p, size_t n, size_t *len) {
    for (size_t i = 0; i < n;) {
        if (!(p[i] & 2)) return NULL;   // no size field: not the low-overhead format
        size_t hdr = p[i] & 4 ? 2 : 1; uint64_t size;
        size_t l = i + hdr < n ? leb128(p + i + hdr, n - i - hdr, &size) : 0;
        if (!l || size > n - i - hdr - l) return NULL;
        if ((p[i] >> 3 & 15) == 1) { *len = hdr + l + (size_t)size; return p + i; }
        i += hdr + l + (size_t)size;
    }
    return NULL;
}
// Only a shown key frame resets every reference slot (AV1 5.9.2).
// Hidden key frames can retain references to pictures before them.
static int av1_key(const mav1 *a, const uint8_t *data, size_t n) {
    int key = 0;
    for (size_t i = 0; i < n;) {
        size_t hdr = data[i] & 4 ? 2 : 1; uint64_t size;
        size_t l = i + hdr < n ? leb128(data + i + hdr, n - i - hdr, &size) : 0;
        if (!(data[i] & 2) || !l || size > n - i - hdr - l) break;
        unsigned type = data[i] >> 3 & 15;
        if ((type == 3 || type == 6) && size && a->nseq) {
            size_t sh = a->seq[0] & 4 ? 2 : 1; uint64_t sn;
            size_t sl = leb128(a->seq + sh, a->nseq - sh, &sn);
            uint8_t frame = data[i + hdr + l];
            key = sl && sn && ((a->seq[sh + sl] & 8) || (frame & 0xf0) == 0x10);
            break;
        }
        i += hdr + l + (size_t)size;
    }
    return key;
}
static void av1_on_frame(void *ref, void *src, OSStatus st, VTDecodeInfoFlags fl, CVImageBufferRef img, CMTime pts, CMTime dur) {
    (void)src; (void)fl; (void)pts; (void)dur; mav1 *a = ref;
    if (st) { a->history.status = st; return; }
    if (a->history.replaying || !img) return;
    if (a->got) CVPixelBufferRelease(a->got);
    a->got = CVPixelBufferRetain(img);
}
static int av1_open(mav1 *a, const uint8_t *seq, size_t n) {
    av1_close(a);
    uint8_t rec[4 + sizeof a->seq]; int w = 0, h = 0;
    if (n > sizeof a->seq || !mav1_av1c(seq, n, rec, &w, &h)) { if (!a->red++) LOG("[RED] movie: an AV1 sequence header the host cannot read (%zu bytes)", n); return 0; }
    memcpy(rec + 4, seq, n);
    CFDataRef data = CFDataCreate(NULL, rec, (CFIndex)(4 + n));
    const void *ak[] = {CFSTR("av1C")}, *av[] = {data};
    CFDictionaryRef atoms = CFDictionaryCreate(NULL, ak, av, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); CFRelease(data);
    const void *ek[] = {kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms}, *ev[] = {atoms};
    CFDictionaryRef ext = CFDictionaryCreate(NULL, ek, ev, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); CFRelease(atoms);
    OSStatus st = CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_AV1, w, h, ext, &a->fd); CFRelease(ext);
    if (st) { if (!a->red++) LOG("[RED] movie: no format description for AV1 (%d)", (int)st); return 0; }
    int32_t pf = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef num = CFNumberCreate(NULL, kCFNumberSInt32Type, &pf);
    const void *k[] = {kCVPixelBufferPixelFormatTypeKey}, *v[] = {num};
    CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks); CFRelease(num);
    VTDecompressionOutputCallbackRecord cb = {av1_on_frame, a};
    st = movie_vt_create(&a->history, a->fd, attrs, &cb, &a->vs); CFRelease(attrs);
    if (st == MOVIE_VT_CANCELLED) return -1;
    if (st) { if (!a->red++) LOG("[RED] movie: VideoToolbox cannot create an AV1 session (%d)", (int)st); a->vs = NULL; return 0; }
    if (a->seq != seq) memcpy(a->seq, seq, n); a->nseq = n;
    static int once; if (!once++) LOG("[movie] AV1: VideoToolbox decoding %dx%d, profile %d level %d", w, h, rec[1] >> 5, rec[1] & 31);
    return 1;
}
static int decode_dav1d(mav1 *a, const uint8_t *data, size_t n, int64_t ts, mav1_picture *out) {
    if (!a->dav1d.ctx) {
        if (!mav1_d_open(&a->dav1d)) { if (!a->red++) LOG("[RED] movie: dav1d could not be opened"); return -1; }
        static int once; if (!once++) LOG("[movie] AV1: dav1d %s decoding, %d threads on %d frames at once (%s)", dav1d_version(), DAV1D_THREADS, DAV1D_FRAMES,
                                          hw_av1() ? "a stream VideoToolbox refused" : getenv("DM_AV1_DAV1D") ? "DM_AV1_DAV1D" : "no hardware AV1 decoder");
    }
    return mav1_d_decode(&a->dav1d, data, n, ts, out, &a->red);
}
int mav1_next(mav1 *a, int drain, mav1_picture *out) { return a->soft && a->dav1d.ctx ? mav1_d_step(&a->dav1d, drain, out, &a->red) : 0; }

int mav1_decode(mav1 *a, const uint8_t *data, size_t n, int64_t ts, mav1_picture *out) {
    if (a->soft) return decode_dav1d(a, data, n, ts, out);
    unlock_picture(a);
    size_t seqn = 0; const uint8_t *seq = find_seq(data, n, &seqn);
    int changed = seq && (seqn != a->nseq || memcmp(seq, a->seq, seqn));
    if (changed) movie_vt_clear(&a->history);
    if (seq && (changed || !a->vs)) {
        int opened = av1_open(a, seq, seqn);
        if (opened < 0) return 0;   // Stop / Flush / Finalize cancelled a lifecycle wait
        if (!opened) {   // a stream VideoToolbox does not take: dav1d from this temporal unit on (it holds the sequence header)
            LOG("[movie] AV1: dav1d decodes the stream instead"); av1_close(a); a->soft = 1;
            return decode_dav1d(a, data, n, ts, out);
        }
    }
    if (!a->vs) return 0;   // the stream's first sequence header has not come
    CMBlockBufferRef bb = NULL; CMSampleBufferRef sb = NULL; size_t ssz = n;
    OSStatus st = CMBlockBufferCreateWithMemoryBlock(NULL, NULL, n, NULL, NULL, 0, n, 0, &bb);
    if (!st) st = CMBlockBufferReplaceDataBytes(data, bb, 0, n);
    if (!st) st = CMSampleBufferCreateReady(NULL, bb, a->fd, 1, 0, NULL, 1, &ssz, &sb);
    if (!st) {
        int key = av1_key(a, data, n);
        if (key) { movie_vt_clear(&a->history); a->history.key = 1; }
        st = movie_vt_submit(&a->history, a->vs, sb, NULL);
        while (st == kVTInvalidSessionErr && a->history.key) {
            int opened = av1_open(a, a->seq, a->nseq);
            if (opened < 0) st = MOVIE_VT_CANCELLED;
            else if (opened) {
                st = movie_vt_replay(&a->history, a->vs);
                if (!st) st = movie_vt_submit(&a->history, a->vs, sb, NULL);
            } else break;
        }
        if (!st) movie_vt_remember(&a->history, sb);
    }
    if (sb) CFRelease(sb);
    if (bb) CFRelease(bb);
    if (st == MOVIE_VT_CANCELLED) return 0;
    if (st) { if (!a->red++) LOG("[RED] movie: AV1 decode submission failed (%d, first case)", (int)st); return -1; }
    if (!a->got) return 0;
    CVPixelBufferRef pb = a->got; a->got = NULL; a->locked = pb;
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    CFStringRef m = CVBufferCopyAttachment(pb, kCVImageBufferYCbCrMatrixKey, NULL);
    *out = (mav1_picture){CVPixelBufferGetBaseAddressOfPlane(pb, 0), CVPixelBufferGetBaseAddressOfPlane(pb, 1), CVPixelBufferGetBytesPerRowOfPlane(pb, 0), CVPixelBufferGetBytesPerRowOfPlane(pb, 1),
                          (int)CVPixelBufferGetWidthOfPlane(pb, 0), (int)CVPixelBufferGetHeightOfPlane(pb, 0),
                          !m || CFEqual(m, kCVImageBufferYCbCrMatrix_ITU_R_709_2) ? 2 : CFEqual(m, kCVImageBufferYCbCrMatrix_ITU_R_2020) ? 5 : 1, 0, ts};
    if (m) CFRelease(m);
    return 1;
}
