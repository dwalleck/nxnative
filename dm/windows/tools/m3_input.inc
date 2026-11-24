// Included by m3_services.c. XInput, the focused keyboard or opt-in DM_CTL feed one FullKey Npad.
#include <math.h>
#include <errno.h>
#include <share.h>
#include <xinput.h>
#include "m4_window.h"
#include "../../host/dm_input_keys.h"

enum { NPAD_FULLKEY = 1, NPAD_HISTORY = 16, NPAD_SAMPLE_100NS = 50000, NPAD_PROBE_100NS = 1000000 };
enum npad_source { NPAD_NONE, NPAD_XINPUT, NPAD_KEYBOARD, NPAD_CTL };
struct npad_state {
    int64_t sampling;
    uint64_t buttons;
    int32_t stick[4];
    uint32_t attributes, reserved;
};
_Static_assert(sizeof(struct npad_state) == 0x28, "NpadFullKeyState ABI");
typedef DWORD (WINAPI *get_xinput_state)(DWORD, XINPUT_STATE *);
static SRWLOCK npad_lock = SRWLOCK_INIT;
static struct {
    int initialized, windowed, detached;
    uint32_t ids, styles, id, keyboard_focus;
    enum npad_source source;
    get_xinput_state get_pad;
    DWORD pad_slot;
    uint64_t next_pad_poll, next_key_sample;
    int64_t sampling;
    unsigned next, available;
    struct npad_state history[NPAD_HISTORY];
} npad = { .pad_slot = XUSER_MAX_COUNT, .id = UINT32_MAX };

enum npad_ctl_phase { CTL_IDLE, CTL_PENDING, CTL_HELD, CTL_NEUTRAL };
enum { NPAD_CTL_GAP_100NS = 500000 };
static struct {
    FILE *file;
    size_t used;
    char line[1100];
    enum npad_ctl_phase phase;
    uint64_t next_poll, phase_at, buttons;
    int32_t stick[4];
    double hold;
} npad_ctl;

static void npad_open_ctl(void) {
    wchar_t *path = NULL;
    if (_wdupenv_s(&path, NULL, L"DM_CTL")) {
        fputs("[hid] cannot read DM_CTL environment setting\n", stderr);
        return;
    }
    if (path && *path) {
        npad_ctl.file = _wfsopen(path, L"rb", _SH_DENYNO);
        if (!npad_ctl.file) fprintf(stderr, "[hid] cannot open DM_CTL file (errno %d)\n", errno);
    }
    free(path);
}

// The live file uses the Apple DM_CTL syntax: one key[+key][~hold seconds] per line.
// This host only supports the single FullKey player's buttons and sticks.
static int npad_ctl_parse(char *line) {
    while (*line == ' ' || *line == '\t') ++line;
    char *end = line + strlen(line);
    while (end > line && strchr(" \t\r", end[-1])) *--end = 0;
    if (!*line || *line == '#') return 0;

    npad_ctl.hold = DM_KEY_HOLD_DEFAULT;
    char *hold = strchr(line, '~');
    if (hold) {
        *hold++ = 0;
        npad_ctl.hold = strtod(hold, &end);
        if (end == hold || *end || !isfinite(npad_ctl.hold) || npad_ctl.hold < 0) {
            fputs("[hid] DM_CTL: invalid hold duration\n", stderr);
            return 0;
        }
    }
    npad_ctl.buttons = 0;
    memset(npad_ctl.stick, 0, sizeof npad_ctl.stick);
    for (char *key = line;;) {
        char *plus = strchr(key, '+');
        if (plus) *plus = 0;
        if (!dm_parse_npad_key(key, &npad_ctl.buttons,
                               &npad_ctl.stick[0], &npad_ctl.stick[1], &npad_ctl.stick[2], &npad_ctl.stick[3])) {
            fprintf(stderr, "[hid] DM_CTL: unsupported key '%s' (command ignored)\n", key);
            return 0;
        }
        if (!plus) return 1;
        key = plus + 1;
    }
}

