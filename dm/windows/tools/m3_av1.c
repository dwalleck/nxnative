// Native Windows AV1 provider for the shared movie decoder contract.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dav1d/dav1d.h>
#include "../../host/hle_movie_av1.h"

#if DAV1D_API_VERSION_MAJOR != 7
#error Update the dav1d DLL name and its staging in build_host.cmd
#endif

#define DAV1D_DLL L"libdav1d-7.dll"
#define DAV1D_SYMBOLS(X) \
    X(const char *, version, (void)) \
    X(void, default_settings, (Dav1dSettings *)) \
    X(int, open, (Dav1dContext **, const Dav1dSettings *)) \
    X(void, close, (Dav1dContext **)) \
    X(void, flush, (Dav1dContext *)) \
    X(int, parse_sequence_header, (Dav1dSequenceHeader *, const uint8_t *, size_t)) \
    X(int, send_data, (Dav1dContext *, Dav1dData *)) \
    X(int, get_picture, (Dav1dContext *, Dav1dPicture *)) \
    X(uint8_t *, data_create, (Dav1dData *, size_t)) \
    X(void, data_unref, (Dav1dData *)) \
    X(void, picture_unref, (Dav1dPicture *))
static struct {
#define DECLARE(ret, name, args) ret (*name) args;
    DAV1D_SYMBOLS(DECLARE)
#undef DECLARE
} dll;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static HMODULE module;

static BOOL CALLBACK load_dav1d(PINIT_ONCE token, PVOID arg, PVOID *context) {
    (void)token; (void)arg; (void)context;
    // Only the application and system directories, not PATH or the guest's working directory.
    HMODULE handle = LoadLibraryExW(DAV1D_DLL, NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!handle) {
        fprintf(stderr, "[RED] movie: cannot load %ls (Win32 error %lu); install the ARM64 DLL beside the host\n",
                DAV1D_DLL, GetLastError());
        return TRUE;  // Cache failure too; all instances use the same runtime.
    }

    const char *missing = "dav1d_version_api";
    unsigned (*version_api)(void) = (unsigned (*)(void))GetProcAddress(handle, missing);
    if (!version_api) goto missing_symbol;
    const unsigned api = version_api();
    const unsigned required = (DAV1D_API_VERSION_MAJOR << 16) | (DAV1D_API_VERSION_MINOR << 8) | DAV1D_API_VERSION_PATCH;
    if (DAV1D_API_MAJOR(api) != DAV1D_API_VERSION_MAJOR || api < required) {
        fprintf(stderr, "[RED] movie: %ls API %u.%u.%u is incompatible with %u.%u.%u headers\n", DAV1D_DLL,
                DAV1D_API_MAJOR(api), DAV1D_API_MINOR(api), DAV1D_API_PATCH(api),
                DAV1D_API_MAJOR(required), DAV1D_API_MINOR(required), DAV1D_API_PATCH(required));
        FreeLibrary(handle);
        return TRUE;
    }

#define RESOLVE(ret, name, args) \
    dll.name = (ret (*) args)GetProcAddress(handle, "dav1d_" #name); \
    if (!dll.name) { missing = "dav1d_" #name; goto missing_symbol; }
    DAV1D_SYMBOLS(RESOLVE)
#undef RESOLVE
    module = handle;  // Keep the verified decoder loaded for the lifetime of the process.
    fprintf(stderr, "[movie] AV1: dav1d %s decoding\n", dll.version());
    return TRUE;

missing_symbol:
    fprintf(stderr, "[RED] movie: %ls does not export %s\n", DAV1D_DLL, missing);
    FreeLibrary(handle);
    return TRUE;
}
#undef DAV1D_SYMBOLS

static void av1_log(const char *format, ...) {
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
    fprintf(stderr, "%s\n", message);
}

#define MAV1_DAV1D(name) dll.name
#define MAV1_DAV1D_LOG av1_log
#include "../../host/hle_movie_av1_dav1d.inc"
#include "../../host/hle_movie_av1_config.inc"

struct mav1 {
    struct mav1_dav1d decoder;
    int have_seq, red;
};

mav1 *mav1_new(struct movie_vt_control *control) {
    (void)control;
    if (!InitOnceExecuteOnce(&once, load_dav1d, NULL, NULL) || !module) return NULL;
    mav1 *a = calloc(1, sizeof *a);
    if (!a) {
        fputs("[RED] movie: AV1 decoder allocation failed\n", stderr);
        return NULL;
    }
    if (!mav1_d_open(&a->decoder)) {
        fputs("[RED] movie: dav1d could not be opened\n", stderr);
        free(a);
        return NULL;
    }
    return a;
}

void mav1_free(mav1 *a) {
    if (a) { mav1_d_close(&a->decoder); free(a); }
}
void mav1_reset(mav1 *a) {
    mav1_d_reset(&a->decoder);
    a->have_seq = 0;
}
const char *mav1_backend(const mav1 *a) { (void)a; return "dav1d"; }

int mav1_decode(mav1 *a, const uint8_t *data, size_t n, int64_t ts, mav1_picture *out) {
    if (!n) return 0;
    if (a->have_seq) return mav1_d_decode(&a->decoder, data, n, ts, out, &a->red);
    // Before a stream's sequence header, no temporal unit can produce a picture.
    Dav1dSequenceHeader seq;
    int r = dll.parse_sequence_header(&seq, data, n);
    if (r == DAV1D_ERR(ENOENT)) return 0;
    if (r) return -1;
    r = mav1_d_decode(&a->decoder, data, n, ts, out, &a->red);
    a->have_seq = r >= 0;
    return r;
}

int mav1_next(mav1 *a, int drain, mav1_picture *out) {
    return a->have_seq ? mav1_d_step(&a->decoder, drain, out, &a->red) : 0;
}