static int npad_ctl_next(void) {
    // EOF is temporary on an append-only file. Keep partial lines until their newline,
    // and bound work done on the guest thread even if the writer floods the file.
    clearerr(npad_ctl.file);
    for (unsigned budget = 4096; budget; --budget) {
        const int c = _fgetc_nolock(npad_ctl.file);
        if (c == EOF) break;
        if (c == '\n') {
            const size_t used = npad_ctl.used;
            npad_ctl.used = 0;
            if (used >= sizeof npad_ctl.line) {
                fputs("[hid] DM_CTL: oversized or binary line ignored\n", stderr);
                continue;
            }
            npad_ctl.line[used] = 0;
            if (npad_ctl_parse(npad_ctl.line)) return 1;
        } else if (c && npad_ctl.used < sizeof npad_ctl.line - 1) {
            npad_ctl.line[npad_ctl.used++] = (char)c;
        } else {
            npad_ctl.used = sizeof npad_ctl.line;
        }
    }
    return 0;
}

static uint32_t npad_id_bit(uint32_t id) {
    if (id < 8) return 1u << id;
    if (id == 0x10) return 1u << 8;
    if (id == 0x20) return 1u << 9;
    dm_fail("hid: invalid Npad ID %#x", id);
}
static uint32_t npad_read_id(const uint32_t *id) {
    if (!dm_guest_range(id, sizeof(*id), 0)) dm_fail("hid: invalid Npad ID reference");
    const uint32_t value = *id;
    (void)npad_id_bit(value);
    return value;
}
static void npad_require_initialized(void) {
    if (!npad.initialized) dm_fail("hid: Npad used before InitializeNpad");
}

static void npad_select_id(void) {
    uint32_t id = UINT32_MAX;
    for (uint32_t players = npad.styles & NPAD_FULLKEY ? npad.ids & 0xffu : 0, i = 0; players; players >>= 1, ++i)
        if (players & 1u) { id = i; break; }
    if (npad.id == id) return;
    npad.id = id;
    npad.source = NPAD_NONE;
    npad.detached = 0;
    npad.available = 0;
    npad.next_pad_poll = npad.next_key_sample = 0;
}

// A missing DLL (or export) is the same as no XInput source. Keep a successful
// System32 module reference for the lifetime of the SDK imports.
static get_xinput_state npad_load_xinput(void) {
    static const wchar_t *const dlls[] = { L"XInput1_4.dll", L"XInput9_1_0.dll" };
    for (size_t i = 0; i < ARRAYSIZE(dlls); ++i) {
        HMODULE dll = LoadLibraryExW(dlls[i], NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll) continue;
        get_xinput_state get_state = (get_xinput_state)GetProcAddress(dll, "XInputGetState");
        if (get_state) return get_state;
        FreeLibrary(dll);
    }
    return NULL;
}

void hid_init_npad(void) {
    AcquireSRWLockExclusive(&npad_lock);
    if (!npad.initialized) {
        npad.initialized = 1;
        npad.windowed = GetEnvironmentVariableW(L"DM_WINDOW", NULL, 0) != 0;
        npad_open_ctl();
        if (npad_ctl.file) puts("[hid] initialized Npad with DM_CTL file input");
        else {
            if (npad.windowed) npad.get_pad = npad_load_xinput();
            puts(npad.windowed ? "[hid] initialized native Npad" : "[hid] initialized headless Npad (no input devices)");
        }
        fflush(stdout);
    }
    ReleaseSRWLockExclusive(&npad_lock);
}
void hid_set_npad_ids(const uint32_t *ids, size_t count) {
    if (count > 10 || (count && !dm_guest_range(ids, count * sizeof(*ids), 0)))
        dm_fail("hid: invalid supported Npad ID list");
    uint32_t mask = 0;
    for (size_t i = 0; i < count; ++i) mask |= npad_id_bit(ids[i]);
    AcquireSRWLockExclusive(&npad_lock);
    npad_require_initialized();
    npad.ids = mask;
    npad_select_id();
    ReleaseSRWLockExclusive(&npad_lock);
}
void hid_set_npad_styles(uint32_t styles) {
    AcquireSRWLockExclusive(&npad_lock);
    npad_require_initialized();
    npad.styles = styles;
    npad_select_id();
    ReleaseSRWLockExclusive(&npad_lock);
}
uint32_t hid_get_npad_styles(void) {
    AcquireSRWLockShared(&npad_lock);
    npad_require_initialized();
    const uint32_t styles = npad.styles;
    ReleaseSRWLockShared(&npad_lock);
    return styles;
}

static void npad_select_source(enum npad_source source, uint32_t focus) {
    if (source == npad.source && focus == npad.keyboard_focus) return;
    npad.source = source;
    npad.keyboard_focus = focus;
    npad.detached = 0;
    npad.available = 0; // Never expose stale history after unplug, rebind or focus loss.
    npad.next_key_sample = 0;
}

static struct npad_state *npad_sample(void) {
    struct npad_state *s = &npad.history[npad.next];
    npad.next = (npad.next + 1) % NPAD_HISTORY;
    if (npad.available < NPAD_HISTORY) ++npad.available;
    // Full controller attached; XInput does not report wired vs. wireless.
    *s = (struct npad_state){ .sampling = ++npad.sampling, .attributes = 0x15 };
    return s;
}

static void npad_stick(int32_t x, int32_t y, int deadzone, int32_t out[2]) {
    const int64_t squared = (int64_t)x * x + (int64_t)y * y;
    if (squared <= deadzone * deadzone) return;
    const double length = sqrt((double)squared);
    const double limit = length > DM_STICK_MAX ? DM_STICK_MAX : length;
    const double scale = (limit - deadzone) * DM_STICK_MAX / ((DM_STICK_MAX - deadzone) * length);
    out[0] = (int32_t)(x * scale);
    out[1] = (int32_t)(y * scale);
}

static void npad_stick_buttons(struct npad_state *s) {
    for (unsigned i = 0; i < 4; ++i) {
        const int32_t direction = (i % 2) ? -s->stick[i] : s->stick[i];
        const unsigned bit = 16 + 4 * (i / 2) + i % 2;
        if (direction < -0x4000) s->buttons |= UINT64_C(1) << bit;
        if (direction > 0x4000) s->buttons |= UINT64_C(1) << (bit + 2);
    }
}

static void npad_sample_pad(const XINPUT_GAMEPAD *pad) {
    static const struct { WORD xbox; uint32_t npad; } buttons[] = {
        // Match button positions: Switch A is right and B is bottom.
        { XINPUT_GAMEPAD_B, DM_BTN_A }, { XINPUT_GAMEPAD_A, DM_BTN_B },
        { XINPUT_GAMEPAD_Y, DM_BTN_X }, { XINPUT_GAMEPAD_X, DM_BTN_Y },
        { XINPUT_GAMEPAD_LEFT_THUMB, DM_BTN_LS }, { XINPUT_GAMEPAD_RIGHT_THUMB, DM_BTN_RS },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, DM_BTN_L }, { XINPUT_GAMEPAD_RIGHT_SHOULDER, DM_BTN_R },
        { XINPUT_GAMEPAD_START, DM_BTN_PLUS }, { XINPUT_GAMEPAD_BACK, DM_BTN_MINUS },
        { XINPUT_GAMEPAD_DPAD_LEFT, DM_BTN_LEFT }, { XINPUT_GAMEPAD_DPAD_UP, DM_BTN_UP },
        { XINPUT_GAMEPAD_DPAD_RIGHT, DM_BTN_RIGHT }, { XINPUT_GAMEPAD_DPAD_DOWN, DM_BTN_DOWN },
    };
    struct npad_state *s = npad_sample();
    for (size_t i = 0; i < ARRAYSIZE(buttons); ++i)
        if (pad->wButtons & buttons[i].xbox) s->buttons |= buttons[i].npad;
    if (pad->bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) s->buttons |= DM_BTN_ZL;
    if (pad->bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) s->buttons |= DM_BTN_ZR;
    npad_stick(pad->sThumbLX, pad->sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, s->stick);
    npad_stick(pad->sThumbRX, pad->sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, s->stick + 2);
    npad_stick_buttons(s);
}

static int npad_key(const uint64_t keys[4], unsigned vk) {
    return (keys[vk / 64] & (UINT64_C(1) << (vk % 64))) != 0;
}
static void npad_sample_keys(const uint64_t keys[4]) {
    static const struct { unsigned key; uint32_t button; } buttons[] = {
        { 'Z', DM_BTN_A }, { VK_RETURN, DM_BTN_A }, { 'X', DM_BTN_B }, { VK_BACK, DM_BTN_B },
        { 'C', DM_BTN_X }, { 'V', DM_BTN_Y }, { 'F', DM_BTN_LS }, { 'G', DM_BTN_RS },
        { 'Q', DM_BTN_L }, { 'E', DM_BTN_R }, { '1', DM_BTN_ZL }, { '3', DM_BTN_ZR },
        { VK_TAB, DM_BTN_PLUS }, { VK_ESCAPE, DM_BTN_MINUS },
        { VK_LEFT, DM_BTN_LEFT }, { VK_UP, DM_BTN_UP }, { VK_RIGHT, DM_BTN_RIGHT }, { VK_DOWN, DM_BTN_DOWN },
    };
    struct npad_state *s = npad_sample();
    for (size_t i = 0; i < ARRAYSIZE(buttons); ++i)
        if (npad_key(keys, buttons[i].key)) s->buttons |= buttons[i].button;
    npad_stick((npad_key(keys, 'D') - npad_key(keys, 'A')) * DM_STICK_MAX,
               (npad_key(keys, 'W') - npad_key(keys, 'S')) * DM_STICK_MAX, 0, s->stick);
    npad_stick((npad_key(keys, 'L') - npad_key(keys, 'J')) * DM_STICK_MAX,
               (npad_key(keys, 'I') - npad_key(keys, 'K')) * DM_STICK_MAX, 0, s->stick + 2);
    npad_stick_buttons(s);
}

static const char *npad_poll_ctl(uint64_t now, int read_state) {
    npad_select_source(NPAD_CTL, 0);
    if (npad_ctl.phase == CTL_NEUTRAL && now - npad_ctl.phase_at >= NPAD_CTL_GAP_100NS)
        npad_ctl.phase = CTL_IDLE;
    if (npad_ctl.phase == CTL_IDLE && now >= npad_ctl.next_poll) {
        if (npad_ctl_next()) {
            npad_ctl.phase = CTL_PENDING;
            npad.detached = 0; // A new deliberately requested press can reattach.
        } else npad_ctl.next_poll = now + NPAD_PROBE_100NS;
    }
    if (!read_state || npad.detached) return NULL;

    // Time the hold from the first guest state read, not the file poll. Publish
    // both edges before taking the next line, even if the guest reads slowly.
    const char *edge = NULL;
    if (npad_ctl.phase == CTL_PENDING) {
        npad_ctl.phase = CTL_HELD;
        edge = "press";
    } else if (npad_ctl.phase == CTL_HELD && (double)(now - npad_ctl.phase_at) * 1e-7 >= npad_ctl.hold) {
        npad_ctl.phase = CTL_NEUTRAL;
        edge = "release";
    }
    if (edge) npad_ctl.phase_at = now;
    if (edge || now >= npad.next_key_sample) {
        struct npad_state *s = npad_sample();
        if (npad_ctl.phase == CTL_HELD) {
            s->buttons = npad_ctl.buttons;
            memcpy(s->stick, npad_ctl.stick, sizeof s->stick);
            npad_stick_buttons(s);
        }
        npad.next_key_sample = now + NPAD_SAMPLE_100NS;
    }
    return edge;
}

static const char *npad_poll(int read_state) {
    if ((!npad.windowed && !npad_ctl.file) || npad.id == UINT32_MAX) return NULL;
    ULONGLONG now;
    QueryInterruptTimePrecise(&now);
    if (npad_ctl.file) return npad_poll_ctl(now, read_state);
    if (npad.get_pad && now >= npad.next_pad_poll) {
        XINPUT_STATE state;
        DWORD slot = npad.pad_slot;
        if (slot >= XUSER_MAX_COUNT || npad.get_pad(slot, &state) != ERROR_SUCCESS) {
            // Keep the same controller until it disconnects; probe other slots
            // only when no controller is known, not on every guest read.
            for (slot = 0; slot < XUSER_MAX_COUNT; ++slot)
                if (slot != npad.pad_slot && npad.get_pad(slot, &state) == ERROR_SUCCESS) break;
        }
        if (slot != npad.pad_slot) npad_select_source(NPAD_NONE, 0);
        npad.pad_slot = slot;
        npad.next_pad_poll = now + (slot < XUSER_MAX_COUNT ? NPAD_SAMPLE_100NS : NPAD_PROBE_100NS);
        if (slot < XUSER_MAX_COUNT) {
            npad_select_source(NPAD_XINPUT, 0);
            if (!npad.detached) npad_sample_pad(&state.Gamepad);
        }
    }
    if (npad.pad_slot < XUSER_MAX_COUNT) return NULL;
    uint64_t keys[4];
    const uint32_t focus = dm_m4_window_keys(keys);
    npad_select_source(focus ? NPAD_KEYBOARD : NPAD_NONE, focus);
    if (focus && !npad.detached && now >= npad.next_key_sample) {
        npad_sample_keys(keys);
        npad.next_key_sample = now + NPAD_SAMPLE_100NS;
    }
    return NULL;
}

void hid_disconnect_npad(const uint32_t *id) {
    const uint32_t player = npad_read_id(id);
    AcquireSRWLockExclusive(&npad_lock);
    npad_require_initialized();
    if (player == npad.id && npad.source != NPAD_NONE) {
        // Detach the guest's assignment until an unplug/replug, a fresh
        // keyboard focus, another explicit file press, or Controller Support.
        npad.detached = 1;
        npad.available = 0;
        if (npad.source == NPAD_CTL) npad_ctl.phase = CTL_IDLE;
    }
    ReleaseSRWLockExclusive(&npad_lock);
}

uint32_t hid_get_npad_style(const uint32_t *id) {
    const uint32_t player = npad_read_id(id);
    AcquireSRWLockExclusive(&npad_lock);
    npad_require_initialized();
    uint32_t style = 0;
    if (player == npad.id) {
        (void)npad_poll(0);
        if (npad.source != NPAD_NONE && !npad.detached) style = NPAD_FULLKEY;
    }
    ReleaseSRWLockExclusive(&npad_lock);
    return style;
}

int hid_get_npad_states(struct npad_state *samples, int count, const uint32_t *id) {
    if (count < 0) dm_fail("hid: negative Npad state count");
    const uint32_t player = npad_read_id(id);
    AcquireSRWLockExclusive(&npad_lock);
    npad_require_initialized();
    unsigned n = 0;
    if (count && player == npad.id) {
        const char *edge = npad_poll(1);
        n = npad.available < (unsigned)count ? npad.available : (unsigned)count;
        if (n && !dm_guest_range(samples, n * sizeof(*samples), 1)) dm_fail("hid: invalid Npad history buffer");
        for (unsigned i = 0; i < n; ++i)
            samples[i] = npad.history[(npad.next + NPAD_HISTORY - 1 - i) % NPAD_HISTORY];
        if (edge && n) {
            const struct npad_state *s = &samples[0];
            printf("[hid] DM_CTL %s returned at %" PRIu64 " ms: id=%" PRIu32 " sample=%" PRId64
                   " buttons=%#" PRIx64 " sticks=(%" PRId32 ",%" PRId32 ",%" PRId32 ",%" PRId32 ")\n",
                   edge, npad_ctl.phase_at / 10000, player, s->sampling, s->buttons,
                   s->stick[0], s->stick[1], s->stick[2], s->stick[3]);
            fflush(stdout);
        }
    }
    ReleaseSRWLockExclusive(&npad_lock);
    return (int)n;
}

int hid_get_disconnected_npad_states(void *samples, int count, const uint32_t *id) {
    (void)samples;
    if (count < 0) dm_fail("hid: negative Npad state count");
    (void)npad_read_id(id);
    AcquireSRWLockShared(&npad_lock);
    npad_require_initialized();
    ReleaseSRWLockShared(&npad_lock);
    return 0; // Handheld and Joy-Con styles have no host source.
}

int hid_get_unavailable_npad_handles(void *handles, int count, const uint32_t *id, uint32_t styles) {
    (void)handles;
    (void)styles;
    if (count < 0) dm_fail("hid: negative Npad handle count");
    (void)npad_read_id(id);
    AcquireSRWLockShared(&npad_lock);
    npad_require_initialized();
    ReleaseSRWLockShared(&npad_lock);
    return 0; // Neither the keyboard nor the XInput backend exposes these peripherals.
}

uint32_t hid_show_controller_support(uint32_t out[2], const int8_t *arg) {
    if (!dm_guest_range(arg, 2, 0) || !dm_guest_range(out, 2 * sizeof(*out), 1))
        dm_fail("hid: invalid controller support arguments");
    AcquireSRWLockExclusive(&npad_lock);
    npad_require_initialized();
    (void)npad_poll(0);
    const uint32_t id = npad.id;
    const int ready = npad.source != NPAD_NONE && arg[0] <= 1 && arg[1] >= 1;
    if (ready && npad.detached) {
        // Opening Controller Support explicitly asks to assign a source.
        npad.detached = 0;
        npad.next_pad_poll = npad.next_key_sample = 0;
    }
    ReleaseSRWLockExclusive(&npad_lock);
    // The SDK copies the applet's result info only on success. Do not claim
    // that a player joined when no compatible controller is attached.
    if (!ready) return 202u | (3101u << 9); // ResultControllerSupportCanceled
    out[0] = 1; // playerCount
    out[1] = id;
    return 0;
}
