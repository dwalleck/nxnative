// SDK import implementations for the Windows bring-up host.
#include "m2_host.h"
#include "m3_save.h"
#include "m4_display.h"
#include "m4_draw.h"
#include "m4_phase.h"
#include "nvn2_info.h"
#include "../../host/account_users.h"
#include "../../gfx/nvn/include/nvn.h"
#include "../../gfx/tex/nvn_deswizzle.h"
#include "m4_gmem.h"
#include "m4_draw_state.h"
#define DM_ROMFS_WIN32
#include "../../host/romfs_image.h"
#include <assert.h>
#include <bcrypt.h>
#include <inttypes.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#define SECURITY_WIN32
#include <security.h>
#include <winternl.h>
#define SCHANNEL_USE_BLACKLISTS
#include <schannel.h>
#pragma comment(lib, "secur32.lib")

// Acquire a client TLS provider locally. This does not contact a server or
// select a user certificate. A handshake must still verify its peer.
static SRWLOCK ssl_lock = SRWLOCK_INIT;
static CredHandle ssl_credential;
static int ssl_active;
uint32_t ssl_initialize(void) {
    AcquireSRWLockExclusive(&ssl_lock);
    uint32_t result = 0x1927b; // SDK ResultAlreadyInitialized
    if (!ssl_active) {
        SCH_CREDENTIALS credentials = {.dwVersion = SCH_CREDENTIALS_VERSION, .dwFlags = SCH_CRED_NO_DEFAULT_CREDS};
        TimeStamp expiry;
        SECURITY_STATUS status = AcquireCredentialsHandleW(NULL, UNISP_NAME_W, SECPKG_CRED_OUTBOUND,
                                      NULL, &credentials, NULL, NULL, &ssl_credential, &expiry);
        ssl_active = status == SEC_E_OK;
        if (!ssl_active) fprintf(stderr, "[RED] ssl: AcquireCredentialsHandleW failed: %#lx\n", (unsigned long)status);
        result = ssl_active ? 0 : 0x247b;
    }
    ReleaseSRWLockExclusive(&ssl_lock);
    return result;
}
uint32_t ssl_finalize(void) {
    AcquireSRWLockExclusive(&ssl_lock);
    uint32_t result = 0xca7b; // SDK ResultNotInitialized
    if (ssl_active) {
        result = FreeCredentialsHandle(&ssl_credential) == SEC_E_OK ? 0 : 0x247b;
        if (!result) ssl_active = 0;
    }
    ReleaseSRWLockExclusive(&ssl_lock);
    return result;
}

#define FILE_SLOTS 256
#define DIRECTORY_SLOTS 256

static romfs_img *rom;
static char mount_name[FS_MOUNT_CAPACITY];
SRWLOCK m3_fs_lock = SRWLOCK_INIT;
struct fs_file_slot {
    uint64_t handle, index;    // image index or store-scoped token
    const struct fs_save_mount *save;
    unsigned mode;
};
struct fs_dir_slot {
    uint64_t handle, index, cursor;
    const struct fs_save_mount *save;
    unsigned mode;
};
static struct fs_file_slot files[FILE_SLOTS];
static struct fs_dir_slot directories[DIRECTORY_SLOTS];
static uint64_t next_handle = 16;
static int fs_trace;
static __declspec(thread) unsigned fs_priority;

void fs_set_priority(unsigned priority) {
    if (priority > 2) dm_fail("fs::SetPriorityOnCurrentThread: invalid priority %u", priority);
    // This hint accompanies Horizon filesystem requests, not the CPU thread.
    // Direct synchronous RomFS reads have no fsp-srv queue to reprioritize.
    fs_priority = priority;
}

int m3_fs_copy_path(const char *guest, char *out, size_t n) {
    if (!dm_guest_string(guest, out, n)) return 0;
    for (char *p = out; *p; ++p) if (*p == '\\') *p = '/';
    return 1;
}

int m3_fs_rom_mounted_locked(const char *name, size_t length) {
    return rom && length == strlen(mount_name) && !memcmp(name, mount_name, length);
}

int64_t m3_fs_rom_find_locked(const char *path, const char *rest) {
    // The guest path only selects entries in the validated image index.
    if (!rest || !m3_fs_rom_mounted_locked(path, (size_t)(rest - path - 1))) return -1;
    return romfs_image_find(rom, rest);
}

// All handle types get unique non-reused integers. Preserve 16-byte alignment:
// some guests borrow the low bits of SDK (normally pointer-valued) handles.
static uint64_t new_fs_handle(void) {
    uint64_t h = next_handle;
    if (!(next_handle += 16)) dm_fail("fs: exhausted handle identifiers");
    return h;
}

static struct fs_file_slot *file_slot(uint64_t h) {
    if (h) for (size_t i = 0; i < ARRAYSIZE(files); ++i) if (files[i].handle == h) return &files[i];
    return NULL;
}

static struct fs_dir_slot *dir_slot(uint64_t h) {
    if (h) for (size_t i = 0; i < ARRAYSIZE(directories); ++i)
        if (directories[i].handle == h) return &directories[i];
    return NULL;
}

int m3_fs_save_busy_locked(const struct fs_save_mount *mount, const m3_save_store *store) {
    for (size_t i = 0; i < ARRAYSIZE(files); ++i) {
        const struct fs_file_slot *f = &files[i];
        if (f->handle && f->save && (mount ? f->save == mount :
            (f->save->store == store && (f->mode & M3_SAVE_WRITE)))) return 1;
    }
    if (mount) for (size_t i = 0; i < ARRAYSIZE(directories); ++i)
        if (directories[i].handle && directories[i].save == mount) return 1;
    return 0;
}

uint32_t fs_cache_size(uint64_t *out) {
    if (!dm_guest_range(out, sizeof(*out), 1)) return FS_FAILURE;
    *out = 0x1000;  // Same minimum cache as the Apple host; the image owns its index.
    return FS_OK;
}

uint32_t fs_mount_rom(const char *guest, void *cache, uint64_t size) {
    (void)cache; (void)size;
    char name[sizeof(mount_name)];
    if (!m3_fs_copy_path(guest, name, sizeof(name)) || !*name || strpbrk(name, ":/")) return FS_FAILURE;
    AcquireSRWLockExclusive(&m3_fs_lock);
    uint32_t result;
    if (m3_fs_save_mount_locked(name, strlen(name))) result = FS_MOUNT_EXISTS;
    else if (rom) result = strcmp(name, mount_name) ? FS_EXISTS : FS_OK;
    else {
        DWORD capacity = GetEnvironmentVariableW(L"DM_ROMFS", NULL, 0);
        if (!capacity || capacity > 32768) dm_fail("fs::MountRom requires DM_ROMFS to point to a RomFS.img");
        wchar_t *wide = malloc((size_t)capacity * sizeof(*wide));
        if (!wide) dm_fail("fs::MountRom cannot allocate path buffer");
        DWORD n = GetEnvironmentVariableW(L"DM_ROMFS", wide, capacity);
        if (!n || n >= capacity) dm_fail("fs::MountRom: DM_ROMFS changed during mount");
        rom = romfs_image_open(wide);
        free(wide);
        if (!rom) dm_fail("fs::MountRom could not open a valid DMROMFS1 image (errno %d)", errno);
        wchar_t flag[2];
        fs_trace = GetEnvironmentVariableW(L"DM_FS", flag, ARRAYSIZE(flag)) == 1 && flag[0] == L'1';
        memcpy(mount_name, name, strlen(name) + 1);
        printf("[fs] mounted %s: %" PRIu64 " image entries\n", mount_name, rom->n);
        fflush(stdout);
        result = FS_OK;
    }
    ReleaseSRWLockExclusive(&m3_fs_lock);
    return result;
}

uint32_t fs_mount_rom_only(const char *name) { return fs_mount_rom(name, NULL, 0); }

uint32_t fs_entry_type(int32_t *out, const char *guest) {
    char path[FS_PATH_CAPACITY];
    if (!dm_guest_range(out, sizeof(*out), 1) || !m3_fs_copy_path(guest, path, sizeof(path))) return FS_FAILURE;
    AcquireSRWLockShared(&m3_fs_lock);
    const char *rest = NULL;
    const struct fs_save_mount *save = m3_fs_save_path_locked(path, &rest);
    uint32_t result = FS_NOT_FOUND;
    if (save) {
        m3_save_type type;
        result = m3_fs_save_result(m3_save_type_at(save->store, rest, &type));
        if (result == FS_OK) *out = (int32_t)type;
    } else {
        int64_t i = m3_fs_rom_find_locked(path, rest);
        if (i >= 0) { *out = rom->e[i].type; result = FS_OK; }
    }
    ReleaseSRWLockShared(&m3_fs_lock);
    return result;
}

uint32_t fs_open_file(uint64_t *out, const char *guest, unsigned mode) {
    char path[FS_PATH_CAPACITY];
    if (!dm_guest_range(out, sizeof(*out), 1) || !m3_fs_copy_path(guest, path, sizeof(path))) return FS_FAILURE;
    *out = 0;
    AcquireSRWLockExclusive(&m3_fs_lock);
    const char *rest = NULL;
    const struct fs_save_mount *save = m3_fs_save_path_locked(path, &rest);
    uint32_t result = FS_NOT_FOUND;
    if (save ? ((mode & ~7u) || !(mode & 3u)) : mode != M3_SAVE_READ) {
        result = save ? FS_INVALID_MODE : FS_FAILURE;
    } else {
        int64_t k = save ? -1 : m3_fs_rom_find_locked(path, rest);
        if (save || (k >= 0 && rom->e[k].type)) {
            size_t i = 0;
            while (i < ARRAYSIZE(files) && files[i].handle) ++i;
            result = FS_OPEN_LIMIT;
            if (i < ARRAYSIZE(files)) {
                uint64_t index;
                if (save) {
                    // AllowAppend has no effect without write permission.
                    int store_mode = (int)(mode & ((mode & M3_SAVE_WRITE) ? 7u : 3u));
                    result = m3_fs_save_result(m3_save_open_file(save->store, rest, store_mode, &index));
                } else {
                    img_ent *ent = &rom->e[k];
                    if ((ent->flags & IMG_ENT_DAMAGED) && !(ent->flags & IMG_ENT_REPORTED)) {
                        ent->flags |= IMG_ENT_REPORTED;
                        fprintf(stderr, "[RED] fs: damaged RomFS entry %s\n", path);
                    }
                    index = (uint64_t)k;
                    result = FS_OK;
                }
                if (result == FS_OK) {
                    files[i] = (struct fs_file_slot){new_fs_handle(), index, save, mode};
                    *out = files[i].handle;
                    if (fs_trace) { printf("[fs] open %s -> %#" PRIx64 "\n", path, *out); fflush(stdout); }
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&m3_fs_lock);
    return result;
}

uint32_t fs_file_size(int64_t *out, uint64_t h) {
    if (!dm_guest_range(out, sizeof(*out), 1)) return FS_FAILURE;
    AcquireSRWLockShared(&m3_fs_lock);
    struct fs_file_slot *f = file_slot(h);
    uint32_t result = FS_FAILURE;
    if (f) {
        if (f->save) result = m3_fs_save_result(m3_save_file_size(f->save->store, f->index, out));
        else { *out = (int64_t)rom->e[f->index].b; result = FS_OK; }
    }
    ReleaseSRWLockShared(&m3_fs_lock);
    return result;
}

static uint32_t read_file(size_t *out, uint64_t h, int64_t off, void *buf, size_t size, const uint32_t *opt) {
    if (out && !dm_guest_range(out, sizeof(*out), 1)) return FS_FAILURE;
    if (out) *out = 0;
    AcquireSRWLockShared(&m3_fs_lock);
    struct fs_file_slot *f = file_slot(h);
    uint32_t result = FS_FAILURE;
    if (f && f->save) {
        if (!size) result = FS_OK;
        else if (off < 0 || size > (uint64_t)(INT64_MAX - off)) result = FS_OUT_OF_RANGE;
        else if (!(f->mode & M3_SAVE_READ)) result = FS_READ_NOT_PERMITTED;
        else if (dm_guest_range(buf, size, 1) && (!opt || dm_guest_range(opt, sizeof(*opt), 0)))
            result = m3_fs_save_result(m3_save_read(f->save->store, f->index, off, buf, size, out));
    } else if (f && off >= 0) {
        const img_ent *ent = &rom->e[f->index];
        uint64_t left = (uint64_t)off < ent->b ? ent->b - (uint64_t)off : 0;
        uint64_t n = size < left ? size : left;
        if ((out || n == size) && (!n || (dm_guest_range(buf, n, 1) &&
            romfs_image_read(rom->fd, buf, n, (romfs_offset)(ent->a + (uint64_t)off))))) {
            if (out) *out = n;
            if (fs_trace) {
                printf("[fs] read %#" PRIx64 " + %#" PRIx64 ": %" PRIu64 " bytes (priority %u)\n",
                       h, (uint64_t)off, n, fs_priority);
                fflush(stdout);
            }
            result = FS_OK;
        }
    }
    ReleaseSRWLockShared(&m3_fs_lock);
    return result;
}

uint32_t fs_read(size_t *out, uint64_t h, int64_t off, void *buf, size_t size) {
    return out ? read_file(out, h, off, buf, size, NULL) : FS_FAILURE;
}
uint32_t fs_read_opts(size_t *out, uint64_t h, int64_t off, void *buf, size_t size, const uint32_t *opt) {
    return out ? read_file(out, h, off, buf, size, opt) : FS_FAILURE;
}
uint32_t fs_read_exact(uint64_t h, int64_t off, void *buf, size_t size) {
    return read_file(NULL, h, off, buf, size, NULL);
}
uint32_t fs_read_exact_opts(uint64_t h, int64_t off, void *buf, size_t size, const uint32_t *opt) {
    return read_file(NULL, h, off, buf, size, opt);
}

void fs_close(uint64_t h) {
    AcquireSRWLockExclusive(&m3_fs_lock);
    struct fs_file_slot *f = file_slot(h);
    if (f) {
        if (f->save) m3_save_close_file(f->save->store, f->index);
        f->handle = 0;
    }
    ReleaseSRWLockExclusive(&m3_fs_lock);
}

uint32_t fs_write(uint64_t h, int64_t off, const void *buf, size_t size, const uint32_t *opt) {
    if (size && (off < 0 || size > (uint64_t)(INT64_MAX - off))) return FS_OUT_OF_RANGE;
    if ((size && !dm_guest_range(buf, size, 0)) || !dm_guest_range(opt, sizeof(*opt), 0))
        return FS_FAILURE;
    const int flush = (*opt & 1u) != 0;
    AcquireSRWLockExclusive(&m3_fs_lock);
    struct fs_file_slot *f = file_slot(h);
    uint32_t result = FS_FAILURE;
    if (f) {
        if (size && !(f->mode & M3_SAVE_WRITE)) result = FS_WRITE_NOT_PERMITTED;
        else if (size) result = m3_fs_save_result(m3_save_write(f->save->store, f->index, off, buf, size));
        else result = FS_OK;
        if (result == FS_OK && flush && f->save)
            result = m3_fs_save_result(m3_save_flush(f->save->store, f->index));
    }
    ReleaseSRWLockExclusive(&m3_fs_lock);
    return result;
}

uint32_t fs_set_size(uint64_t h, int64_t size) {
    if (size < 0) return FS_OUT_OF_RANGE;
    AcquireSRWLockExclusive(&m3_fs_lock);
    struct fs_file_slot *f = file_slot(h);
    uint32_t result = FS_FAILURE;
    if (f) result = f->mode & M3_SAVE_WRITE ?
        m3_fs_save_result(m3_save_set_size(f->save->store, f->index, size)) : FS_WRITE_NOT_PERMITTED;
    ReleaseSRWLockExclusive(&m3_fs_lock);
    return result;
}

uint32_t fs_flush_file(uint64_t h) {
    AcquireSRWLockShared(&m3_fs_lock);
    const struct fs_file_slot *f = file_slot(h);
    uint32_t result = !f ? FS_FAILURE : f->save ?
        m3_fs_save_result(m3_save_flush(f->save->store, f->index)) : FS_OK;
    ReleaseSRWLockShared(&m3_fs_lock);
    return result;
}

unsigned fs_get_mode(uint64_t h) {
    AcquireSRWLockShared(&m3_fs_lock);
    const struct fs_file_slot *f = file_slot(h);
    unsigned mode = f ? f->mode : 0;
    ReleaseSRWLockShared(&m3_fs_lock);
    return mode;
}

uint32_t fs_open_directory(uint64_t *out, const char *guest, unsigned mode) {
    char path[FS_PATH_CAPACITY];
    if (!dm_guest_range(out, sizeof(*out), 1) || !m3_fs_copy_path(guest, path, sizeof(path))) return FS_FAILURE;
    *out = 0;
    if ((mode & ~(FS_LIST_NO_SIZE | 3u)) || !(mode & 3u)) return FS_INVALID_MODE;
    AcquireSRWLockExclusive(&m3_fs_lock);
    const char *rest = NULL;
    const struct fs_save_mount *save = m3_fs_save_path_locked(path, &rest);
    int64_t k = save ? -1 : m3_fs_rom_find_locked(path, rest);
    uint32_t result = FS_NOT_FOUND;
    if (save || (k >= 0 && !rom->e[k].type)) {
        size_t i = 0;
        while (i < ARRAYSIZE(directories) && directories[i].handle) ++i;
        result = FS_OPEN_LIMIT;
        if (i < ARRAYSIZE(directories)) {
            uint64_t index = (uint64_t)k;
            result = save ? m3_fs_save_result(m3_save_open_dir(save->store, rest, (int)(mode & 3u), &index)) : FS_OK;
            if (result == FS_OK) {
                directories[i] = (struct fs_dir_slot){new_fs_handle(), index, 0, save, mode};
                *out = directories[i].handle;
            }
        }
    }
    ReleaseSRWLockExclusive(&m3_fs_lock);
    return result;
}

static int listed_type(unsigned mode, unsigned type) {
    return (mode & (type == M3_SAVE_FILE ? M3_SAVE_LIST_FILE : M3_SAVE_LIST_DIR)) != 0;
}

uint32_t fs_read_directory(int64_t *count, struct fs_dir_entry *out, uint64_t h, int64_t max) {
    if (!dm_guest_range(count, sizeof(*count), 1)) return FS_FAILURE;
    *count = 0;
    if (max < 0 || (uint64_t)max > SIZE_MAX / sizeof(*out)) return FS_INVALID_ARGUMENT;
    if (max && !dm_guest_range(out, (size_t)max * sizeof(*out), 1)) return FS_FAILURE;
    AcquireSRWLockExclusive(&m3_fs_lock);
    struct fs_dir_slot *d = dir_slot(h);
    uint32_t result = FS_FAILURE;
    int64_t n = 0;
    if (d) {
        result = FS_OK;
        if (d->save) {
            while (n < max) {
                m3_save_entry entry;
                int64_t got;
                result = m3_fs_save_result(m3_save_read_dir(d->save->store, d->index, &entry, 1, &got));
                if (result != FS_OK || !got) break;
                struct fs_dir_entry *e = &out[n++];
                memset(e, 0, sizeof(*e));
                memcpy(e->name, entry.name, strlen(entry.name));
                e->type = (int8_t)entry.type;
                if (!(d->mode & FS_LIST_NO_SIZE)) e->size = entry.size;
            }
        } else {
            const img_ent *parent = &rom->e[d->index];
            while (n < max && d->cursor < parent->b) {
                const img_ent *entry = &rom->e[parent->a + d->cursor++];
                if (!listed_type(d->mode, entry->type)) continue;
                struct fs_dir_entry *e = &out[n++];
                memset(e, 0, sizeof(*e));
                memcpy(e->name, rom->names + entry->name, entry->nlen);
                e->type = (int8_t)entry->type;
                if (entry->type && !(d->mode & FS_LIST_NO_SIZE)) e->size = (int64_t)entry->b;
            }
        }
    }
    *count = n;
    ReleaseSRWLockExclusive(&m3_fs_lock);
    return result;
}

uint32_t fs_directory_count(int64_t *out, uint64_t h) {
    if (!dm_guest_range(out, sizeof(*out), 1)) return FS_FAILURE;
    AcquireSRWLockShared(&m3_fs_lock);
    const struct fs_dir_slot *d = dir_slot(h);
    uint32_t result = FS_FAILURE;
    if (d) {
        if (d->save) result = m3_fs_save_result(m3_save_dir_count(d->save->store, d->index, out));
        else {
            const img_ent *parent = &rom->e[d->index];
            int64_t count = 0;
            for (uint64_t i = 0; i < parent->b; ++i)
                count += listed_type(d->mode, rom->e[parent->a + i].type);
            *out = count;
            result = FS_OK;
        }
    }
    ReleaseSRWLockShared(&m3_fs_lock);
    return result;
}

void fs_close_directory(uint64_t h) {
    AcquireSRWLockExclusive(&m3_fs_lock);
    struct fs_dir_slot *d = dir_slot(h);
    if (d) {
        if (d->save) m3_save_close_dir(d->save->store, d->index);
        d->handle = 0;
    }
    ReleaseSRWLockExclusive(&m3_fs_lock);
}

void fs_unmount(const char *guest) {
    char name[sizeof(mount_name)];
    if (!m3_fs_copy_path(guest, name, sizeof(name))) return;
    AcquireSRWLockExclusive(&m3_fs_lock);
    if (m3_fs_rom_mounted_locked(name, strlen(name))) {
        for (size_t i = 0; i < ARRAYSIZE(files); ++i)
            if (files[i].handle && !files[i].save) dm_fail("fs: unmount with open RomFS files");
        for (size_t i = 0; i < ARRAYSIZE(directories); ++i)
            if (directories[i].handle && !directories[i].save) dm_fail("fs: unmount with open RomFS directories");
        romfs_image_close(rom);
        rom = NULL;
    } else m3_fs_save_unmount_locked(name);
    ReleaseSRWLockExclusive(&m3_fs_lock);
}

// Like the Apple host, the launcher owns the local account identity. Windows
// headless launches have one registered user and no selector or network account.
typedef struct { uint64_t uid[2], token; } account_handle;
static_assert(sizeof(account_handle) == 24, "account handle ABI");
static const uint64_t launch_uid[2] = {DM_UID0, DM_UID1};
static char launch_nickname[33] = "Player";
static INIT_ONCE account_once = INIT_ONCE_STATIC_INIT;
static SRWLOCK account_lock = SRWLOCK_INIT;
struct account_session { struct account_session *next; uint64_t token; };
static struct account_session *account_sessions;
static uint64_t next_account_token = UINT64_C(0x5553455200000001);

enum { ACCOUNT_FAILURE = 0x2a2, ACCOUNT_CANCELED = 124 | (1 << 9) };

static BOOL CALLBACK init_local_account(PINIT_ONCE once, void *arg, void **context) {
    (void)once; (void)arg; (void)context;
    wchar_t *name = NULL;
    size_t count = 0;
    if (!_wdupenv_s(&name, &count, L"DM_NICK") && name && count) {
        // Fit a complete UTF-8 prefix in the SDK's 32-byte nickname field.
        char utf8[32];
        int len = count > sizeof(utf8) ? (int)sizeof(utf8) : (int)count - 1, n = 0;
        while (len && !(n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name, len,
                                                utf8, (int)sizeof(utf8), NULL, NULL))) --len;
        memset(launch_nickname, 0, sizeof(launch_nickname));
        memcpy(launch_nickname, utf8, (size_t)n);
    }
    free(name);
    puts("[account] initialized local launch user");
    fflush(stdout);
    return TRUE;
}
void account_init(void) {
    if (!InitOnceExecuteOnce(&account_once, init_local_account, NULL, NULL)) dm_fail("cannot initialize local account");
}
uint32_t account_try_open(account_handle *out) {
    if (!dm_guest_range(out, sizeof(*out), 1)) return 0;
    account_init();
    struct account_session *s = malloc(sizeof(*s));
    if (!s) return 0;
    AcquireSRWLockExclusive(&account_lock);
    if (!next_account_token) dm_fail("exhausted local account session identifiers");
    s->token = next_account_token++;
    s->next = account_sessions;
    account_sessions = s;
    const account_handle opened = {{DM_UID0, DM_UID1}, s->token};
    memcpy(out, &opened, sizeof(opened));
    ReleaseSRWLockExclusive(&account_lock);
    return 1;
}
uint32_t account_list_open(int *count, void *out, int max) {
    if (!dm_guest_range(count, sizeof(*count), 1) || max < 0 ||
        (max && !dm_guest_range(out, sizeof(launch_uid), 1))) return ACCOUNT_FAILURE;
    AcquireSRWLockShared(&account_lock);
    *count = max && account_sessions;
    if (*count) memcpy(out, launch_uid, sizeof(launch_uid));
    ReleaseSRWLockShared(&account_lock);
    return 0;
}
uint32_t account_get_id(void *out, const account_handle *h) {
    // The SDK accessor just copies the UID, even for game-constructed handles.
    if (!dm_guest_range(out, sizeof(launch_uid), 1) || !dm_guest_range(h, sizeof(launch_uid), 0)) return ACCOUNT_FAILURE;
    memmove(out, h, sizeof(launch_uid));
    return 0;
}
uint32_t account_get_nick(char *out, const void *uid) {
    if (!dm_guest_range(out, sizeof(launch_nickname), 1) || !dm_guest_range(uid, sizeof(launch_uid), 0) ||
        dm_uid_user(uid) != 0) return ACCOUNT_FAILURE;
    account_init();
    memcpy(out, launch_nickname, sizeof(launch_nickname));
    return 0;
}
void account_close(const account_handle *guest) {
    if (!dm_guest_range(guest, sizeof(*guest), 0)) return;
    account_handle h;
    memcpy(&h, guest, sizeof(h));
    if (dm_uid_user(h.uid) != 0) return;
    AcquireSRWLockExclusive(&account_lock);
    struct account_session **link = &account_sessions;
    while (*link && (*link)->token != h.token) link = &(*link)->next;
    if (*link) {
        struct account_session *s = *link;
        *link = s->next;
        free(s);
    }
    ReleaseSRWLockExclusive(&account_lock);
}
uint32_t account_ensure_network(const account_handle *h) {
    (void)h;
    return ACCOUNT_CANCELED; // Same offline applet cancellation as Apple.
}

// This host mounts only the main RomFS. Don't claim that requested DLC is
// absent just because add-on mounts aren't implemented yet.
static void aoc_require_base_only(void) {
    if (GetEnvironmentVariableW(L"DM_AOC_DIR", NULL, 0) > 1)
        dm_fail("DM_AOC_DIR is set, but this host does not support add-on mounts");
}
int aoc_count(void) {
    aoc_require_base_only();
    return 0;
}
int aoc_list(int *indices, int offset, int capacity) {
    (void)indices; (void)offset; (void)capacity;
    aoc_require_base_only();
    return 0; // No entries written.
}

// Same single-process applet identity as DM_APPLET_RESOURCE_USER_ID on Apple.
uint64_t applet_resource_user_id(void) { return 0x80; }

// A direct headless launch has neither an applet sender nor a preselected
// user payload. Match the SDK's empty-channel branch: false, storage unchanged.
uint32_t applet_pop_launch_parameter(void **out, unsigned kind) {
    if (!dm_guest_range(out, sizeof(*out), 1) || (kind != 1 && kind != 2))
        dm_fail("TryPopFromApplicationParameterChannel: invalid storage or kind %u", kind);
    return 0;
}

#include "m3_input.inc"

#include "m3_time.inc"

uint32_t time_steady_timespec(uint64_t *out) {
    if (!dm_guest_range(out, 2 * sizeof(*out), 1)) dm_fail("steady clock: invalid timespec");
    ULONGLONG units;
    QueryInterruptTimePrecise(&units); // Monotonic time since boot in 100 ns units.
    out[0] = units / UINT64_C(10000000);
    out[1] = units % UINT64_C(10000000) * 100;
    return 0;
}
uint32_t time_wall_timespec(int64_t *out) {
    if (!dm_guest_range(out, 2 * sizeof(*out), 1)) dm_fail("system clock: invalid timespec");
    uint64_t t = wall_units();
    out[0] = (int64_t)(t / UINT64_C(10000000));
    out[1] = (int64_t)(t % UINT64_C(10000000)) * 100;
    return 0;
}
uint32_t nsd_environment(char *out) {
    if (!dm_guest_range(out, 8, 1)) dm_fail("nsd::GetEnvironmentIdentifier: invalid output");
    memcpy(out, "lp1\0\0\0\0", 8); // Retail environment, including while offline.
    return 0;
}

static volatile LONG joy_hold_type;
void hid_set_hold_type(int type) {
    if ((unsigned)type > 1) dm_fail("hid::SetNpadJoyHoldType: invalid orientation %d", type);
    InterlockedExchange(&joy_hold_type, type);
}
int hid_get_hold_type(void) { return ReadAcquire(&joy_hold_type); }

// No NFC reader is exposed by the Windows input backend. A real, initially
// clear SDK SystemEvent still has to be usable by the guest's multi-wait.
static volatile LONG nfp_initialized;
void nfp_initialize(void) { InterlockedExchange(&nfp_initialized, 1); }
void nfp_finalize(void) { InterlockedExchange(&nfp_initialized, 0); }
int nfp_state(void) { return ReadAcquire(&nfp_initialized) != 0; }
uint32_t nfp_list(void *devices, int *count, int capacity) {
    (void)devices;
    if (!nfp_state() || capacity < 0 || !dm_guest_range(count, sizeof(*count), 1))
        dm_fail("nfp::ListDevices: invalid request");
    *count = 0;
    return 0;
}
void nfp_availability_event(void *event) {
    const uint64_t args[8] = {(uintptr_t)event, 1, 1}; // Auto-clear, inter-process.
    uintptr_t fn = dm_guest_sdk_symbol("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    struct dm_guest_result r;
    if (!nfp_state() || !dm_guest_call(fn, args, &r) || (uint32_t)r.x0)
        dm_fail("nfp: cannot attach availability change event");
}

static volatile LONG mouse_initialized, keyboard_initialized;
void hid_init_mouse(void) { InterlockedExchange(&mouse_initialized, 1); }
int hid_get_mouse_states(void *samples, int count) {
    (void)samples;
    if (!ReadAcquire(&mouse_initialized) || count < 0) dm_fail("hid: invalid mouse history request");
    return 0; // No mouse source: no samples to write.
}
void hid_init_keyboard(void) { InterlockedExchange(&keyboard_initialized, 1); }
void hid_get_keyboard_state(void *out) {
    if (!ReadAcquire(&keyboard_initialized) || !dm_guest_range(out, 0x30, 1))
        dm_fail("hid: invalid keyboard state request");
    memset(out, 0, 0x30); // No keyboard source: disconnected, no modifiers or keys.
}

// Mirror the Apple host's driver-pool handoff. This host has no physical GPU device.
static volatile LONG nv_initialized;

void nv_init(void *pool, uint64_t size) {
    if ((pool || size) && (!size || !dm_guest_range(pool, size, 1)))
        dm_fail("nv::InitializeGraphics: invalid pool %p + %#" PRIx64, pool, size);
    InterlockedExchange(&nv_initialized, 1);
    printf("[nv] graphics pool %#" PRIx64 " bytes (no rendering)\n", size);
    fflush(stdout);
}

void nv_init_check(void) {
    if (!InterlockedCompareExchange(&nv_initialized, 0, 0)) dm_fail("nv::InitCheck before nv::InitializeGraphics");
}

// Minimal single-display VI state. The opaque window is not an HWND or a swapchain.
static uint32_t display, layer, window;
static int vi_initialized;

void vi_init(void) { vi_initialized = 1; }
uint32_t vi_open_display(void **out) {
    if (!vi_initialized || !dm_guest_range(out, sizeof(*out), 1)) return FS_FAILURE;
    *out = &display;
    return 0;
}
uint32_t vi_create_layer(void **out, const void *disp) {
    if (disp != &display || !dm_guest_range(out, sizeof(*out), 1)) return FS_FAILURE;
    *out = &layer;
    return 0;
}
uint32_t vi_native_window(void **out, const void *layer_arg) {
    if (layer_arg != &layer || !dm_guest_range(out, sizeof(*out), 1)) return FS_FAILURE;
    *out = &window;
    return 0;
}
// The guest display clock is fixed at 60 Hz (see nvn_window_acquire).
// No variable-refresh or extended-swap timing is active on this layer.
uint32_t vi_clear_extended_interval(const void *l) { return l == &layer ? 0 : FS_FAILURE; }
uint32_t vi_refresh_rate(int *hz, uint8_t *vrr, const void *d) {
    if (d != &display || !dm_guest_range(hz, sizeof(*hz), 1) || !dm_guest_range(vrr, 1, 1)) return FS_FAILURE;
    *hz = 60; *vrr = 0;
    return 0;
}
uint32_t vi_vrr_supported(uint8_t *supported, const void *d) {
    if (d != &display || !dm_guest_range(supported, 1, 1)) return FS_FAILURE;
    *supported = 0;
    return 0;
}
uint32_t vi_set_display_capability(const void *d, uint8_t enabled) {
    return d == &display && !enabled ? 0 : FS_FAILURE;
}

// NVN builder/device prefix from dm/gfx/nvn/nvn_obj.h. Do not touch the
// opaque storage past the fields this bootstrap owns.
struct nvn_builder { uint32_t flags; };
struct nvn_device { uint32_t magic, flags, origin, depth_mode; };
#define HEADLESS_NVN_DEVICE UINT32_C(0x00564544)
static SRWLOCK submit_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE submit_changed = CONDITION_VARIABLE_INIT;
static void queue_device_finalizing(const struct nvn_device *d);
static int device_valid(const struct nvn_device *d) {
    return dm_guest_range(d, sizeof(*d), 0) && d->magic == HEADLESS_NVN_DEVICE;
}
static struct nvn2_info nvn_info;

void dm_m3_nvn_load(const struct dm_module *sdk) {
    nvn_info = nvn2_find_info((const uint32_t *)sdk->base, sdk->text / 4, sdk->ro, sdk->data);
    int minor;
    if (nvn2_case(&nvn_info, 1, &minor)) printf("[nvn] SDK API 231.%d\n", minor);
}

void nvn_device_integer(const struct nvn_device *d, unsigned item, int *out) {
    if ((d && (!dm_guest_range(d, sizeof(*d), 0) || d->magic != HEADLESS_NVN_DEVICE)) ||
        !dm_guest_range(out, sizeof(*out), 1)) dm_fail("nvnDeviceGetInteger: invalid arguments");
    int value;
    // Headless reserves guest scratch from the SDK's sizing baseline; there
    // is no physical GPU description or backend shader execution to size for.
    int scratch_scale = item == NVN_DEVICE_INFO_SHADER_SCRATCH_MEMORY_SCALE_FACTOR_MINIMUM ||
                        item == NVN_DEVICE_INFO_SHADER_SCRATCH_MEMORY_SCALE_FACTOR_RECOMMENDED ||
                        item == NVN_DEVICE_INFO_SHADER_SCRATCH_MEMORY_COMPUTE_SCALE_FACTOR_MINIMUM;
    if (!nvn2_case(&nvn_info, item, &value) && !(scratch_scale && nvn2_default(&nvn_info, item, &value) && value > 0))
        dm_fail("headless NVN device query %#x has no decoded SDK constant", item);
    *out = value;
}

void nvn_builder_defaults(struct nvn_builder *b) {
    if (!dm_guest_range(b, sizeof(*b), 1)) dm_fail("nvnDeviceBuilderSetDefaults: invalid builder");
    *b = (struct nvn_builder){0};
}
void nvn_builder_flags(struct nvn_builder *b, uint32_t flags) {
    if (!dm_guest_range(b, sizeof(*b), 1)) dm_fail("nvnDeviceBuilderSetFlags: invalid builder");
    b->flags = flags;
}
uint32_t nvn_get_builder_flags(const struct nvn_builder *b) {
    if (!dm_guest_range(b, sizeof(*b), 0)) dm_fail("nvnDeviceBuilderGetFlags: invalid builder");
    return b->flags;
}
uint32_t nvn_device_init(struct nvn_device *d, const struct nvn_builder *b) {
    if (!dm_guest_range(d, sizeof(*d), 1) || !dm_guest_range(b, sizeof(*b), 0)) return 0;
    *d = (struct nvn_device){ .magic = HEADLESS_NVN_DEVICE, .flags = b->flags };
    puts("[nvn] initialized headless device (no queue or present)");
    fflush(stdout);
    return 1;
}
void nvn_device_origin(struct nvn_device *d, unsigned mode) {
    if (!dm_guest_range(d, sizeof(*d), 1) || d->magic != HEADLESS_NVN_DEVICE || mode > 1)
        dm_fail("nvnDeviceSetWindowOriginMode: invalid device or mode");
    d->origin = mode;
}
void nvn_device_depth(struct nvn_device *d, unsigned mode) {
    if (!dm_guest_range(d, sizeof(*d), 1) || d->magic != HEADLESS_NVN_DEVICE || mode > 1)
        dm_fail("nvnDeviceSetDepthMode: invalid device or mode");
    d->depth_mode = mode;
}
void nvn_device_fini(struct nvn_device *d) {
    AcquireSRWLockExclusive(&submit_lock);
    if (!dm_guest_range(d, sizeof(*d), 1) || d->magic != HEADLESS_NVN_DEVICE)
        dm_fail("nvnDeviceFinalize: invalid device");
    queue_device_finalizing(d);
    d->magic = 0;
    ReleaseSRWLockExclusive(&submit_lock);
}
NVNtextureHandle nvn_device_texture_handle(const struct nvn_device *d, int texture, int sampler) {
    if (!dm_guest_range(d, sizeof(*d), 0) || d->magic != HEADLESS_NVN_DEVICE)
        dm_fail("nvnDeviceGetTextureHandle: invalid device");
    // NVN2 reserves texture ID zero as a null handle. Other IDs are packed,
    // not resolved here: the command buffer selects the descriptor pools.
    if (!texture) return 0;
    return (uint64_t)(int64_t)texture | ((uint64_t)(int64_t)sampler << 20);
}
NVNtextureHandle nvn_device_texel_fetch_handle(const struct nvn_device *d, int texture) {
    return nvn_device_texture_handle(d, texture, 0);
}
NVNimageHandle nvn_device_image_handle(const struct nvn_device *d, int image) {
    if (!dm_guest_range(d, sizeof(*d), 0) || d->magic != HEADLESS_NVN_DEVICE)
        dm_fail("nvnDeviceGetImageHandle: invalid device");
    return (NVNimageHandle)(int64_t)image;
}
uint64_t nvn_device_timestamp(const struct nvn_device *d, const NVNcounterData *report) {
    if (!dm_guest_range(d, sizeof(*d), 0) || d->magic != HEADLESS_NVN_DEVICE ||
        !dm_guest_range(report, sizeof(*report), 0))
        dm_fail("nvnDeviceGetTimestampInNanoseconds: invalid device or counter data");
    // The NVN2 raw report field is already nanoseconds.
    uint64_t ns;
    memcpy(&ns, (const uint8_t *)report + offsetof(NVNcounterData, timestamp), sizeof(ns));
    return ns;
}
uint64_t nvn_device_current_timestamp(const struct nvn_device *d) {
    if (!dm_guest_range(d, sizeof(*d), 0) || d->magic != HEADLESS_NVN_DEVICE)
        dm_fail("nvnDeviceGetCurrentTimestampInNanoseconds: invalid device");
    return dm_guest_time_ns();
}
NVNboolean nvn_device_fast_clear_color(const struct nvn_device *d, const void *rgba, NVNformat format) {
    if (!dm_guest_range(d, sizeof(*d), 0) || d->magic != HEADLESS_NVN_DEVICE ||
        !dm_guest_range(rgba, 4 * sizeof(uint32_t), 0))
        dm_fail("nvnDeviceRegisterFastClearColor: invalid device or color");
    (void)format;
    // The legacy bool reports whether the optional hardware cache took the
    // value. Declining doesn't clear anything or affect later clear commands.
    return 0;
}

// Ordinary pools borrow committed guest storage; a fully mapped virtual
// pool can alias that storage. Neither address is a Vulkan device address.
enum { POOL_CPU_NO_ACCESS = 1, POOL_GPU_CACHED = 0x20, POOL_VIRTUAL = 0x200 };
#define HEADLESS_NVN_POOL UINT32_C(0x4c4f4f50)
struct pool_builder { const struct nvn_device *device; void *mem; size_t size; uint32_t flags; };
struct pool {
    uint32_t magic, flags;
    void *mem;
    size_t size;
    const struct nvn_device *device;
    struct pool *backing;
    size_t aliases;
};
static_assert(sizeof(struct pool) <= sizeof(NVNmemoryPool), "memory pool prefix");
static SRWLOCK pool_alias_lock = SRWLOCK_INIT;
struct descriptor_pool;
static void descriptor_tables_drop(const struct descriptor_pool *view, const struct pool *backing);
#include "m3_gmem_pools.inc"

static void pool_builder_check(const struct pool_builder *b, int write) {
    if (!dm_guest_range(b, sizeof(*b), write)) dm_fail("NVN memory pool: invalid builder");
}
static void pool_check(const struct pool *p, int write) {
    if (!dm_guest_range(p, sizeof(*p), write) || p->magic != HEADLESS_NVN_POOL)
        dm_fail("NVN memory pool: invalid pool");
}
static void *pool_storage(const struct pool *p, ptrdiff_t off, size_t size, size_t alignment, int write) {
    if (!dm_guest_range(p, sizeof(*p), 0) || p->magic != HEADLESS_NVN_POOL || !p->mem || off < 0 ||
        (size_t)off > p->size || !size || size > p->size - (size_t)off) return NULL;
    uintptr_t start = (uintptr_t)p->mem;
    if (start > UINTPTR_MAX - (size_t)off) return NULL;
    start += (size_t)off;
    if (start % alignment || !dm_guest_range((void *)start, size, write)) return NULL;
    return (void *)start;
}
void nvn_pool_defaults(struct pool_builder *b) {
    pool_builder_check(b, 1);
    b->mem = NULL;
    b->size = 0;
    b->flags = POOL_CPU_NO_ACCESS | POOL_GPU_CACHED;
}
void nvn_pool_device(struct pool_builder *b, const struct nvn_device *d) {
    pool_builder_check(b, 1);
    if (!dm_guest_range(d, sizeof(*d), 0) || d->magic != HEADLESS_NVN_DEVICE)
        dm_fail("nvnMemoryPoolBuilderSetDevice: invalid device");
    b->device = d;
}
void nvn_pool_storage(struct pool_builder *b, void *mem, size_t size) {
    pool_builder_check(b, 1);
    b->mem = mem;
    b->size = size;
}
void nvn_pool_flags(struct pool_builder *b, uint32_t flags) {
    pool_builder_check(b, 1);
    b->flags = flags;
}
uint32_t nvn_pool_init(struct pool *p, const struct pool_builder *b) {
    pool_builder_check(b, 0);
    const LONG64 generation = (LONG64)dm_guest_access_generation();
    const struct dm_m4_gmem_span span = nvn_gmem_span(b->mem, b->size);
    if (!dm_guest_range(p, sizeof(*p), 1) || !dm_guest_range(b->device, sizeof(*b->device), 0) ||
        b->device->magic != HEADLESS_NVN_DEVICE || !span.size) return 0;
    if (b->flags & POOL_VIRTUAL) {
        if (span.host || (b->flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL) || span.size % DM_PAGE) return 0;
    } else if (!dm_guest_range(span.host, span.size, 1)) return 0;
    AcquireSRWLockExclusive(&pool_alias_lock);
    struct nvn_live_pool **link = live_pool_insert_link(p, span.host);
    if (!link) {
        ReleaseSRWLockExclusive(&pool_alias_lock);
        return 0;
    }
    struct nvn_live_pool *entry = malloc(sizeof *entry);
    if (!entry) {
        ReleaseSRWLockExclusive(&pool_alias_lock);
        return 0;
    }
    *entry = (struct nvn_live_pool){*link, p, b->device, span, b->flags, span.host ? generation : 0};
    *link = entry;
    *p = (struct pool){.magic = HEADLESS_NVN_POOL, .flags = b->flags, .mem = span.host,
        .size = span.size, .device = b->device};
    ReleaseSRWLockExclusive(&pool_alias_lock);
    return 1;
}
struct pool_mapping {
    struct pool *physical;
    ptrdiff_t physical_offset, virtual_offset;
    size_t size;
    NVNstorageClass storage_class;
};
static_assert(sizeof(struct pool_mapping) == 0x28, "NVN mapping request ABI");
NVNboolean nvn_pool_map_virtual(struct pool *p, int count, const struct pool_mapping *requests) {
    AcquireSRWLockExclusive(&pool_alias_lock);
    pool_check(p, 1);
    if (!(p->flags & POOL_VIRTUAL) || count < 0) dm_fail("nvnMemoryPoolMapVirtual: invalid pool or count");
    if (!count) {
        ReleaseSRWLockExclusive(&pool_alias_lock);
        return 1;
    }
    if (count != 1 || !dm_guest_range(requests, sizeof(*requests), 0))
        dm_fail("nvnMemoryPoolMapVirtual: expected one readable whole-pool mapping");
    struct pool_mapping r = *requests;
    if (!r.physical || p->backing || r.virtual_offset || r.size != p->size)
        dm_fail("nvnMemoryPoolMapVirtual: partial mappings, remapping and unmapping are unsupported");
    pool_check(r.physical, 1);
    if (!(r.physical->flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL) || r.physical->device != p->device ||
        r.physical_offset % DM_PAGE)
        dm_fail("nvnMemoryPoolMapVirtual: invalid physical pool or offset");
    void *mem = pool_storage(r.physical, r.physical_offset, r.size, DM_PAGE, 1);
    if (!mem) dm_fail("nvnMemoryPoolMapVirtual: invalid physical range");
    struct nvn_live_pool **link = live_pool_link(p);
    struct nvn_live_pool *entry = *link;
    const struct nvn_live_pool *physical = *live_pool_link(r.physical);
    if (!entry || !physical || physical->span.host != r.physical->mem ||
        physical->span.size != r.physical->size || entry->device != physical->device)
        dm_fail("nvnMemoryPoolMapVirtual: unregistered or changed pool");
    *link = entry->next;
    entry->span = nvn_gmem_span(mem, r.size);
    entry->writable_generation = 0; // The virtual registration now denotes a different span.
    link = live_pool_insert_link(NULL, mem);
    entry->next = *link;
    *link = entry;
    // Preserve aliasing, not just the initial bytes. Page kinds are hardware
    // metadata; there is no GPU mapping in the headless backend.
    ++r.physical->aliases;
    p->backing = r.physical;
    p->mem = mem;
    ReleaseSRWLockExclusive(&pool_alias_lock);
    return 1;
}
void nvn_pool_fini(struct pool *p) {
    AcquireSRWLockExclusive(&pool_alias_lock);
    pool_check(p, 1);
    if (p->aliases) dm_fail("nvnMemoryPoolFinalize: physical pool still has virtual aliases");
    struct nvn_live_pool **link = live_pool_link(p);
    struct nvn_live_pool *entry = *link;
    if (!entry) dm_fail("nvnMemoryPoolFinalize: unregistered pool");
    *link = entry->next;
    if (entry->span.host && entry->span.size) {
        // The exclusive pool lock prevents draws from reacquiring this
        // registration between invalidation and removal.
        dm_guest_gpu_unmap_begin(entry->span.host, entry->span.size);
        dm_guest_gpu_unmap_end();
    }
    free(entry);
    descriptor_tables_drop(NULL, p);
    if (p->backing) --p->backing->aliases;
    *p = (struct pool){0};
    ReleaseSRWLockExclusive(&pool_alias_lock);
}
void *nvn_pool_map(const struct pool *p) {
    pool_check(p, 0);
    return p->flags & (POOL_CPU_NO_ACCESS | POOL_VIRTUAL) ? NULL : p->mem;
}
void nvn_pool_cache_range(const struct pool *p, ptrdiff_t offset, size_t size) {
    pool_check(p, 0);
    if (offset < 0 || (size_t)offset > p->size || size > p->size - (size_t)offset)
        dm_fail("NVN memory pool cache operation: invalid mapped range");
    // Coherent guest backing: replay stages CPU writes at submit and copies
    // GPU results back before completion. Neither direction needs a second copy.
}
NVNmemoryPoolFlags nvn_pool_get_flags(const struct pool *p) {
    pool_check(p, 0);
    return (NVNmemoryPoolFlags)p->flags;
}
size_t nvn_pool_get_size(const struct pool *p) {
    pool_check(p, 0);
    return p->size;
}
uint64_t nvn_pool_address(const struct pool *p) {
    if (!dm_guest_range(p, sizeof(*p), 0)) dm_fail("nvnMemoryPoolGetBufferAddress: invalid pool");
    if (p->magic != HEADLESS_NVN_POOL) {
        // NVN2 reads the address field directly. A zero-initialized inline
        // pool can be queried before its owner decides to initialize it.
        static const uint8_t empty[sizeof(*p)] = {0};
        if (!memcmp(p, empty, sizeof(empty))) return 0;
        dm_fail("nvnMemoryPoolGetBufferAddress: invalid pool");
    }
    if (!p->mem) dm_fail("nvnMemoryPoolGetBufferAddress: unmapped virtual pool is unsupported");
    return (uintptr_t)p->mem;
}

void nvn_object_debug_label(void *object, const char *label) {
    // Production NVN2 does not read either argument. Match that diagnostic
    // behavior without retaining a guest string or changing the object.
    (void)object;
    (void)label;
}

// Descriptor pools describe views of guest memory. Logical entries are
// host-owned and never written as GPU descriptors into that memory.
struct descriptor_pool {
    const struct pool *pool;
    ptrdiff_t offset;
    int count;
};
static_assert(sizeof(struct descriptor_pool) <= sizeof(NVNsamplerPool), "sampler pool prefix");
static_assert(sizeof(struct descriptor_pool) <= sizeof(NVNtexturePool), "texture pool prefix");

struct sampler_state {
    float lod_min, lod_max, lod_bias, aniso, lod_snap;
    uint32_t border[4], modes;
};
struct sampler_slot { struct sampler_state state; int populated; };
struct texture_binding;
struct descriptor_table {
    struct descriptor_table *next;
    const struct descriptor_pool *view;
    const struct pool *backing;
    uintptr_t begin, end;
    size_t stride;
    uint64_t generation;
    int reserved, count, sampler;
    struct sampler_slot *slots;
    struct texture_binding **textures;
};
static SRWLOCK descriptor_lock = SRWLOCK_INIT;
static struct descriptor_table *descriptor_tables;
static uint64_t descriptor_generation; // protected by descriptor_lock; zero means no live pool

// Caller holds descriptor_lock. Never load a host pointer from the guest object.
static struct descriptor_table *descriptor_table_find(const struct descriptor_pool *view, int sampler) {
    for (struct descriptor_table *t = descriptor_tables; t; t = t->next)
        if (t->view == view && t->sampler == sampler) return t;
    return NULL;
}
static void descriptor_tables_drop(const struct descriptor_pool *view, const struct pool *backing) {
    AcquireSRWLockExclusive(&descriptor_lock);
    struct descriptor_table *prev = NULL;
    for (struct descriptor_table *t = descriptor_tables, *next; t; t = next) {
        next = t->next;
        if (t->view != view && t->backing != backing) {
            prev = t;
            continue;
        }
        if (prev) prev->next = next;
        else descriptor_tables = next;
        if (t->textures) {
            for (int i = 0; i < t->count - t->reserved; ++i) free(t->textures[i]);
            free(t->textures);
        }
        free(t->slots);
        free(t);
    }
    ReleaseSRWLockExclusive(&descriptor_lock);
}
static void descriptor_table_add(const struct descriptor_pool *view, const struct pool *backing,
                                 uintptr_t begin, size_t stride, int reserved, int count, int sampler) {
    size_t n = (size_t)(count - reserved);
    size_t entry_size = sampler ? sizeof(struct sampler_slot) : sizeof(struct texture_binding *);
    if (n > SIZE_MAX / entry_size) dm_fail("NVN descriptor pool: table too large");
    AcquireSRWLockExclusive(&descriptor_lock);
    uintptr_t end = begin + (size_t)count * stride;
    for (const struct descriptor_table *p = descriptor_tables; p; p = p->next) {
        if (p->view == view) dm_fail("NVN descriptor pool: pool already initialized");
        if (p->sampler == sampler && begin < p->end && p->begin < end)
            dm_fail("NVN descriptor pool: overlapping pool views are not supported");
    }
    struct descriptor_table *t = calloc(1, sizeof(*t));
    if (!t) dm_fail("NVN descriptor pool: cannot allocate logical table");
    void *entries = n ? calloc(n, entry_size) : NULL;
    if (n && !entries) dm_fail("NVN descriptor pool: cannot allocate logical slots");
    *t = (struct descriptor_table){.next = descriptor_tables, .view = view, .backing = backing,
        .begin = begin, .end = end, .stride = stride, .reserved = reserved, .count = count, .sampler = sampler,
        .slots = sampler ? entries : NULL, .textures = sampler ? NULL : entries};
    t->generation = ++descriptor_generation;
    descriptor_tables = t;
    ReleaseSRWLockExclusive(&descriptor_lock);
}

static NVNboolean descriptor_init(struct descriptor_pool *out, const struct pool *pool,
                                  ptrdiff_t offset, int count, int sampler) {
    if (offset < 0 || count <= 0 || !dm_guest_range(out, sizeof(*out), 1) ||
        !dm_guest_range(pool, sizeof(*pool), 0) || pool->magic != HEADLESS_NVN_POOL) return 0;
    int stride, reserved, limit;
    if (!nvn2_case(&nvn_info, sampler ? NVN_DEVICE_INFO_SAMPLER_DESCRIPTOR_SIZE : NVN_DEVICE_INFO_TEXTURE_DESCRIPTOR_SIZE, &stride) ||
        !nvn2_case(&nvn_info, sampler ? NVN_DEVICE_INFO_RESERVED_SAMPLER_DESCRIPTORS : NVN_DEVICE_INFO_RESERVED_TEXTURE_DESCRIPTORS, &reserved) ||
        !nvn2_case(&nvn_info, sampler ? NVN_DEVICE_INFO_MAX_SAMPLER_POOL_SIZE : NVN_DEVICE_INFO_MAX_TEXTURE_POOL_SIZE, &limit) ||
        stride <= 0 || reserved <= 0 || limit < reserved)
        dm_fail("headless NVN %s pool: missing SDK descriptor limits", sampler ? "sampler" : "texture");
    size_t start = (size_t)offset, width = (size_t)stride;
    uintptr_t base = (uintptr_t)pool->mem;
    if (count < reserved || count > limit || start > pool->size || (size_t)count > (pool->size - start) / width ||
        start % width || base > UINTPTR_MAX - start || (base + start) % width ||
        !dm_guest_range((void *)(base + start), (size_t)count * width, 1)) return 0;
    descriptor_table_add(out, pool, base + start, width, reserved, count, sampler);
    *out = (struct descriptor_pool){pool, offset, count};
    return 1;
}
NVNboolean nvn_sampler_pool_init(struct descriptor_pool *out, const struct pool *pool, ptrdiff_t offset, int count) {
    return descriptor_init(out, pool, offset, count, 1);
}
NVNboolean nvn_texture_pool_init(struct descriptor_pool *out, const struct pool *pool, ptrdiff_t offset, int count) {
    return descriptor_init(out, pool, offset, count, 0);
}
static void descriptor_check(const struct descriptor_pool *d, int write) {
    if (!dm_guest_range(d, sizeof(*d), write) || !d->pool) dm_fail("NVN descriptor pool: invalid object");
}
void nvn_descriptor_fini(struct descriptor_pool *d) {
    descriptor_check(d, 1);
    descriptor_tables_drop(d, NULL);
    *d = (struct descriptor_pool){0};
}
const struct pool *nvn_descriptor_pool(const struct descriptor_pool *d) {
    descriptor_check(d, 0);
    return d->pool;
}
ptrdiff_t nvn_descriptor_offset(const struct descriptor_pool *d) {
    descriptor_check(d, 0);
    return d->offset;
}
int nvn_descriptor_count(const struct descriptor_pool *d) {
    descriptor_check(d, 0);
    return d->count;
}

// Store the sampler parameters (SDK 22.2.8 encodings, including raw border
// colour and LOD snap) inside the smallest public object prefix. Like the Apple
// HLE, the hashed parameter bytes start at +8. No GPU sampler is created here.
struct sampler_builder { const struct nvn_device *device; struct sampler_state state; };
struct sampler { uint32_t magic, pad; struct sampler_state state; };
#define HEADLESS_SAMPLER UINT32_C(0x00504d53)
static_assert(sizeof(struct sampler_builder) <= sizeof(NVNsamplerBuilder), "sampler builder prefix");
static_assert(sizeof(struct sampler) <= sizeof(NVNsampler), "sampler prefix");

static void sampler_builder_check(const struct sampler_builder *b, int write) {
    if (!dm_guest_range(b, sizeof(*b), write)) dm_fail("NVN sampler: invalid builder");
}
void nvn_sampler_defaults(struct sampler_builder *b) {
    sampler_builder_check(b, 1);
    b->state = (struct sampler_state){ .lod_max = 1000.f, .aniso = 1.f,
        .modes = NVN_WRAP_MODE_REPEAT | NVN_WRAP_MODE_REPEAT << 4 | NVN_WRAP_MODE_REPEAT << 8 |
                 NVN_MIN_FILTER_LINEAR << 16 | NVN_MAG_FILTER_LINEAR << 20 | NVN_COMPARE_FUNC_LESS << 26 };
}
void nvn_sampler_device(struct sampler_builder *b, const struct nvn_device *d) {
    sampler_builder_check(b, 1);
    if (!dm_guest_range(d, sizeof(*d), 0) || d->magic != HEADLESS_NVN_DEVICE) dm_fail("NVN sampler: invalid device");
    b->device = d;
}
void nvn_sampler_filter(struct sampler_builder *b, unsigned min, unsigned mag) {
    sampler_builder_check(b, 1);
    b->state.modes = (b->state.modes & ~UINT32_C(0x00ff0000)) | ((min & 15) << 16) | ((mag & 15) << 20);
}
void nvn_sampler_wrap(struct sampler_builder *b, unsigned s, unsigned t, unsigned r) {
    sampler_builder_check(b, 1);
    b->state.modes = (b->state.modes & ~UINT32_C(0xfff)) | (s & 15) | ((t & 15) << 4) | ((r & 15) << 8);
}
void nvn_sampler_clamp(struct sampler_builder *b, float lo, float hi) {
    sampler_builder_check(b, 1);
    b->state.lod_min = lo;
    b->state.lod_max = hi;
}
void nvn_sampler_bias(struct sampler_builder *b, float v) { sampler_builder_check(b, 1); b->state.lod_bias = v; }
void nvn_sampler_aniso(struct sampler_builder *b, float v) { sampler_builder_check(b, 1); b->state.aniso = v; }
void nvn_sampler_snap(struct sampler_builder *b, float v) { sampler_builder_check(b, 1); b->state.lod_snap = v; }
void nvn_sampler_compare(struct sampler_builder *b, unsigned mode, unsigned func) {
    sampler_builder_check(b, 1);
    b->state.modes = (b->state.modes & UINT32_C(0x00ffffff)) | ((mode & 3) << 24) | (func << 26);
}
void nvn_sampler_reduction(struct sampler_builder *b, unsigned mode) {
    sampler_builder_check(b, 1);
    b->state.modes = (b->state.modes & ~UINT32_C(0xf000)) | ((mode & 15) << 12);
}
void nvn_sampler_border(struct sampler_builder *b, const void *colour) {
    sampler_builder_check(b, 1);
    if (!dm_guest_range(colour, sizeof(b->state.border), 0)) dm_fail("NVN sampler: invalid border colour");
    memmove(b->state.border, colour, sizeof(b->state.border));
}
NVNboolean nvn_sampler_init(struct sampler *s, const struct sampler_builder *b) {
    sampler_builder_check(b, 0);
    if (!dm_guest_range(s, sizeof(*s), 1) || !dm_guest_range(b->device, sizeof(*b->device), 0) ||
        b->device->magic != HEADLESS_NVN_DEVICE) return 0;
    *s = (struct sampler){ .magic = HEADLESS_SAMPLER, .state = b->state };
    return 1;
}
void nvn_sampler_fini(struct sampler *s) {
    if (!dm_guest_range(s, sizeof(*s), 1) || s->magic != HEADLESS_SAMPLER) dm_fail("NVN sampler: invalid object");
    *s = (struct sampler){0};
}

static const struct sampler_state *sampler_params(const struct sampler *s) {
    if (!dm_guest_range(s, sizeof(*s), 0) || s->magic != HEADLESS_SAMPLER) dm_fail("NVN sampler: invalid object");
    return &s->state;
}
void nvn_sampler_get_min_mag(const struct sampler *s, NVNminFilter *min, NVNmagFilter *mag) {
    uint32_t modes = sampler_params(s)->modes;
    if (!dm_guest_range(min, sizeof(*min), 1) || !dm_guest_range(mag, sizeof(*mag), 1))
        dm_fail("nvnSamplerGetMinMagFilter: invalid output");
    *min = (NVNminFilter)((modes >> 16) & 15);
    *mag = (NVNmagFilter)((modes >> 20) & 15);
}
void nvn_sampler_get_wrap(const struct sampler *s, NVNwrapMode *x, NVNwrapMode *y, NVNwrapMode *z) {
    uint32_t modes = sampler_params(s)->modes;
    if (!dm_guest_range(x, sizeof(*x), 1) || !dm_guest_range(y, sizeof(*y), 1) || !dm_guest_range(z, sizeof(*z), 1))
        dm_fail("nvnSamplerGetWrapMode: invalid output");
    *x = (NVNwrapMode)(modes & 15);
    *y = (NVNwrapMode)((modes >> 4) & 15);
    *z = (NVNwrapMode)((modes >> 8) & 15);
}
void nvn_sampler_get_clamp(const struct sampler *s, float *lo, float *hi) {
    const struct sampler_state p = *sampler_params(s);
    if (!dm_guest_range(lo, sizeof(*lo), 1) || !dm_guest_range(hi, sizeof(*hi), 1))
        dm_fail("nvnSamplerGetLodClamp: invalid output");
    *lo = p.lod_min;
    *hi = p.lod_max;
}
void nvn_sampler_get_compare(const struct sampler *s, NVNcompareMode *mode, NVNcompareFunc *func) {
    uint32_t modes = sampler_params(s)->modes;
    if (!dm_guest_range(mode, sizeof(*mode), 1) || !dm_guest_range(func, sizeof(*func), 1))
        dm_fail("nvnSamplerGetCompare: invalid output");
    *mode = (NVNcompareMode)((modes >> 24) & 3);
    *func = (NVNcompareFunc)(modes >> 26);
}
void nvn_sampler_get_border(const struct sampler *s, void *color) {
    const struct sampler_state *p = sampler_params(s);
    if (!dm_guest_range(color, sizeof(p->border), 1)) dm_fail("NVN sampler: invalid border output");
    memmove(color, p->border, sizeof(p->border));
}
float nvn_sampler_get_bias(const struct sampler *s) { return sampler_params(s)->lod_bias; }
float nvn_sampler_get_aniso(const struct sampler *s) { return sampler_params(s)->aniso; }
NVNsamplerReduction nvn_sampler_get_reduction(const struct sampler *s) {
    return (NVNsamplerReduction)((sampler_params(s)->modes >> 12) & 15);
}
void nvn_sampler_register(const struct descriptor_pool *d, int id, const struct sampler *s) {
    if (!dm_guest_range(s, sizeof(*s), 0) || s->magic != HEADLESS_SAMPLER)
        dm_fail("nvnSamplerPoolRegisterSampler: invalid sampler");
    AcquireSRWLockExclusive(&descriptor_lock);
    struct descriptor_table *t = descriptor_table_find(d, 1);
    if (!t) dm_fail("nvnSamplerPoolRegisterSampler: no live sampler pool");
    if (id < t->reserved || id >= t->count)
        dm_fail("nvnSamplerPoolRegisterSampler: slot %d outside writable range [%d, %d)", id, t->reserved, t->count);
    if (!dm_guest_range((void *)(t->begin + (size_t)id * t->stride), t->stride, 1))
        dm_fail("nvnSamplerPoolRegisterSampler: slot backing is no longer writable");
    t->slots[id - t->reserved] = (struct sampler_slot){s->state, 1};
    t->generation = ++descriptor_generation;
    ReleaseSRWLockExclusive(&descriptor_lock);
}

// Not an NVN export; resolved only for a --fixture guest.
uint32_t fixture_sampler_snapshot(const struct descriptor_pool *d, int id, struct sampler_state *out) {
    if (!dm_guest_range(out, sizeof(*out), 1)) dm_fail("fixture sampler snapshot: invalid output");
    uint32_t present = 0;
    AcquireSRWLockShared(&descriptor_lock);
    const struct descriptor_table *t = descriptor_table_find(d, 1);
    if (t && id >= t->reserved && id < t->count) {
        const struct sampler_slot *slot = &t->slots[id - t->reserved];
        if (slot->populated) {
            *out = slot->state;
            present = 1;
        }
    }
    ReleaseSRWLockShared(&descriptor_lock);
    return present;
}

// Queue builder state only. SDK 22.2.8 distinguishes an unset value from
// an explicitly provided zero, and SetDefaults leaves the device alone.
enum { QB_COMMAND, QB_COMPUTE, QB_CONTROL, QB_RAY, QB_FLUSH, QB_OPTION_COUNT };
struct queue_builder {
    const struct nvn_device *device;
    void *mem;
    uint32_t flags, size[QB_OPTION_COUNT], set, mem_size;
    int priority;
    const struct headless_queue *primary;
};
static_assert(sizeof(struct queue_builder) <= sizeof(NVNqueueBuilder), "queue builder prefix");

static void queue_builder_check(const struct queue_builder *b, int write) {
    if (!dm_guest_range(b, sizeof(*b), write)) dm_fail("NVN queue: invalid builder");
}
void nvn_queue_builder_defaults(struct queue_builder *b) {
    queue_builder_check(b, 1);
    // Don't read the device field; it need not have been set yet.
    size_t start = offsetof(struct queue_builder, mem);
    memset((unsigned char *)b + start, 0, sizeof(*b) - start);
    b->priority = 100;
}
void nvn_queue_builder_device(struct queue_builder *b, const struct nvn_device *d) {
    queue_builder_check(b, 1);
    if (!dm_guest_range(d, sizeof(*d), 0) || d->magic != HEADLESS_NVN_DEVICE)
        dm_fail("nvnQueueBuilderSetDevice: invalid device");
    b->device = d;
}
const struct nvn_device *nvn_queue_builder_get_device(const struct queue_builder *b) {
    queue_builder_check(b, 0);
    return b->device;
}
void nvn_queue_builder_flags(struct queue_builder *b, uint32_t f) { queue_builder_check(b, 1); b->flags = f; }
uint32_t nvn_queue_builder_get_flags(const struct queue_builder *b) { queue_builder_check(b, 0); return b->flags; }
void nvn_queue_builder_priority(struct queue_builder *b, int p) { queue_builder_check(b, 1); b->priority = p; }
int nvn_queue_builder_get_priority(const struct queue_builder *b) { queue_builder_check(b, 0); return b->priority; }

static void queue_builder_set_size(struct queue_builder *b, unsigned option, size_t size) {
    queue_builder_check(b, 1);
    if (size > INT32_MAX) dm_fail("NVN queue: builder size is larger than a signed 32-bit value");
    b->size[option] = (uint32_t)size;
    b->set |= 1u << option;
}
static NVNboolean queue_builder_get_size(const struct queue_builder *b, unsigned option, int *size) {
    queue_builder_check(b, 0);
    if (!dm_guest_range(size, sizeof(*size), 1)) dm_fail("NVN queue: invalid builder size output");
    *size = (int)b->size[option];
    return (NVNboolean)((b->set >> option) & 1u);
}
#define QB_SIZE(name, option) \
    void nvn_queue_builder_##name(struct queue_builder *b, size_t n) { queue_builder_set_size(b, option, n); } \
    NVNboolean nvn_queue_builder_get_##name(const struct queue_builder *b, int *n) { return queue_builder_get_size(b, option, n); }
QB_SIZE(command, QB_COMMAND)
QB_SIZE(compute, QB_COMPUTE)
QB_SIZE(control, QB_CONTROL)
QB_SIZE(ray, QB_RAY)
QB_SIZE(flush, QB_FLUSH)
#undef QB_SIZE

void nvn_queue_builder_memory(struct queue_builder *b, void *mem, size_t size) {
    queue_builder_check(b, 1);
    if (size > INT32_MAX) dm_fail("nvnQueueBuilderSetQueueMemory: size is larger than a signed 32-bit value");
    b->mem = mem;
    b->mem_size = (uint32_t)size;
}
void *nvn_queue_builder_get_memory(const struct queue_builder *b) {
    queue_builder_check(b, 0);
    return b->mem;
}
size_t nvn_queue_builder_get_memory_size(const struct queue_builder *b) {
    queue_builder_check(b, 0);
    return b->mem_size;
}

// Headless storage contract: command, compute and control reservations. The
// console also includes GPU-specific context overhead; no such context is
// allocated here. Use this same layout to size and validate explicit storage.
struct queue_layout { size_t offset[3], total; uint32_t size[3]; };
static int queue_layout(const struct queue_builder *b, struct queue_layout *l) {
    static const uint32_t defaults[] = {0x10000, 0x40000, 0x4000};
    static const uint32_t minimum[] = {0x10000, 0x4000, 0x1000};
    static const uint32_t alignment[] = {0x1000, 0x100, 0x1000};
    *l = (struct queue_layout){0};
    for (unsigned i = 0; i < ARRAYSIZE(defaults); ++i) {
        uint32_t n = (b->set & (1u << i)) ? b->size[i] : defaults[i];
        if ((n || i != QB_COMPUTE) && (n < minimum[i] || n > INT32_MAX || n % alignment[i])) return 0;
        l->total = (l->total + alignment[i] - 1) & ~(size_t)(alignment[i] - 1);
        l->offset[i] = l->total;
        l->size[i] = n;
        l->total += n;
    }
    if (l->size[QB_CONTROL] & (l->size[QB_CONTROL] - 1)) return 0;
    if ((b->set & (1u << QB_FLUSH)) && (b->size[QB_FLUSH] < 0x1000 || b->size[QB_FLUSH] > l->size[QB_COMMAND])) return 0;
    if ((b->set & (1u << QB_RAY)) && b->size[QB_RAY]) dm_fail("headless NVN queue: ray tracing memory is not supported");
    l->total = (l->total + DM_PAGE - 1) & ~(size_t)(DM_PAGE - 1);
    return l->total <= INT32_MAX;
}
size_t nvn_queue_builder_get_queue_memory_size(const struct queue_builder *b) {
    queue_builder_check(b, 0);
    struct queue_layout layout;
    if (!queue_layout(b, &layout)) dm_fail("nvnQueueBuilderGetQueueMemorySize: invalid queue memory settings");
    return layout.total;
}

#define HEADLESS_QUEUE UINT32_C(0x55455551)
struct headless_queue {
    uint32_t magic, origin, flags;
    int priority;
    const struct nvn_device *device;
    void *guest_memory;
    struct queue_layout layout;
    const struct headless_queue *primary;
};
static_assert(sizeof(struct headless_queue) <= sizeof(NVNqueue), "queue prefix");
static int queue_valid(const struct headless_queue *q) {
    return dm_guest_range(q, sizeof(*q), 0) && q->magic == HEADLESS_QUEUE && device_valid(q->device);
}
static void m4_draw_queue_drop(const struct headless_queue *q);
struct pending_queue;
static struct pending_queue *queue_pending(const struct headless_queue *q);
static void queue_finish_locked(const struct headless_queue *q);
static void queue_close_locked(const struct headless_queue *q);
void nvn_queue_builder_primary(struct queue_builder *b, const struct headless_queue *primary) {
    queue_builder_check(b, 1);
    if (primary && !queue_valid(primary)) dm_fail("nvnQueueBuilderSetPrimaryQueue: invalid queue");
    b->primary = primary;
}
const struct headless_queue *nvn_queue_builder_get_primary(const struct queue_builder *b) {
    queue_builder_check(b, 0);
    return b->primary;
}
NVNboolean nvn_queue_init(struct headless_queue *q, const struct queue_builder *b) {
    if (!dm_guest_range(q, sizeof(*q), 1) || !dm_guest_range(b, sizeof(*b), 0)) return 0;
    struct queue_layout layout;
    if (!queue_layout(b, &layout)) return 0;
    if (b->mem_size && (((uintptr_t)b->mem | b->mem_size) & (DM_PAGE - 1) ||
        b->mem_size < layout.total || !dm_guest_range(b->mem, b->mem_size, 1))) return 0;
    AcquireSRWLockExclusive(&submit_lock);
    if (!device_valid(b->device) || queue_pending(q) ||
        (b->primary && (b->primary == q || !queue_valid(b->primary) || b->primary->device != b->device))) {
        ReleaseSRWLockExclusive(&submit_lock);
        return 0;
    }
    *q = (struct headless_queue){ HEADLESS_QUEUE, b->device->origin, b->flags, b->priority,
                                  b->device, b->mem_size ? b->mem : NULL, layout, b->primary };
    ReleaseSRWLockExclusive(&submit_lock);
    printf("[nvn] initialized headless queue (%#zx bytes)\n", layout.total);
    fflush(stdout);
    return 1;
}
void nvn_queue_fini(struct headless_queue *q) {
    AcquireSRWLockExclusive(&submit_lock);
    if (!dm_guest_range(q, sizeof(*q), 1) || !queue_valid(q)) dm_fail("nvnQueueFinalize: invalid queue");
    queue_close_locked(q);
    *q = (struct headless_queue){0};
    m4_draw_queue_drop(q);
    ReleaseSRWLockExclusive(&submit_lock);
}

// Use the shared NVN layout for storage calculations; no GPU object is created.
struct texture_builder {
    const struct nvn_device *device;
    nvn_texdesc desc;
    const struct pool *pool;
    ptrdiff_t offset;
    uint8_t swizzle[4], depth_stencil_mode;
    const void *packaged_data;
};
struct headless_texture {
    uint32_t magic;
    nvn_texdesc desc;
    uint8_t swizzle[4];
    uint8_t depth_stencil_mode;
    const struct pool *pool;
    ptrdiff_t offset;
    nvn_texlayout layout;
};
// Newer SDKs allocate texture views in 16-byte slots.
enum { VIEW_LEVELS = 1, VIEW_LAYERS = 2, VIEW_FORMAT = 4, VIEW_TARGET = 8,
       VIEW_SWIZZLE = 16, VIEW_DEPTH_STENCIL = 32 };
struct texture_view {
    uint8_t set, target, base_level, levels;
    uint16_t format, base_layer, layers;
    uint8_t swizzle[4], depth_stencil_mode;
};
static_assert(sizeof(struct texture_view) == 16, "texture view prefix");
struct texture_binding { struct headless_texture texture; struct texture_view view; };
static const uint8_t identity_swizzle[4] = {
    NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_A,
};
#define HEADLESS_TEXTURE UINT32_C(0x00584554)
static_assert(sizeof(struct texture_builder) <= sizeof(NVNtextureBuilder), "texture builder prefix");
static_assert(sizeof(struct headless_texture) <= 0x80, "texture prefix used by newer SDKs");

static void texture_builder_check(const struct texture_builder *b, int write) {
    if (!dm_guest_range(b, sizeof(*b), write)) dm_fail("NVN texture: invalid builder");
}
void nvn_texture_defaults(struct texture_builder *b) {
    texture_builder_check(b, 1);
    b->desc = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .levels = 1, .gob_order = 1};
    b->pool = NULL;
    b->offset = 0;
    b->packaged_data = NULL;
    memcpy(b->swizzle, identity_swizzle, sizeof(b->swizzle));
    b->depth_stencil_mode = NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH;
}
void nvn_texture_device(struct texture_builder *b, const struct nvn_device *d) {
    texture_builder_check(b, 1);
    if (!dm_guest_range(d, sizeof(*d), 0) || d->magic != HEADLESS_NVN_DEVICE) dm_fail("nvnTextureBuilderSetDevice: invalid device");
    b->device = d;
}
#define TEX_OPT(name, field, type) \
    void nvn_texture_##name(struct texture_builder *b, type value) { texture_builder_check(b, 1); b->desc.field = value; }
TEX_OPT(target, target, int)
TEX_OPT(format, format, int)
TEX_OPT(flags, flags, int)
TEX_OPT(samples, samples, int)
TEX_OPT(levels, levels, uint32_t)
TEX_OPT(width, width, uint32_t)
TEX_OPT(height, height, uint32_t)
TEX_OPT(depth, depth, uint32_t)
#undef TEX_OPT
void nvn_texture_3d(struct texture_builder *b, uint32_t w, uint32_t h, uint32_t d) {
    texture_builder_check(b, 1);
    b->desc.width = w;
    b->desc.height = h;
    b->desc.depth = d;
}
void nvn_texture_2d(struct texture_builder *b, uint32_t w, uint32_t h) { nvn_texture_3d(b, w, h, 1); }
void nvn_texture_1d(struct texture_builder *b, uint32_t w) { nvn_texture_3d(b, w, 1, 1); }
void nvn_texture_stride(struct texture_builder *b, ptrdiff_t stride) {
    texture_builder_check(b, 1);
    if (stride < 0 || stride > INT32_MAX) dm_fail("nvnTextureBuilderSetStride: invalid stride");
    b->desc.stride = (uint32_t)stride;
}
void nvn_texture_packaged_layout(struct texture_builder *b, const NVNpackagedTextureLayout *layout) {
    texture_builder_check(b, 1);
    if (layout && !dm_guest_range(layout, sizeof(*layout), 0)) dm_fail("nvnTextureBuilderSetPackagedTextureLayout: invalid layout");
    b->desc.pkg_layout = layout ? (uint16_t)(1 + (layout->layout[0] | ((layout->layout[1] & 15) << 8))) : 0;
}
static void texture_set_swizzle(uint8_t out[4], NVNtextureSwizzle r, NVNtextureSwizzle g,
                                NVNtextureSwizzle blue, NVNtextureSwizzle a) {
    const NVNtextureSwizzle channels[] = {r, g, blue, a};
    for (unsigned i = 0; i < ARRAYSIZE(channels); ++i)
        if ((unsigned)channels[i] > NVN_TEXTURE_SWIZZLE_A) dm_fail("NVN texture swizzle: invalid channel");
    for (unsigned i = 0; i < ARRAYSIZE(channels); ++i) out[i] = (uint8_t)channels[i];
}
void nvn_texture_set_swizzle(struct texture_builder *b, NVNtextureSwizzle r, NVNtextureSwizzle g,
                             NVNtextureSwizzle blue, NVNtextureSwizzle a) {
    texture_builder_check(b, 1);
    texture_set_swizzle(b->swizzle, r, g, blue, a);
}
static void texture_read_swizzle(const uint8_t swizzle[4], NVNtextureSwizzle *r, NVNtextureSwizzle *g,
                                 NVNtextureSwizzle *blue, NVNtextureSwizzle *a) {
    NVNtextureSwizzle *out[] = {r, g, blue, a};
    for (unsigned i = 0; i < ARRAYSIZE(out); ++i) {
        if (!out[i]) continue;
        if (!dm_guest_range(out[i], sizeof(*out[i]), 1)) dm_fail("NVN texture swizzle: invalid output");
        *out[i] = (NVNtextureSwizzle)swizzle[i];
    }
}
void nvn_texture_builder_swizzle(const struct texture_builder *b, NVNtextureSwizzle *r, NVNtextureSwizzle *g,
                                 NVNtextureSwizzle *blue, NVNtextureSwizzle *a) {
    texture_builder_check(b, 0);
    texture_read_swizzle(b->swizzle, r, g, blue, a);
}
static void texture_check(const struct headless_texture *t, int write) {
    if (!dm_guest_range(t, sizeof(*t), write) || t->magic != HEADLESS_TEXTURE) dm_fail("NVN texture: invalid texture");
}
NVNtextureAddress nvn_texture_address(const struct headless_texture *t) {
    texture_check(t, 0);
    void *base = pool_storage(t->pool, t->offset, 1, 1, 0);
    if (!base) dm_fail("nvnTextureGetTextureAddress: invalid backing range");
    return (NVNtextureAddress)(uintptr_t)base;
}
NVNtextureFlags nvn_texture_get_flags(const struct headless_texture *t) {
    texture_check(t, 0);
    return (NVNtextureFlags)t->desc.flags;
}
NVNtextureTarget nvn_texture_get_target(const struct headless_texture *t) {
    texture_check(t, 0);
    return (NVNtextureTarget)t->desc.target;
}
NVNformat nvn_texture_get_format(const struct headless_texture *t) {
    texture_check(t, 0);
    return (NVNformat)t->desc.format;
}
ptrdiff_t nvn_texture_get_stride(const struct headless_texture *t) {
    texture_check(t, 0);
    return t->desc.stride;
}
#define TEXTURE_INT_GETTER(field) \
    int nvn_texture_get_##field(const struct headless_texture *t) { texture_check(t, 0); return (int)t->desc.field; }
TEXTURE_INT_GETTER(width)
TEXTURE_INT_GETTER(height)
TEXTURE_INT_GETTER(depth)
TEXTURE_INT_GETTER(levels)
TEXTURE_INT_GETTER(samples)
#undef TEXTURE_INT_GETTER
void nvn_texture_get_swizzle(const struct headless_texture *t, NVNtextureSwizzle *r, NVNtextureSwizzle *g,
                             NVNtextureSwizzle *blue, NVNtextureSwizzle *a) {
    texture_check(t, 0);
    texture_read_swizzle(t->swizzle, r, g, blue, a);
}
void nvn_texture_set_depth_stencil(struct texture_builder *b, NVNtextureDepthStencilMode mode) {
    texture_builder_check(b, 1);
    if (mode != NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH && mode != NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL)
        dm_fail("nvnTextureBuilderSetDepthStencilMode: invalid mode");
    b->depth_stencil_mode = (uint8_t)mode;
}
NVNtextureDepthStencilMode nvn_texture_builder_depth_stencil(const struct texture_builder *b) {
    texture_builder_check(b, 0);
    return (NVNtextureDepthStencilMode)b->depth_stencil_mode;
}
NVNtextureDepthStencilMode nvn_texture_get_depth_stencil(const struct headless_texture *t) {
    texture_check(t, 0);
    return (NVNtextureDepthStencilMode)t->depth_stencil_mode;
}
void nvn_texture_storage(struct texture_builder *b, const struct pool *pool, ptrdiff_t offset) {
    texture_builder_check(b, 1);
    b->pool = pool;
    b->offset = offset;
}
void nvn_texture_packaged_data(struct texture_builder *b, const void *data) {
    texture_builder_check(b, 1);
    b->packaged_data = data;
}
static int texture_layout(const nvn_texdesc *d, nvn_texlayout *layout) {
    if (d->target < 0 || d->target > NVN_TEXTURE_TARGET_BUFFER || !d->width || d->levels < 1 || d->levels > 16 ||
        d->width > (d->target == NVN_TEXTURE_TARGET_BUFFER ? 0x10000000u : 0x10000u) ||
        d->height > 0x10000u || d->depth > 0x10000u ||
        (d->samples != 0 && d->samples != 1 && d->samples != 2 && d->samples != 4 && d->samples != 8 && d->samples != 16)) return 0;
    nvn_tex_layout(d, layout);
    return layout->size != 0 && layout->size < DM_GUEST_LIMIT;
}
NVNstorageClass nvn_texture_storage_class(const struct texture_builder *b) {
    texture_builder_check(b, 0);
    // Like Metal, headless storage has no GPU page class. Do not use this
    // for physical-to-virtual mappings (unsupported) or raw page kinds.
    return (NVNstorageClass)0;
}
size_t nvn_texture_size(const struct texture_builder *b) {
    texture_builder_check(b, 0);
    nvn_texlayout l;
    if (!texture_layout(&b->desc, &l)) dm_fail("nvnTextureBuilderGetStorageSize: unsupported description (target %d format %#x)", b->desc.target, b->desc.format);
    return (size_t)l.size;
}
size_t nvn_texture_zcull_size(const struct texture_builder *b) {
    texture_builder_check(b, 0);
    return (size_t)nvn_zcull_reservation(&b->desc);
}
size_t nvn_texture_alignment(const struct texture_builder *b) {
    texture_builder_check(b, 0);
    nvn_texlayout l;
    if (!texture_layout(&b->desc, &l)) dm_fail("nvnTextureBuilderGetStorageAlignment: unsupported texture description");
    return l.align;
}
NVNboolean nvn_texture_init(struct headless_texture *t, const struct texture_builder *b) {
    if (!dm_guest_range(t, sizeof(*t), 1) || !dm_guest_range(b, sizeof(*b), 0) ||
        !dm_guest_range(b->device, sizeof(*b->device), 0) || b->device->magic != HEADLESS_NVN_DEVICE) return 0;
    nvn_texlayout l;
    if (!texture_layout(&b->desc, &l)) return 0;
    void *dst = pool_storage(b->pool, b->offset, (size_t)l.size, l.align, 1);
    if (!dst) return 0;
    // Packaged data already uses the NVN mip/GOB layout, including padding.
    if (b->packaged_data && b->packaged_data != dst) {
        if (!dm_guest_range(b->packaged_data, (size_t)l.size, 0)) return 0;
        memmove(dst, b->packaged_data, (size_t)l.size);
    }
    *t = (struct headless_texture){.magic = HEADLESS_TEXTURE, .desc = b->desc,
        .depth_stencil_mode = b->depth_stencil_mode, .pool = b->pool, .offset = b->offset, .layout = l};
    t->desc.gob_order = (uint16_t)((b->device->flags & 0x10000u) == 0);
    memcpy(t->swizzle, b->swizzle, sizeof(t->swizzle));
    return 1;
}
void nvn_texture_fini(struct headless_texture *t) {
    texture_check(t, 1);
    *t = (struct headless_texture){0};
}
static void texture_view_check(const struct texture_view *v, int write) {
    if (!dm_guest_range(v, sizeof(*v), write)) dm_fail("NVN texture view: invalid object");
}
void nvn_texture_view_defaults(struct texture_view *v) {
    texture_view_check(v, 1);
    *v = (struct texture_view){0};
}
void nvn_texture_view_levels(struct texture_view *v, int base, int count) {
    texture_view_check(v, 1);
    if ((unsigned)base > UINT8_MAX || (unsigned)count > UINT8_MAX)
        dm_fail("nvnTextureViewSetLevels: invalid range");
    v->base_level = (uint8_t)base;
    v->levels = (uint8_t)count;
    v->set |= VIEW_LEVELS;
}
void nvn_texture_view_layers(struct texture_view *v, int base, int count) {
    texture_view_check(v, 1);
    if ((unsigned)base > UINT16_MAX || (unsigned)count > UINT16_MAX)
        dm_fail("nvnTextureViewSetLayers: invalid range");
    v->base_layer = (uint16_t)base;
    v->layers = (uint16_t)count;
    v->set |= VIEW_LAYERS;
}
void nvn_texture_view_format(struct texture_view *v, NVNformat format) {
    texture_view_check(v, 1);
    if ((unsigned)format > UINT16_MAX) dm_fail("nvnTextureViewSetFormat: invalid format");
    v->format = (uint16_t)format;
    v->set |= VIEW_FORMAT;
}
void nvn_texture_view_target(struct texture_view *v, NVNtextureTarget target) {
    texture_view_check(v, 1);
    if ((unsigned)target > NVN_TEXTURE_TARGET_BUFFER) dm_fail("nvnTextureViewSetTarget: invalid target");
    v->target = (uint8_t)target;
    v->set |= VIEW_TARGET;
}
void nvn_texture_view_swizzle(struct texture_view *v, NVNtextureSwizzle r, NVNtextureSwizzle g,
                              NVNtextureSwizzle blue, NVNtextureSwizzle a) {
    texture_view_check(v, 1);
    texture_set_swizzle(v->swizzle, r, g, blue, a);
    v->set |= VIEW_SWIZZLE;
}
void nvn_texture_view_depth_stencil(struct texture_view *v, NVNtextureDepthStencilMode mode) {
    texture_view_check(v, 1);
    if (mode != NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH && mode != NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL)
        dm_fail("nvnTextureViewSetDepthStencilMode: invalid mode");
    v->depth_stencil_mode = (uint8_t)mode;
    v->set |= VIEW_DEPTH_STENCIL;
}
static void view_get_range(int *base, int *count, int first, int length) {
    if (!dm_guest_range(base, sizeof(*base), 1) || !dm_guest_range(count, sizeof(*count), 1))
        dm_fail("NVN texture view: invalid range outputs");
    *base = first;
    *count = length;
}
NVNboolean nvn_texture_view_get_levels(const struct texture_view *v, int *base, int *count) {
    texture_view_check(v, 0);
    view_get_range(base, count, v->base_level, v->levels);
    return !!(v->set & VIEW_LEVELS);
}
NVNboolean nvn_texture_view_get_layers(const struct texture_view *v, int *base, int *count) {
    texture_view_check(v, 0);
    view_get_range(base, count, v->base_layer, v->layers);
    return !!(v->set & VIEW_LAYERS);
}
#define VIEW_GETTER(name, field, type, bit) \
    NVNboolean nvn_texture_view_get_##name(const struct texture_view *v, type *out) { \
        texture_view_check(v, 0); \
        if (out) { \
            if (!dm_guest_range(out, sizeof(*out), 1)) dm_fail("NVN texture view: invalid output"); \
            *out = (type)v->field; \
        } \
        return !!(v->set & bit); \
    }
VIEW_GETTER(format, format, NVNformat, VIEW_FORMAT)
VIEW_GETTER(target, target, NVNtextureTarget, VIEW_TARGET)
VIEW_GETTER(depth_stencil, depth_stencil_mode, NVNtextureDepthStencilMode, VIEW_DEPTH_STENCIL)
#undef VIEW_GETTER
NVNboolean nvn_texture_view_get_swizzle(const struct texture_view *v, NVNtextureSwizzle *r, NVNtextureSwizzle *g,
                                        NVNtextureSwizzle *blue, NVNtextureSwizzle *a) {
    texture_view_check(v, 0);
    texture_read_swizzle(v->swizzle, r, g, blue, a);
    return !!(v->set & VIEW_SWIZZLE);
}
static uint32_t mip_dim(uint32_t size, unsigned level) {
    uint32_t result = size >> level;
    return result ? result : 1;
}
struct texture_region { unsigned x, y, z, w, h, images, level, layer, width, height; };
static struct texture_region texture_region_geometry(const struct headless_texture *t, const struct texture_view *v,
                                                      NVNcopyRegion r, const char *operation) {
    const nvn_texdesc *d = &t->desc;
    unsigned level = v && (v->set & VIEW_LEVELS) ? v->base_level : 0;
    if (level >= d->levels || r.xoffset < 0 || r.yoffset < 0 || r.zoffset < 0 ||
        r.width <= 0 || r.height <= 0 || r.depth < 0)
        dm_fail("%s: invalid region", operation);
    if (v && (v->set & VIEW_LEVELS) && v->levels > d->levels - level)
        dm_fail("%s: invalid view levels", operation);
    int is_3d = d->target == NVN_TEXTURE_TARGET_3D;
    int one_d = d->target == NVN_TEXTURE_TARGET_1D || d->target == NVN_TEXTURE_TARGET_1D_ARRAY;
    unsigned width = mip_dim(d->width, level), height = one_d ? 1 : mip_dim(d->height, level);
    unsigned zlimit = is_3d ? mip_dim(d->depth, level) : t->layout.layers;
    unsigned layer0 = 0, images = r.depth ? (unsigned)r.depth : 1;
    if (!is_3d && v && (v->set & VIEW_LAYERS)) {
        layer0 = v->base_layer;
        if (layer0 >= zlimit || v->layers > zlimit - layer0) dm_fail("%s: invalid view layers", operation);
        zlimit = v->layers ? v->layers : zlimit - layer0;
    }
    unsigned x = (unsigned)r.xoffset, y = (unsigned)r.yoffset, z = (unsigned)r.zoffset;
    unsigned w = (unsigned)r.width, h = (unsigned)r.height;
    if (x >= width || w > width - x || y >= height || h > height - y || z >= zlimit || images > zlimit - z)
        dm_fail("%s: region exceeds the subresource", operation);
    return (struct texture_region){ x, y, z, w, h, images, level, layer0, width, height };
}
static struct texture_region texture_region_check(const struct headless_texture *t, const struct texture_view *v,
                                                   const NVNcopyRegion *region, const char *operation) {
    texture_check(t, 0);
    if (v) texture_view_check(v, 0);
    if (!dm_guest_range(region, sizeof(*region), 0)) dm_fail("%s: invalid region pointer", operation);
    return texture_region_geometry(t, v, *region, operation);
}
void nvn_texture_flush_texels(const struct headless_texture *t, const struct texture_view *v,
                              const NVNcopyRegion *region) {
    (void)texture_region_check(t, v, region, "nvnTextureFlushTexels");
    if (!pool_storage(t->pool, t->offset, (size_t)t->layout.size, t->layout.align, 0))
        dm_fail("nvnTextureFlushTexels: invalid storage");
    // CPU-backed headless storage has no separate GPU copy or cache.
}
#include "m3_texture_transfer.inc"
void nvn_texture_write_texels(const struct headless_texture *t, const struct texture_view *v,
                              const NVNcopyRegion *region, const uint8_t *src) {
    const char *operation = "nvnTextureWriteTexels";
    struct texture_region r = texture_region_check(t, v, region, operation);
    texture_write_region(t, v, r, src, 0, 0, operation);
}
static void texture_register_descriptor(const struct descriptor_pool *d, int id, const struct headless_texture *texture,
                                        const struct texture_view *view, const char *name) {
    if (view) texture_view_check(view, 0);
    texture_check(texture, 0);
    AcquireSRWLockExclusive(&descriptor_lock);
    struct descriptor_table *t = descriptor_table_find(d, 0);
    if (!t) dm_fail("%s: no live texture pool", name);
    if (id < t->reserved || id >= t->count)
        dm_fail("%s: slot %d outside writable range [%d, %d)", name, id, t->reserved, t->count);
    if (!dm_guest_range((void *)(t->begin + (size_t)id * t->stride), t->stride, 1))
        dm_fail("%s: slot backing is no longer writable", name);
    struct texture_binding **slot = &t->textures[id - t->reserved];
    if (!*slot) *slot = malloc(sizeof(**slot));
    if (!*slot) dm_fail("%s: cannot allocate logical descriptor", name);
    **slot = (struct texture_binding){.texture = *texture};
    if (view) (*slot)->view = *view;
    t->generation = ++descriptor_generation;
    ReleaseSRWLockExclusive(&descriptor_lock);
}
void nvn_texture_register(const struct descriptor_pool *d, int id, const struct headless_texture *texture,
                          const struct texture_view *view) {
    texture_register_descriptor(d, id, texture, view, "nvnTexturePoolRegisterTexture");
}
void nvn_texture_register_image(const struct descriptor_pool *d, int id, const struct headless_texture *texture,
                                const struct texture_view *view) {
    // The active texture pool's index space serves both sampled and image handles.
    texture_register_descriptor(d, id, texture, view, "nvnTexturePoolRegisterImage");
}

// NVN2 builder metadata only. A shader array remains caller-owned until
// program initialization; the headless host does not run or compile it.
struct program_builder {
    const struct nvn_device *device;
    const NVNshaderData *shaders, *rt_modules;
    int shader_count, rt_count;
    uint32_t trace_depth, callable_depth, rt_flags;
};
static_assert(sizeof(struct program_builder) <= 0x48, "NVN2 program builder prefix");
static void program_builder_check(const struct program_builder *b, int write) {
    if (!dm_guest_range(b, sizeof(*b), write)) dm_fail("NVN program: invalid builder");
}
void nvn_program_builder_defaults(struct program_builder *b) {
    program_builder_check(b, 1);
    size_t start = offsetof(struct program_builder, shaders);
    memset((uint8_t *)b + start, 0, sizeof(*b) - start);
    b->trace_depth = b->callable_depth = 2;
}
void nvn_program_builder_device(struct program_builder *b, const struct nvn_device *d) {
    program_builder_check(b, 1);
    if (!dm_guest_range(d, sizeof(*d), 0) || d->magic != HEADLESS_NVN_DEVICE)
        dm_fail("nvnProgramBuilderSetDevice: invalid device");
    b->device = d;
}
void nvn_program_builder_shaders(struct program_builder *b, int count, const NVNshaderData *shaders) {
    program_builder_check(b, 1);
    if (count < 0 || count > 6 || (count && !dm_guest_range(shaders, (size_t)count * sizeof(*shaders), 0)))
        dm_fail("nvnProgramBuilderSetShaders: invalid shader array");
    b->shader_count = count;
    b->shaders = shaders;
}

enum { NVN2_KEY_CONTROL_BYTES = 0x48, HEADLESS_STAGE_COUNT = 8 };
#define HEADLESS_PROGRAM UINT32_C(0x474f5250)
struct headless_program {
    uint32_t magic, stage_mask;
    const struct nvn_device *device;
    uint8_t keys[HEADLESS_STAGE_COUNT][8];
    uint8_t point_size_outputs; // NVN stage bits: the SPH exports per-vertex PointSize.
    uint8_t rate_index_outputs; // NVN stage bits: the SPH exports the VPRS table index.
};
// A guest copy of the program also copies its identities. Do not retain guest
// shader, control or builder pointers after successful initialization.
static_assert(sizeof(struct headless_program) <= 0x70, "program prefix");
static uint32_t shader_word(const uint8_t *p) {
    uint32_t word;
    memcpy(&word, p, sizeof(word));
    return word;
}
static int nvn2_shader_stage(const uint8_t *ctl, uint32_t ctl_len, const uint8_t *code, uint32_t code_len) {
    // A zero length is the SDK's in-memory form. Packaged short controls
    // (0x48..0x56 bytes) have no stage byte; use the graphics SPH instead.
    if ((ctl_len == 0 || ctl_len > 0x56) && dm_guest_range(ctl, 0x57, 0)) {
        if (ctl[0x56] > 7) return -1; // Dedicated RT modules are not supported.
        if (ctl[0x56] >= 5) return ctl[0x56];
    }
    if (code_len < 0x80) return -1;
    uint32_t sph = shader_word(code), type = (sph >> 10) & 15;
    static const uint8_t stages[] = {0, 0, 3, 4, 2, 1}; // SPH order -> NVN stage order.
    if (type >= ARRAYSIZE(stages) || (!type && (sph & 31) != 1)) return -1;
    return stages[type];
}
static void nvn2_shader_key(uint8_t out[8], uint8_t prefix[NVN2_KEY_CONTROL_BYTES],
                           const uint8_t *code, uint32_t code_len) {
    // Same key as hle_nvn.c and shader_split.py: normalize the control
    // length so that file-backed and in-memory sections have one identity.
    memset(prefix + 0x30, 0, sizeof(uint32_t));
    BCRYPT_HASH_HANDLE hash;
    if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, NULL, 0, NULL, 0, 0) < 0)
        dm_fail("NVN program: cannot create SHA-256 state");
    uint8_t digest[32];
    NTSTATUS status = BCryptHashData(hash, prefix, NVN2_KEY_CONTROL_BYTES, 0);
    if (status >= 0) status = BCryptHashData(hash, (PUCHAR)code, code_len, 0);
    if (status >= 0) status = BCryptFinishHash(hash, digest, sizeof(digest), 0);
    BCryptDestroyHash(hash);
    if (status < 0) dm_fail("NVN program: cannot hash shader metadata");
    memcpy(out, digest, 8);
}
static NVNboolean program_set(struct headless_program *out, const struct nvn_device *device,
                              int count, const void *stages, size_t stride) {
    if (!dm_guest_range(out, sizeof(*out), 1) || !dm_guest_range(device, sizeof(*device), 0) ||
        device->magic != HEADLESS_NVN_DEVICE || count < 0 || count > 6 ||
        (count && !dm_guest_range(stages, (size_t)count * stride, 0))) return 0;
    struct headless_program p = {.magic = HEADLESS_PROGRAM, .device = device};
    for (int i = 0; i < count; ++i) {
        NVNshaderData shader;
        memcpy(&shader, (const uint8_t *)stages + (size_t)i * stride, sizeof(shader));
        const uint8_t *ctl = shader.control, *code = (const void *)(uintptr_t)shader.data;
        uint8_t prefix[NVN2_KEY_CONTROL_BYTES];
        if (!dm_guest_range(ctl, sizeof(prefix), 0)) return 0;
        memcpy(prefix, ctl, sizeof(prefix));
        if (shader_word(prefix) != UINT32_C(0x98761234) || shader_word(prefix + 4) != 0xc9) return 0;
        uint32_t ctl_len = shader_word(prefix + 0x30), code_len = shader_word(prefix + 0x44);
        if ((ctl_len && (ctl_len < sizeof(prefix) || ctl_len >= 0x10000 || !dm_guest_range(ctl, ctl_len, 0))) ||
            !code_len || code_len >= (1u << 24) || !dm_guest_range(code, code_len, 0)) return 0;
        int stage = nvn2_shader_stage(ctl, ctl_len, code, code_len);
        if (stage < 0 || (p.stage_mask & (1u << stage))) return 0;
        p.stage_mask |= 1u << stage;
        if ((stage == NVN_SHADER_STAGE_VERTEX || stage == DM_M4_NV_MESH_STAGE) &&
            code_len > 0x35 && (code[0x35] & 8u))
            p.point_size_outputs |= (uint8_t)(1u << stage);
        // Ampere SPHV4T1 OMAP_VPRS_TABLE_INDEX is bit 423. A mesh
        // shader may also export it in the second, per-primitive SPH.
        if ((1u << stage) & DM_M4_RATE_INDEX_STAGES) {
            int rate_index = code_len > 0x34 && (code[0x34] & 0x80u);
            if (stage == DM_M4_NV_MESH_STAGE && !rate_index &&
                (ctl_len >= 0x69 || (!ctl_len && dm_guest_range(ctl, 0x69, 0))) && (ctl[0x68] & 0x80u)) {
                uint32_t offset = shader_word(ctl + 0x64);
                if (offset > code_len || code_len - offset <= 0x34) return 0;
                rate_index = (code[offset + 0x34] & 0x80u) != 0;
            }
            if (rate_index) p.rate_index_outputs |= (uint8_t)(1u << stage);
        }
        nvn2_shader_key(p.keys[stage], prefix, code, code_len);
    }
    *out = p;
    return 1;
}
NVNboolean nvn_program_init(struct headless_program *out, const struct nvn_device *device) {
    return program_set(out, device, 0, NULL, sizeof(NVNshaderData));
}
NVNboolean nvn_program_shaders(struct headless_program *out, int count, const NVNshaderData *stages) {
    if (!dm_guest_range(out, sizeof(*out), 1) || out->magic != HEADLESS_PROGRAM) return 0;
    return program_set(out, out->device, count, stages, sizeof(*stages));
}
NVNboolean nvn_program_shaders_ext(struct headless_program *out, int count, const void *stages) {
    if (!dm_guest_range(out, sizeof(*out), 1) || out->magic != HEADLESS_PROGRAM) return 0;
    // NVN2 extracts {code, control} and a separate extension word per stage.
    // Like the offline translator, Vulkan identifies a stage by code and control.
    return program_set(out, out->device, count, stages, sizeof(NVNshaderData) + sizeof(uint64_t));
}
NVNboolean nvn_program_init_builder(struct headless_program *out, const struct program_builder *b) {
    program_builder_check(b, 0);
    if (b->rt_count) dm_fail("nvnProgramInitializeFromBuilder: ray-tracing programs are not implemented");
    return program_set(out, b->device, b->shader_count, b->shaders, sizeof(*b->shaders));
}
void nvn_program_fini(struct headless_program *p) {
    if (!dm_guest_range(p, sizeof(*p), 1) || p->magic != HEADLESS_PROGRAM)
        dm_fail("nvnProgramFinalize: invalid program");
    *p = (struct headless_program){0};
}

// Attribute arrays use the four-byte guest stride; format, byte offset, and
// stream index share a word just as in the Apple host's retained state.
struct vertex_attrib { uint32_t packed; };
struct vertex_stream { uint32_t stride; int divisor; };
static_assert(sizeof(struct vertex_attrib) == sizeof(NVNvertexAttribState), "vertex attribute stride");
static_assert(sizeof(struct vertex_stream) == sizeof(NVNvertexStreamState), "vertex stream stride");
void nvn_vertex_attrib_defaults(struct vertex_attrib *a) {
    if (!dm_guest_range(a, sizeof(*a), 1)) dm_fail("NVN vertex attribute: invalid state");
    a->packed = 0;
}
void nvn_vertex_attrib_format(struct vertex_attrib *a, NVNformat f, ptrdiff_t offset) {
    if (!dm_guest_range(a, sizeof(*a), 1) || (unsigned)f > UINT8_MAX || offset < 0 || offset >= (1 << 19))
        dm_fail("nvnVertexAttribStateSetFormat: invalid state, format or offset");
    a->packed = (a->packed & UINT32_C(0xf8000000)) | ((uint32_t)offset << 8) | (unsigned)f;
}
void nvn_vertex_attrib_stream(struct vertex_attrib *a, int stream) {
    if (!dm_guest_range(a, sizeof(*a), 1) || (unsigned)stream > 31)
        dm_fail("nvnVertexAttribStateSetStreamIndex: invalid state or stream");
    a->packed = (a->packed & UINT32_C(0x07ffffff)) | ((uint32_t)stream << 27);
}
void nvn_vertex_stream_defaults(struct vertex_stream *s) {
    if (!dm_guest_range(s, sizeof(*s), 1)) dm_fail("NVN vertex stream: invalid state");
    *s = (struct vertex_stream){0};
}
void nvn_vertex_stream_stride(struct vertex_stream *s, ptrdiff_t stride) {
    if (!dm_guest_range(s, sizeof(*s), 1) || stride < 0 || (size_t)stride > UINT32_MAX)
        dm_fail("nvnVertexStreamStateSetStride: invalid state or stride");
    s->stride = (uint32_t)stride;
}
void nvn_vertex_stream_divisor(struct vertex_stream *s, int divisor) {
    if (!dm_guest_range(s, sizeof(*s), 1) || divisor < 0)
        dm_fail("nvnVertexStreamStateSetDivisor: invalid state or divisor");
    s->divisor = divisor;
}

#include "m3_raster_state.inc"

struct headless_sync { uint32_t magic; const struct nvn_device *device; };
#define HEADLESS_SYNC UINT32_C(0x434e5953)
static_assert(sizeof(struct headless_sync) <= sizeof(NVNsync), "sync prefix");

// Only pending fences need a receipt. The sync's association, recorded work,
// and sleepers hold independent references under submit_lock. A re-fence
// never retargets a wait that already captured a receipt.
struct queue_receipt {
    struct queue_receipt *next;
    const struct headless_sync *owner;
    size_t refs;
    int complete;
};
static struct queue_receipt *sync_receipts;

static void receipt_drop(struct queue_receipt *r) {
    if (r && !--r->refs) free(r);
}
static struct queue_receipt *receipt_take(struct queue_receipt *r) {
    if (r) ++r->refs;
    return r;
}
static struct queue_receipt *receipt_new(void) {
    struct queue_receipt *r = calloc(1, sizeof(*r));
    if (!r) dm_fail("NVN queue: cannot allocate a completion receipt");
    r->refs = 1;
    return r;
}
static struct queue_receipt *sync_pending(const struct headless_sync *s) {
    for (struct queue_receipt *r = sync_receipts; r; r = r->next)
        if (r->owner == s) return r;
    return NULL;
}
static void sync_clear(const struct headless_sync *s) {
    struct queue_receipt **at = &sync_receipts;
    while (*at && (*at)->owner != s) at = &(*at)->next;
    if (!*at) return;
    struct queue_receipt *r = *at;
    *at = r->next;
    r->owner = NULL;
    receipt_drop(r);
}
static struct queue_receipt *sync_fence_pending(const struct headless_sync *s) {
    sync_clear(s);
    struct queue_receipt *r = receipt_new();
    r->owner = s;
    r->next = sync_receipts;
    sync_receipts = receipt_take(r);
    return r;
}
static void receipt_complete(struct queue_receipt *r) {
    if (r->owner) sync_clear(r->owner);
    r->complete = 1;
    WakeAllConditionVariable(&submit_changed);
    receipt_drop(r);
}
static int sync_valid(const struct headless_sync *s) {
    return dm_guest_range(s, sizeof(*s), 0) && s->magic == HEADLESS_SYNC && device_valid(s->device);
}
static void wait_for_submit_change(DWORD ms) {
    if (!SleepConditionVariableSRW(&submit_changed, &submit_lock, ms, 0) && GetLastError() != ERROR_TIMEOUT)
        dm_fail("NVN queue: condition wait failed (%lu)", GetLastError());
}
NVNboolean nvn_sync_init(struct headless_sync *s, const struct nvn_device *d) {
    AcquireSRWLockExclusive(&submit_lock);
    int valid = dm_guest_range(s, sizeof(*s), 1) && device_valid(d);
    if (valid) {
        sync_clear(s);
        *s = (struct headless_sync){ HEADLESS_SYNC, d };
    }
    ReleaseSRWLockExclusive(&submit_lock);
    return (NVNboolean)valid;
}
void nvn_sync_fini(struct headless_sync *s) {
    AcquireSRWLockExclusive(&submit_lock);
    if (!dm_guest_range(s, sizeof(*s), 1) || !sync_valid(s)) dm_fail("nvnSyncFinalize: invalid sync object");
    sync_clear(s);
    *s = (struct headless_sync){0};
    ReleaseSRWLockExclusive(&submit_lock);
}
NVNsyncWaitResult nvn_sync_wait(const struct headless_sync *s, uint64_t timeout) {
    const uint64_t begin = dm_guest_time_ns();
    AcquireSRWLockExclusive(&submit_lock);
    NVNsyncWaitResult result = NVN_SYNC_WAIT_RESULT_FAILED;
    if (sync_valid(s)) {
        struct queue_receipt *r = receipt_take(sync_pending(s));
        result = r ? NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED : NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
        while (r && !r->complete) {
            DWORD ms = INFINITE;
            if (timeout != UINT64_MAX) {
                uint64_t elapsed = dm_guest_time_ns() - begin;
                if (elapsed >= timeout) break;
                uint64_t left = timeout - elapsed;
                uint64_t rounded = left / 1000000 + (left % 1000000 != 0);
                ms = rounded < INFINITE ? (DWORD)rounded : INFINITE - 1;
            }
            wait_for_submit_change(ms);
        }
        if (r && r->complete) result = NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED;
        receipt_drop(r);
    }
    ReleaseSRWLockExclusive(&submit_lock);
    return result;
}

enum { WINDOW_TEXTURES = 4 };
struct window_builder {
    const struct nvn_device *device;
    const void *native;
    const struct headless_texture *textures[WINDOW_TEXTURES];
    int count, active, interval;
};
struct headless_window {
    uint32_t magic, acquired;
    struct window_builder state;
    unsigned next, queued;
    uint64_t presents;
    LONGLONG available[WINDOW_TEXTURES], last_display, frequency;
    NVNrectangle crop;
};
#define HEADLESS_WINDOW UINT32_C(0x004e4957)
static_assert(sizeof(struct window_builder) <= sizeof(NVNwindowBuilder), "window builder prefix");
static_assert(sizeof(struct headless_window) <= sizeof(NVNwindow), "window prefix");
#include "m4_present.inc"
static void window_builder_check(const struct window_builder *b) {
    if (!dm_guest_range(b, sizeof(*b), 1)) dm_fail("NVN window: invalid builder");
}
void nvn_window_defaults(struct window_builder *b) {
    window_builder_check(b);
    size_t start = offsetof(struct window_builder, native);
    memset((unsigned char *)b + start, 0, sizeof(*b) - start);
    b->interval = 1;
}
void nvn_window_device(struct window_builder *b, const struct nvn_device *d) {
    window_builder_check(b);
    if (!dm_guest_range(d, sizeof(*d), 0) || d->magic != HEADLESS_NVN_DEVICE) dm_fail("nvnWindowBuilderSetDevice: invalid device");
    b->device = d;
}
void nvn_window_native(struct window_builder *b, const void *native) {
    window_builder_check(b);
    if (native && native != &window) dm_fail("nvnWindowBuilderSetNativeWindow: unknown VI window");
    b->native = native;
}
void nvn_window_textures(struct window_builder *b, int count, const struct headless_texture *const *textures) {
    window_builder_check(b);
    if (count < 1 || count > WINDOW_TEXTURES || !dm_guest_range(textures, (size_t)count * sizeof(*textures), 0))
        dm_fail("nvnWindowBuilderSetTextures: invalid texture list");
    const struct headless_texture *copy[WINDOW_TEXTURES] = {0};
    for (int i = 0; i < count; ++i) copy[i] = textures[i];
    for (int i = 0; i < WINDOW_TEXTURES; ++i) b->textures[i] = copy[i];
    b->count = count;
}
void nvn_window_interval(struct window_builder *b, int interval) {
    window_builder_check(b);
    if (interval < 0 || interval > 5) dm_fail("nvnWindowBuilderSetPresentInterval: invalid interval %d", interval);
    b->interval = interval;
}
void nvn_window_active(struct window_builder *b, int active) {
    window_builder_check(b);
    if (active < 1 || active > WINDOW_TEXTURES) dm_fail("nvnWindowBuilderSetNumActiveTextures: invalid count %d", active);
    b->active = active;
}
NVNboolean nvn_window_init(struct headless_window *w, const struct window_builder *b) {
    if (!dm_guest_range(w, sizeof(*w), 1) || !dm_guest_range(b, sizeof(*b), 0) ||
        !dm_guest_range(b->device, sizeof(*b->device), 0) || b->device->magic != HEADLESS_NVN_DEVICE ||
        (b->native && b->native != &window) || b->count < 1 || b->count > WINDOW_TEXTURES ||
        b->active < 0 || b->active > b->count) return 0;
    for (int i = 0; i < b->count; ++i) {
        const struct headless_texture *t = b->textures[i];
        if (!dm_guest_range(t, sizeof(*t), 0) || t->magic != HEADLESS_TEXTURE) return 0;
        for (int j = 0; j < i; ++j) if (b->textures[j] == t) return 0;
    }
    LARGE_INTEGER freq;
    if (!QueryPerformanceFrequency(&freq)) dm_fail("NVN window: cannot initialize present clock");
    *w = (struct headless_window){ .magic = HEADLESS_WINDOW, .state = *b, .frequency = freq.QuadPart };
    int visible = open_display(w);
    printf("[nvn] initialized %s window (%d textures, no presents)\n", visible ? "Vulkan" : "headless", b->count);
    fflush(stdout);
    return 1;
}
void nvn_window_fini(struct headless_window *w) {
    AcquireSRWLockExclusive(&submit_lock);
    if (!dm_guest_range(w, sizeof(*w), 1) || w->magic != HEADLESS_WINDOW) dm_fail("nvnWindowFinalize: invalid window");
    while (w->queued) wait_for_submit_change(INFINITE);
    close_display(w);
    *w = (struct headless_window){0};
    ReleaseSRWLockExclusive(&submit_lock);
}

static int window_valid(const struct headless_window *w) {
    return dm_guest_range(w, sizeof(*w), 1) && w->magic == HEADLESS_WINDOW && device_valid(w->state.device) &&
           w->state.count >= 1 && w->state.count <= WINDOW_TEXTURES &&
           w->state.active >= 0 && w->state.active <= w->state.count && w->frequency > 0 &&
           w->state.interval >= 0 && w->state.interval <= 5;
}
void nvn_window_set_crop(struct headless_window *w, int x, int y, int width, int height) {
    AcquireSRWLockExclusive(&submit_lock);
    if (!window_valid(w) || x < 0 || y < 0 || width < 0 || height < 0 || (!width != !height))
        dm_fail("nvnWindowSetCrop: invalid crop rectangle");
    w->crop = (NVNrectangle){x, y, width, height};
    ReleaseSRWLockExclusive(&submit_lock);
}
void nvn_window_get_crop(const struct headless_window *w, NVNrectangle *rect) {
    AcquireSRWLockShared(&submit_lock);
    if (!window_valid(w) || !dm_guest_range(rect, sizeof(*rect), 1)) dm_fail("nvnWindowGetCrop: invalid output or window");
    const struct headless_texture *t = w->state.textures[0];
    texture_check(t, 0);
    *rect = w->crop.width ? w->crop : (NVNrectangle){0, 0, (int)t->desc.width, (int)t->desc.height};
    ReleaseSRWLockShared(&submit_lock);
}
void nvn_window_set_interval(struct headless_window *w, int interval) {
    AcquireSRWLockExclusive(&submit_lock);
    if (!window_valid(w) || (unsigned)interval > 5) dm_fail("nvnWindowSetPresentInterval: invalid interval");
    w->state.interval = interval;
    ReleaseSRWLockExclusive(&submit_lock);
}
static LONGLONG present_now(void) {
    LARGE_INTEGER now;
    if (!QueryPerformanceCounter(&now)) dm_fail("NVN window: cannot read present clock");
    return now.QuadPart;
}
static NVNwindowAcquireTextureResult window_acquire(const struct headless_queue *q, struct headless_window *w,
                                                   const struct headless_sync *s, int *index) {
    AcquireSRWLockExclusive(&submit_lock);
    NVNwindowAcquireTextureResult result = NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_NATIVE_ERROR;
    while (window_valid(w) && dm_guest_range(index, sizeof(*index), 1) &&
           (!s || (sync_valid(s) && s->device == w->state.device)) &&
           (!q || (queue_valid(q) && q->device == w->state.device))) {
        unsigned n = (unsigned)(w->state.active ? w->state.active : w->state.count);
        for (unsigned i = 0; i < n; ++i) {
            unsigned slot = (w->next % n + i) % n;
            if (w->acquired & (1u << slot)) continue;
            const struct headless_texture *t = w->state.textures[slot];
            if (!dm_guest_range(t, sizeof(*t), 0) || t->magic != HEADLESS_TEXTURE) goto done;
            LONGLONG available = w->state.interval ? w->available[slot] : 0;
            LONGLONG frequency = w->frequency;
            w->acquired |= 1u << slot;
            w->next = (slot + 1) % n;
            ReleaseSRWLockExclusive(&submit_lock);
            // Pacing must not prevent another queue from publishing a signal.
            LONGLONG left;
            while (available && (left = available - present_now()) > 0) {
                DWORD ms = (DWORD)((left * 1000 + frequency - 1) / frequency);
                Sleep(ms);
            }
            AcquireSRWLockExclusive(&submit_lock);
            if (s) sync_clear(s);
            *index = (int)slot;
            result = NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
            goto done;
        }
        if (!(w->queued & ((1u << n) - 1))) break; // All active slots are still guest-owned.
        wait_for_submit_change(INFINITE);
    }
done:
    ReleaseSRWLockExclusive(&submit_lock);
    return result;
}
NVNwindowAcquireTextureResult nvn_window_acquire(struct headless_window *w, const struct headless_sync *s, int *index) {
    return window_acquire(NULL, w, s, index);
}
NVNqueueAcquireTextureResult nvn_queue_acquire(const struct headless_queue *q, struct headless_window *w, int *index) {
    if (!q) return NVN_QUEUE_ACQUIRE_TEXTURE_RESULT_NATIVE_ERROR;
    return window_acquire(q, w, NULL, index) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS ?
        NVN_QUEUE_ACQUIRE_TEXTURE_RESULT_SUCCESS : NVN_QUEUE_ACQUIRE_TEXTURE_RESULT_NATIVE_ERROR;
}
static int queue_present_later(const struct headless_queue *q, struct headless_window *w,
                               const struct headless_texture *t, int index);
static void replay_present(struct headless_window *w, const struct headless_texture *t,
                           const NVNrectangle *crop, int interval, int index) {
    int displayed = present_display(w, t, crop);
    LONGLONG now = present_now();
    LONGLONG next = w->last_display > now ? w->last_display : now;
    w->last_display = interval ? next + w->frequency * interval / 60 : now;
    w->available[index] = w->last_display;
    w->acquired &= ~(1u << index);
    if (w->queued & (1u << index)) {
        w->queued &= ~(1u << index);
        WakeAllConditionVariable(&submit_changed);
    }
    uint64_t frame = ++w->presents;
    if (frame <= 3 || frame % 60 == 0) {
        printf("[nvn] %s present #%" PRIu64 " (texture %d)\n",
               displayed == 1 ? "Vulkan" : displayed == 2 ? "skipped" : "headless", frame, index);
        fflush(stdout);
    }
}
void nvn_queue_present(const struct headless_queue *q, struct headless_window *w, int index) {
    AcquireSRWLockExclusive(&submit_lock);
    if (!queue_valid(q) || !window_valid(w) || q->device != w->state.device)
        dm_fail("nvnQueuePresentTexture: invalid queue or window");
    if (index < 0 || index >= w->state.count || !((w->acquired & ~w->queued) & (1u << index)))
        dm_fail("nvnQueuePresentTexture: texture %d was not acquired", index);
    const struct headless_texture *t = w->state.textures[index];
    if (!dm_guest_range(t, sizeof(*t), 0) || t->magic != HEADLESS_TEXTURE)
        dm_fail("nvnQueuePresentTexture: texture was finalized");
    if (!queue_present_later(q, w, t, index))
        replay_present(w, t, &w->crop, w->state.interval, index);
    ReleaseSRWLockExclusive(&submit_lock);
}

// A recording keeps the address of the event word, not the event object.
struct event_builder { const struct pool *pool; ptrdiff_t offset; };
struct headless_event { uint32_t magic; const struct nvn_device *device; volatile LONG *word; };
#define HEADLESS_EVENT UINT32_C(0x544e5645)
static_assert(sizeof(struct event_builder) <= sizeof(NVNeventBuilder), "event builder prefix");
static_assert(sizeof(struct headless_event) <= sizeof(NVNevent), "event prefix");
void nvn_event_defaults(struct event_builder *b) {
    if (!dm_guest_range(b, sizeof(*b), 1)) dm_fail("nvnEventBuilderSetDefaults: invalid builder");
    *b = (struct event_builder){0};
}
void nvn_event_storage(struct event_builder *b, const struct pool *p, ptrdiff_t off) {
    if (!dm_guest_range(b, sizeof(*b), 1)) dm_fail("nvnEventBuilderSetStorage: invalid builder");
    *b = (struct event_builder){p, off};
}
NVNboolean nvn_event_init(struct headless_event *e, const struct event_builder *b) {
    if (!dm_guest_range(e, sizeof(*e), 1) || !dm_guest_range(b, sizeof(*b), 0)) return 0;
    volatile LONG *word = pool_storage(b->pool, b->offset, sizeof(LONG), sizeof(LONG), 1);
    if (!word || !device_valid(b->pool->device)) return 0;
    *e = (struct headless_event){ HEADLESS_EVENT, b->pool->device, word };
    return 1;
}
void nvn_event_fini(struct headless_event *e) {
    if (!dm_guest_range(e, sizeof(*e), 1) || e->magic != HEADLESS_EVENT) dm_fail("nvnEventFinalize: invalid event");
    *e = (struct headless_event){0};
}
static volatile LONG *event_storage(volatile LONG *word) {
    if ((uintptr_t)word % sizeof(*word) || !dm_guest_range((const void *)word, sizeof(*word), 1))
        dm_fail("NVN event: invalid backing storage");
    return word;
}
static volatile LONG *event_word(const struct headless_event *e, const struct nvn_device *d) {
    if (!dm_guest_range(e, sizeof(*e), 0) || e->magic != HEADLESS_EVENT ||
        !device_valid(e->device) || (d && e->device != d)) dm_fail("NVN event: invalid event or device");
    return event_storage(e->word);
}
static uint32_t event_load(volatile LONG *word) {
    return (uint32_t)InterlockedCompareExchange(word, 0, 0);
}
static uint32_t event_publish(volatile LONG *word, unsigned mode, uint32_t value) {
    if (mode == NVN_EVENT_SIGNAL_MODE_WRITE) InterlockedExchange(word, (LONG)value);
    else if (mode == NVN_EVENT_SIGNAL_MODE_ADD) value += (uint32_t)InterlockedExchangeAdd(word, (LONG)value);
    else dm_fail("NVN event: invalid signal mode %u", mode);
    return value;
}
static void queue_run_ready(void);
static void queue_note_event_signal(volatile LONG *word, uint32_t value);
uint32_t nvn_event_value(const struct headless_event *e) { return event_load(event_word(e, NULL)); }
void nvn_event_signal(const struct headless_event *e, unsigned mode, uint32_t value) {
    AcquireSRWLockExclusive(&submit_lock);
    volatile LONG *word = event_word(e, NULL);
    queue_note_event_signal(word, event_publish(word, mode, value));
    queue_run_ready();
    ReleaseSRWLockExclusive(&submit_lock);
}

// Recording uses the game's command and control ranges. Beginning a recording
// uses no bytes; command handlers must admit their own records explicitly.
// Within a multi-piece control list, zero size denotes a link to the next grant.
struct command_piece { const void *bytes; size_t size; };
struct command_buffer {
    uint32_t magic;
    NVNboolean callback_enabled, recording;
    const struct nvn_device *device;
    uint64_t guest_flags; // SDK inline writes at +0x10 (see the NVN object reference).
    uint32_t recording_mode;
    PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC callback;
    void *callback_data;
    const struct pool *pool;
    ptrdiff_t offset;
    size_t command_size, control_size;
    void *control;
    size_t command_used, control_used, first_command;
    uint32_t copy_row, copy_image;
    struct command_piece *first_piece, *last_piece;
    size_t pieces;
    struct command_piece closed_piece; // Nonempty only before the new grant emits a command.
};
// Portable records in guest command memory, with opaque handles pointing to
// control records. A list is valid while the guest keeps those bytes alive.
struct command_list {
    uint32_t magic, recording_mode;
    const struct nvn_device *device;
    const void *commands;
    size_t size; // Command bytes for a single piece, descriptor bytes for a multi-piece list.
};
struct command_header { uint32_t op, size; };
enum { COMMAND_DRAW_ELEMENTS = 1, COMMAND_BARRIER, COMMAND_COPY_BUFFER_TO_TEXTURE, COMMAND_DRAW_INSTANCED };
enum { COMMAND_COPY_TEXTURE_TO_TEXTURE = 0x66, COMMAND_DRAW_ARRAYS_INSTANCED };
enum { COMMAND_REPORT_COUNTER = 0x70 };  // Counter type in the high 24 opcode bits.
struct command_report_counter { struct command_header header; NVNbufferAddress destination; };
static_assert(sizeof(struct command_report_counter) == 16 && sizeof(NVNcounterData) == 16 &&
              offsetof(NVNcounterData, timestamp) == 8, "counter command and output layouts");
enum { COMMAND_WAIT_EVENT = 0x71, COMMAND_SIGNAL_EVENT, COMMAND_WAIT_SYNC, COMMAND_FENCE_SYNC };
enum { COMMAND_RESET_SAMPLES = 0x75 };
struct command_event {
    struct command_header header;
    volatile LONG *word;
    uint32_t mode, value;
};
struct command_sync {
    struct command_header header;
    union { const struct headless_sync *sync; struct queue_receipt *receipt; } target;
};
static_assert(sizeof(struct command_event) == 24 && sizeof(struct command_sync) == 16, "queue dependency records");
struct draw_elements {
    struct command_header header;
    uint32_t count, params;
    NVNbufferAddress index_buffer;
};
struct draw_instanced { struct draw_elements indexed; int32_t base_vertex; uint32_t first_instance, instances, pad; };
struct command_barrier { struct command_header header; uint32_t bits, pad; };
#include "m4_guest_resources.h"
enum { COMMAND_CLEAR_ATTACHMENT = 0x60 };
enum { COMMAND_UPDATE_UNIFORM = 0x61 };
enum { COMMAND_DISPATCH_COMPUTE = 0x62 };
enum { COMMAND_DISPATCH_INDIRECT = COMMAND_DISPATCH_COMPUTE | (1 << 8) };
enum { COMMAND_CLEAR_TEXTURE = 0x63 };
enum { COMMAND_DRAW_MESH_TASKS = 0x64 };
enum { COMMAND_DRAW_MESH_INDIRECT = COMMAND_DRAW_MESH_TASKS | (1 << 8) };
enum { COMMAND_RENDER_ENABLE = 0x65 };
enum { COMMAND_CLEAR_BUFFER = 0x68 };
struct command_clear_buffer { struct command_header h; NVNbufferAddress dst; size_t bytes; uint32_t value, pad; };
enum { RENDER_IF_EQUAL, RENDER_IF_NOT_EQUAL, RENDER_ALWAYS, RENDER_NEVER };
struct command_render_enable { struct command_header h; NVNbufferAddress address; };
struct draw_mesh_tasks { struct command_header header; uint32_t first, count, modes, pad; };
static_assert(sizeof(struct draw_mesh_tasks) == 24, "mesh draw record layout");
struct draw_indirect_mesh { struct command_header header; NVNbufferAddress arguments; uint32_t modes, pad; };
struct command_clear_texture {
    struct command_header h;
    struct dm_m4_draw_texture texture;
    struct dm_m4_draw_scissor rect;
    struct dm_m4_clear clear;
};
static_assert(sizeof(struct command_clear_texture) % 8 == 0, "texture clear record alignment");
struct command_uniform { struct command_header h; NVNbufferAddress dst; uint64_t bytes; };
struct command_clear { struct command_header h; struct dm_m4_clear clear; uint32_t pad; };
struct command_copy_buffer_to_texture {
    struct command_header header;
    NVNbufferAddress source;
    uint32_t flags, row_stride, image_stride;
    NVNcopyRegion region;
    struct texture_binding destination;
};
struct command_copy_texture_to_texture {
    struct command_header header;
    uint32_t flags;
    NVNcopyRegion regions[2];
    struct texture_binding source, destination;
};
static_assert(sizeof(struct command_copy_texture_to_texture) % 8 == 0, "texture copy record alignment");
static_assert(sizeof(struct command_list) % 8 == 0 && sizeof(struct draw_elements) % 8 == 0 &&
              sizeof(struct command_barrier) % 8 == 0 && sizeof(struct command_copy_buffer_to_texture) % 8 == 0,
              "command and control record alignment");
#define HEADLESS_COMMAND_LIST UINT32_C(0x54534c43)
#define MULTI_COMMAND_LIST UINT32_C(0x4d534c43)
#define HEADLESS_COMMAND UINT32_C(0x42444d43)
static_assert(offsetof(struct command_buffer, guest_flags) == 0x10, "guest command flag offset");
static_assert(offsetof(struct command_buffer, recording_mode) == 0x18, "NVN2 recording mode offset");
static_assert(sizeof(struct command_buffer) <= sizeof(NVNcommandBuffer), "command buffer prefix");
static void command_check(const struct command_buffer *c, int write) {
    if (!dm_guest_range(c, sizeof(*c), write) || c->magic != HEADLESS_COMMAND) dm_fail("NVN command buffer: invalid object");
}
NVNboolean nvn_command_init(struct command_buffer *c, const struct nvn_device *d) {
    if (!dm_guest_range(c, sizeof(*c), 1) || !dm_guest_range(d, sizeof(*d), 0) || d->magic != HEADLESS_NVN_DEVICE) return 0;
    *c = (struct command_buffer){ .magic = HEADLESS_COMMAND, .callback_enabled = 1, .device = d };
    return 1;
}
void nvn_command_fini(struct command_buffer *c) {
    command_check(c, 1);
    if (c->recording) dm_fail("nvnCommandBufferFinalize: recording is active");
    *c = (struct command_buffer){0};
}
void nvn_command_set_recording_mode(struct command_buffer *c, uint32_t mode) {
    command_check(c, 1);
    // NVN2 uses modes 0–2 to select native command encodings. Portable
    // records do not emit those words. Modes 3–4 can also change the handle
    // format at EndRecording, which this recorder does not implement.
    if (mode > 2) dm_fail("nvnCommandBufferSetRecordingMode: unsupported mode %u", mode);
    c->recording_mode = mode;
}
// SDK 22.2.8 stores w1 and reads it back through w0. These are buffer
// state, not commands: a buffer/texture copy must snapshot them when recorded.
void nvn_command_set_copy_row(struct command_buffer *c, ptrdiff_t stride) {
    command_check(c, 1);
    c->copy_row = (uint32_t)stride;
}
void nvn_command_set_copy_image(struct command_buffer *c, ptrdiff_t stride) {
    command_check(c, 1);
    c->copy_image = (uint32_t)stride;
}
ptrdiff_t nvn_command_get_copy_row(const struct command_buffer *c) {
    command_check(c, 0);
    return c->copy_row;
}
ptrdiff_t nvn_command_get_copy_image(const struct command_buffer *c) {
    command_check(c, 0);
    return c->copy_image;
}
void nvn_command_begin(struct command_buffer *c) {
    command_check(c, 1);
    if (c->recording) dm_fail("nvnCommandBufferBeginRecording: already recording");
    c->first_command = c->command_used;
    c->first_piece = c->last_piece = NULL;
    c->pieces = 0;
    c->recording = 1;
}
NVNboolean nvn_command_recording(const struct command_buffer *c) {
    command_check(c, 0);
    return c->recording;
}
static void request_memory(struct command_buffer *c, NVNcommandBufferMemoryEvent event, size_t size) {
    if (!c->callback || (event == NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY && !c->callback_enabled))
        return;
    const uint64_t args[8] = {(uintptr_t)c, (uint64_t)event, size, (uintptr_t)c->callback_data};
    if (!dm_guest_call((uintptr_t)c->callback, args, NULL))
        dm_fail("NVN command buffer: cannot enter guest memory callback");
}
static void flush_closed_piece(struct command_buffer *c);
// The caller has checked that the command buffer is recording.
static void *command_reserve(struct command_buffer *c, size_t size) {
    if (c->closed_piece.size) flush_closed_piece(c);
    if (size > c->command_size - c->command_used) {
        request_memory(c, NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY, size);
        if (c->closed_piece.size) flush_closed_piece(c);
        if (size > c->command_size - c->command_used)
            dm_fail("NVN command buffer: command memory exhausted (need %zu bytes)", size);
    }
    if (c->offset < 0 || c->command_used > (size_t)(PTRDIFF_MAX - c->offset))
        dm_fail("NVN command buffer: invalid command memory offset");
    void *dst = pool_storage(c->pool, c->offset + (ptrdiff_t)c->command_used, size, 4, 1);
    if (!dst) dm_fail("NVN command buffer: invalid command memory backing");
    c->command_used += size;
    return dst;
}
#include "m4_draw_state.inc"

static void command_append(struct command_buffer *c, const void *record, size_t size) {
    draw_require_recording(c);
    memcpy(command_reserve(c, size), record, size);
}
void nvn_command_update_uniform(struct command_buffer *c, NVNbufferAddress base, size_t capacity,
                                ptrdiff_t offset, size_t bytes, const void *source) {
    draw_require_recording(c);
    if (offset < 0 || (size_t)offset > capacity || bytes > capacity - (size_t)offset ||
        base > UINT64_MAX - (size_t)offset || bytes > UINT32_MAX - sizeof(struct command_uniform) - 7 ||
        (bytes && !dm_guest_range(source, bytes, 0))) dm_fail("nvnCommandBufferUpdateUniformBuffer: invalid range");
    if (!bytes) return;
    size_t size = (sizeof(struct command_uniform) + bytes + 7) & ~(size_t)7;
    const struct command_uniform r = {{COMMAND_UPDATE_UNIFORM, (uint32_t)size}, base + (size_t)offset, bytes};
    uint8_t *record = command_reserve(c, size);
    memcpy(record, &r, sizeof(r));
    memcpy(record + sizeof(r), source, bytes);
    memset(record + sizeof(r) + bytes, 0, size - sizeof(r) - bytes);
}
void nvn_command_clear_buffer(struct command_buffer *c, NVNbufferAddress dst, size_t bytes, uint32_t value) {
    draw_require_recording(c);
    if ((dst | bytes) & 3) dm_fail("nvnCommandBufferClearBuffer: expected a word-aligned range");
    if (!bytes) return;
    const struct command_clear_buffer r = {{COMMAND_CLEAR_BUFFER, sizeof(r)}, dst, bytes, value, 0};
    command_append(c, &r, sizeof(r));
}
void nvn_command_wait_event(struct command_buffer *c, const struct headless_event *e, NVNeventWaitMode mode, uint32_t value) {
    command_check(c, 0);
    if ((unsigned)mode > NVN_EVENT_WAIT_MODE_GEQUAL_WRAP) dm_fail("nvnCommandBufferWaitEvent: invalid mode");
    const struct command_event r = {{COMMAND_WAIT_EVENT, sizeof(r)}, event_word(e, c->device), (uint32_t)mode, value};
    command_append(c, &r, sizeof(r));
}
void nvn_command_signal_event(struct command_buffer *c, const struct headless_event *e, NVNeventSignalMode mode,
                              NVNeventSignalLocation location, int flags, uint32_t value) {
    command_check(c, 0);
    if ((unsigned)mode > NVN_EVENT_SIGNAL_MODE_ADD || (unsigned)location > NVN_EVENT_SIGNAL_LOCATION_BOTTOM || (flags & ~1))
        dm_fail("nvnCommandBufferSignalEvent: invalid mode, signal location or flags");
    // Replay completes each record (including GPU writeback) before the next.
    // All pipeline locations and the no-wait-memory flag converge here.
    const struct command_event r = {{COMMAND_SIGNAL_EVENT, sizeof(r)}, event_word(e, c->device), (uint32_t)mode, value};
    command_append(c, &r, sizeof(r));
}
static void check_fence_options(NVNsyncCondition condition, int flags) {
    if ((unsigned)condition > NVN_SYNC_CONDITION_GRAPHICS_WORLD_SPACE_COMPLETE || (flags & ~1))
        dm_fail("NVN fence sync: invalid condition or flags");
}
static void record_sync(struct command_buffer *c, const struct headless_sync *s, unsigned op) {
    draw_require_recording(c);
    AcquireSRWLockShared(&submit_lock);
    if (!sync_valid(s) || s->device != c->device) dm_fail("NVN command buffer: invalid sync or device");
    ReleaseSRWLockShared(&submit_lock);
    const struct command_sync r = {{op, sizeof(r)}, {.sync = s}};
    command_append(c, &r, sizeof(r));
}
void nvn_command_wait_sync(struct command_buffer *c, const struct headless_sync *s) {
    record_sync(c, s, COMMAND_WAIT_SYNC);
}
void nvn_command_fence_sync(struct command_buffer *c, struct headless_sync *s, NVNsyncCondition condition, int flags) {
    check_fence_options(condition, flags);
    record_sync(c, s, COMMAND_FENCE_SYNC);
}
static void record_clear(struct command_buffer *c, struct dm_m4_clear clear) {
    const struct command_clear r = {{COMMAND_CLEAR_ATTACHMENT, sizeof(r)}, clear, 0};
    command_append(c, &r, sizeof(r));
}
static void color_clear(struct command_buffer *c, int index, const void *value, int mask, uint8_t type) {
    if ((unsigned)index >= DM_M4_DRAW_TARGETS || (mask & ~15) || !dm_guest_range(value, 16, 0))
        dm_fail("nvnCommandBufferClearColor: invalid color or channel mask");
    struct dm_m4_clear r = {.type = type, .index = (uint8_t)index, .mask = (uint8_t)mask};
    memcpy(r.value, value, sizeof(r.value));
    record_clear(c, r);
}
void nvn_clear_f(struct command_buffer *c, int index, const void *v, int m) { color_clear(c, index, v, m, DM_M4_CLEAR_FLOAT); }
void nvn_clear_i(struct command_buffer *c, int index, const void *v, int m) { color_clear(c, index, v, m, DM_M4_CLEAR_INT); }
void nvn_clear_u(struct command_buffer *c, int index, const void *v, int m) { color_clear(c, index, v, m, DM_M4_CLEAR_UINT); }
static void record_texture_clear(struct command_buffer *c, const struct headless_texture *t, const struct texture_view *v,
                                 const NVNcopyRegion *region, const void *value, int mask, uint8_t type) {
    struct texture_region area = texture_region_check(t, v, region, "nvnCommandBufferClearTexture");
    if (area.layer + area.z > UINT16_MAX || area.images > UINT16_MAX || (mask & ~15) || !dm_guest_range(value, 16, 0))
        dm_fail("nvnCommandBufferClearTexture: invalid image range, color or channel mask");
    struct command_clear_texture r = {.h = {COMMAND_CLEAR_TEXTURE, sizeof(r)}, .texture = draw_texture_snapshot(t, v),
        .rect = {(int)area.x, (int)area.y, (int)area.w, (int)area.h}, .clear = {.type = type, .mask = (uint8_t)mask}};
    // Record the absolute array layers or, for a volume, the region's mip slices.
    r.texture.view.set |= VIEW_LAYERS;
    r.texture.view.base_layer = (uint16_t)(area.layer + area.z);
    r.texture.view.layers = (uint16_t)area.images;
    memcpy(r.clear.value, value, sizeof(r.clear.value));
    command_append(c, &r, sizeof(r));
}
#define TEXTURE_CLEAR(name, type) \
    void nvn_clear_texture_##name(struct command_buffer *c, const struct headless_texture *t, const struct texture_view *v, \
                                  const NVNcopyRegion *region, const void *value, int mask) { \
        record_texture_clear(c, t, v, region, value, mask, type); \
    }
TEXTURE_CLEAR(f, DM_M4_CLEAR_FLOAT)
TEXTURE_CLEAR(i, DM_M4_CLEAR_INT)
TEXTURE_CLEAR(u, DM_M4_CLEAR_UINT)
#undef TEXTURE_CLEAR
void nvn_clear_ds(struct command_buffer *c, float depth, unsigned write, int stencil, int smask) {
    struct dm_m4_clear r = {.type = DM_M4_CLEAR_DEPTH_STENCIL, .mask = !!write, .stencil_mask = (uint8_t)smask};
    memcpy(&r.value[0], &depth, sizeof(depth));
    r.value[1] = (uint32_t)stencil;
    record_clear(c, r);
}
static struct draw_elements draw_description(struct command_buffer *c, NVNdrawPrimitive mode, NVNindexType type,
                                             int count, NVNbufferAddress indices) {
    if ((unsigned)mode > NVN_DRAW_PRIMITIVE_PATCHES || (unsigned)type > NVN_INDEX_TYPE_UNSIGNED_INT || count < 0)
        dm_fail("nvnCommandBufferDrawElements: invalid draw description");
    draw_require_recording(c);
    const struct nvn_device *d = c->device;
    if (!dm_guest_range(d, sizeof(*d), 0) || d->magic != HEADLESS_NVN_DEVICE || d->origin > 1 || d->depth_mode > 1)
        dm_fail("nvnCommandBufferDrawElements: invalid device raster modes");
    struct draw_elements record = {.header = {COMMAND_DRAW_ELEMENTS, sizeof(record)},
        .count = (unsigned)count, .index_buffer = indices,
        .params = (unsigned)mode | ((unsigned)type << 8) | (d->origin << 10) | (d->depth_mode << 11)};
    return record;
}
void nvn_command_draw_elements(struct command_buffer *c, NVNdrawPrimitive mode, NVNindexType type,
                               int count, NVNbufferAddress indices) {
    struct draw_elements record = draw_description(c, mode, type, count, indices);
    memcpy(command_reserve(c, sizeof(record)), &record, sizeof(record));
}
void nvn_command_draw_instanced(struct command_buffer *c, NVNdrawPrimitive mode, NVNindexType type,
                                int count, NVNbufferAddress indices, int base, int first, int instances) {
    if (first < 0 || instances < 0) dm_fail("nvnCommandBufferDrawElementsInstanced: invalid instance range");
    struct draw_instanced r = {.indexed = draw_description(c, mode, type, count, indices), .base_vertex = base,
                               .first_instance = (unsigned)first, .instances = (unsigned)instances};
    r.indexed.header = (struct command_header){COMMAND_DRAW_INSTANCED, sizeof(r)};
    command_append(c, &r, sizeof(r));
}
void nvn_command_draw_arrays_instanced(struct command_buffer *c, NVNdrawPrimitive mode, int start,
                                      int count, int first, int instances) {
    if (start < 0 || first < 0 || instances < 0) dm_fail("nvnCommandBufferDrawArraysInstanced: invalid range");
    struct draw_instanced r = {.indexed = draw_description(c, mode, NVN_INDEX_TYPE_UNSIGNED_BYTE, count, 0),
                               .base_vertex = start, .first_instance = (unsigned)first, .instances = (unsigned)instances};
    r.indexed.header = (struct command_header){COMMAND_DRAW_ARRAYS_INSTANCED, sizeof(r)};
    command_append(c, &r, sizeof(r));
}
void nvn_command_draw_arrays(struct command_buffer *c, NVNdrawPrimitive mode, int start, int count) {
    nvn_command_draw_arrays_instanced(c, mode, start, count, 0, 1);
}
void nvn_command_draw_base_vertex(struct command_buffer *c, NVNdrawPrimitive mode, NVNindexType type,
                                  int count, NVNbufferAddress indices, int base) {
    nvn_command_draw_instanced(c, mode, type, count, indices, base, 0, 1);
}
static uint32_t mesh_raster_modes(struct command_buffer *c) {
    draw_require_recording(c);
    const struct nvn_device *d = c->device;
    if (!dm_guest_range(d, sizeof(*d), 0) || d->magic != HEADLESS_NVN_DEVICE || d->origin > 1 || d->depth_mode > 1)
        dm_fail("nvnCommandBufferDrawMeshTasks: invalid device raster modes");
    return d->origin | (d->depth_mode << 1);
}
void nvn_command_draw_mesh_tasks(struct command_buffer *c, int first, int count) {
    if (first < 0 || count < 0) dm_fail("nvnCommandBufferDrawMeshTasks: invalid task range");
    const struct draw_mesh_tasks record = {{COMMAND_DRAW_MESH_TASKS, sizeof(record)}, (uint32_t)first,
        (uint32_t)count, mesh_raster_modes(c), 0};
    command_append(c, &record, sizeof(record));
}
void nvn_command_draw_mesh_indirect(struct command_buffer *c, NVNbufferAddress address) {
    const struct draw_indirect_mesh r = {{COMMAND_DRAW_MESH_INDIRECT, sizeof(r)}, address, mesh_raster_modes(c), 0};
    command_append(c, &r, sizeof(r));
}
void nvn_command_barrier(struct command_buffer *c, int bits) {
    // Keep NVN's complete mask for replay. No ordering or invalidation is
    // performed while recording the command.
    struct command_barrier record = {.header = {COMMAND_BARRIER, sizeof(record)}, .bits = (uint32_t)bits};
    command_append(c, &record, sizeof(record));
}
void nvn_command_shader_scratch(struct command_buffer *c, const struct pool *p, ptrdiff_t offset, size_t size) {
    command_check(c, 0);
    if (!c->recording) dm_fail("nvnCommandBufferSetShaderScratchMemory: not recording");
    // The NVN region is driver-private spill storage, not a shader-visible
    // descriptor. Vulkan allocates spill space for the compiled SPIR-V itself.
    if (p) {
        pool_check(p, 0);
        if (p->device != c->device || offset < 0 || (size_t)offset > p->size || size > p->size - (size_t)offset)
            dm_fail("nvnCommandBufferSetShaderScratchMemory: invalid pool range");
    } else if (size) dm_fail("nvnCommandBufferSetShaderScratchMemory: missing pool");
}
void nvn_command_tiled_cache(struct command_buffer *c, NVNtiledCacheAction action) {
    draw_require_recording(c);
    switch (action) {
    case NVN_TILED_CACHE_ACTION_FLUSH:
    case NVN_TILED_CACHE_ACTION_FLUSH_NO_TILING:
        // Record the ordering point. Each previous Vulkan draw stores its attachments and fences.
        nvn_command_barrier(c, NVN_BARRIER_ORDER_FRAGMENTS_BIT);
        break;
    case NVN_TILED_CACHE_ACTION_ENABLE: case NVN_TILED_CACHE_ACTION_DISABLE:
    case NVN_TILED_CACHE_ACTION_ENABLE_RENDER_TARGET_BINNING: case NVN_TILED_CACHE_ACTION_DISABLE_RENDER_TARGET_BINNING:
        break; // Binning is a driver scheduling choice; it does not change attachment contents.
    default: dm_fail("nvnCommandBufferSetTiledCacheAction: unknown action %d", (int)action);
    }
}
void nvn_command_tile_size(struct command_buffer *c, int width, int height) {
    draw_require_recording(c);
    // Tile-cache geometry is a binning hint, not a viewport or framebuffer extent.
    // Vulkan selects its own granularity for dynamic rendering.
    (void)width; (void)height;
}
void nvn_command_compute_policy(struct command_buffer *c, int policy) {
    command_check(c, 0);
    if (!c->recording) dm_fail("NVN compute scheduling policy: not recording");
    // Neither overlap nor workload priority changes dependencies. The single
    // synchronous replay queue has no concurrent work to schedule.
    (void)policy;
}
void nvn_command_render_enable(struct command_buffer *c, NVNboolean enable) {
    struct command_render_enable r = {{COMMAND_RENDER_ENABLE | ((enable ? RENDER_ALWAYS : RENDER_NEVER) << 8), sizeof(r)}, 0};
    command_append(c, &r, sizeof(r));
}
void nvn_command_render_conditional(struct command_buffer *c, uint32_t mode, NVNbufferAddress address) {
    if (mode > RENDER_IF_NOT_EQUAL) dm_fail("nvnCommandBufferSetRenderEnableConditional: invalid mode %u", mode);
    struct command_render_enable r = {{COMMAND_RENDER_ENABLE | (mode << 8), sizeof(r)}, address};
    command_append(c, &r, sizeof(r));
}
void nvn_command_reset_counter(struct command_buffer *c, NVNcounterType type) {
    if (type != NVN_COUNTER_TYPE_SAMPLES_PASSED) dm_fail("nvnCommandBufferResetCounter: unsupported counter %#x", (unsigned)type);
    const struct command_header h = {COMMAND_RESET_SAMPLES, sizeof(h)};
    command_append(c, &h, sizeof(h));
}
void nvn_command_report_counter(struct command_buffer *c, NVNcounterType type, NVNbufferAddress destination) {
    if (type != NVN_COUNTER_TYPE_TIMESTAMP && type != NVN_COUNTER_TYPE_TIMESTAMP_TOP && type != NVN_COUNTER_TYPE_SAMPLES_PASSED)
        dm_fail("nvnCommandBufferReportCounter: unsupported counter type %#x", (unsigned)type);
    const struct command_report_counter record = {
        .header = {COMMAND_REPORT_COUNTER | ((uint32_t)type << 8), sizeof(record)},
        .destination = destination,
    };
    command_append(c, &record, sizeof(record));
}
void nvn_command_copy_buffer_to_texture(struct command_buffer *c, NVNbufferAddress source,
                                        const struct headless_texture *texture, const struct texture_view *view,
                                        const NVNcopyRegion *region, int flags) {
    command_check(c, 0);
    (void)texture_region_check(texture, view, region, "nvnCommandBufferCopyBufferToTexture");
    // The CPU-side descriptions may be reused after recording. The source
    // address and destination pool remain replay-time dependencies.
    struct command_copy_buffer_to_texture record = {
        .header = {COMMAND_COPY_BUFFER_TO_TEXTURE, sizeof(record)},
        .source = source, .flags = (uint32_t)flags, .row_stride = c->copy_row, .image_stride = c->copy_image,
        .region = *region, .destination.texture = *texture,
    };
    if (view) record.destination.view = *view;
    command_append(c, &record, sizeof(record));
}
void nvn_command_copy_texture_to_texture(struct command_buffer *c,
        const struct headless_texture *source, const struct texture_view *source_view, const NVNcopyRegion *source_region,
        const struct headless_texture *destination, const struct texture_view *destination_view,
        const NVNcopyRegion *destination_region, int flags) {
    const char *operation = "nvnCommandBufferCopyTextureToTexture";
    command_check(c, 0);
    (void)texture_region_check(source, source_view, source_region, operation);
    (void)texture_region_check(destination, destination_view, destination_region, operation);
    struct command_copy_texture_to_texture record = {
        .header = {COMMAND_COPY_TEXTURE_TO_TEXTURE, sizeof(record)},
        .flags = (uint32_t)flags, .regions = {*source_region, *destination_region},
        .source.texture = *source, .destination.texture = *destination,
    };
    if (source_view) record.source.view = *source_view;
    if (destination_view) record.destination.view = *destination_view;
    command_append(c, &record, sizeof(record));
}
static struct command_piece command_tail(const struct command_buffer *c) {
    size_t bytes = c->command_used - c->first_command;
    void *commands = NULL;
    if (bytes) {
        if (c->offset < 0 || c->first_command > (size_t)(PTRDIFF_MAX - c->offset))
            dm_fail("nvnCommandBufferEndRecording: invalid command offset");
        commands = pool_storage(c->pool, c->offset + (ptrdiff_t)c->first_command, bytes, 4, 0);
        if (!commands) dm_fail("nvnCommandBufferEndRecording: invalid command backing");
    }
    return (struct command_piece){commands, bytes};
}
static void *control_reserve(struct command_buffer *c, size_t size, size_t spare) {
    size_t need = size + spare;
    if (need > c->control_size - c->control_used) {
        request_memory(c, NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_CONTROL_MEMORY, need);
        if (need > c->control_size - c->control_used)
            dm_fail("NVN command buffer: control memory exhausted (need %zu bytes)", need);
    }
    void *entry = (uint8_t *)c->control + c->control_used;
    if (!dm_guest_range(entry, need, 1)) dm_fail("NVN command buffer: invalid control backing");
    c->control_used += size;
    return entry;
}
static void add_piece(struct command_buffer *c, struct command_piece p) {
    if (!p.size) return;
    // Leave room for a link in case the next piece lives in another grant.
    struct command_piece *entry = control_reserve(c, sizeof(p), sizeof(p));
    if (!c->first_piece) c->first_piece = entry;
    else if (entry != c->last_piece + 1) {
        struct command_piece *link = c->last_piece + 1;
        if (!dm_guest_range(link, sizeof(*link), 1)) dm_fail("NVN command buffer: invalid control link backing");
        *link = (struct command_piece){entry, 0};
    }
    *entry = p;
    c->last_piece = entry;
    ++c->pieces;
}
static void flush_closed_piece(struct command_buffer *c) {
    struct command_piece p = c->closed_piece;
    c->closed_piece.size = 0;
    add_piece(c, p);
}
static void close_piece(struct command_buffer *c) {
    flush_closed_piece(c);
    struct command_piece p = command_tail(c);
    c->first_command = c->command_used;
    add_piece(c, p);
}
static struct command_list read_list(const struct nvn_device *device, NVNcommandHandle handle) {
    struct command_list l;
    const void *addr = (const void *)(uintptr_t)handle;
    if (handle % 8 || !dm_guest_range(addr, sizeof(l), 0)) dm_fail("NVN command list: invalid handle");
    memcpy(&l, addr, sizeof(l));
    if (l.recording_mode > 2 || l.device != device ||
        (l.magic == HEADLESS_COMMAND_LIST ? l.size % 8 != 0 :
         l.magic != MULTI_COMMAND_LIST || !l.size || l.size % sizeof(struct command_piece) != 0))
        dm_fail("NVN command list: invalid list");
    return l;
}
static struct command_piece read_control_piece(const struct command_piece *entry) {
    if ((uintptr_t)entry % 8 || !dm_guest_range(entry, sizeof(*entry), 0))
        dm_fail("NVN command list: invalid control entry");
    return *entry;
}
static int next_piece(struct command_list *l, struct command_piece *p) {
    if (!l->size) return 0;
    if (l->magic == HEADLESS_COMMAND_LIST) {
        *p = (struct command_piece){l->commands, l->size};
        l->size = 0;
        return 1;
    }
    const struct command_piece *entry = l->commands;
    *p = read_control_piece(entry);
    if (!p->size) {
        entry = p->bytes;
        *p = read_control_piece(entry);
    }
    // Every link leads directly to a piece, never another link.
    if (!p->size || p->size % 8) dm_fail("NVN command list: invalid command piece size");
    l->commands = entry + 1;
    l->size -= sizeof(*p);
    return 1;
}
static void check_recording_handles(struct command_buffer *c, int n, const NVNcommandHandle *handles) {
    draw_require_recording(c);
    if (n < 0 || (n && !dm_guest_range(handles, (size_t)n * sizeof(*handles), 0)))
        dm_fail("NVN command buffer: invalid handle array");
}
// Calls snapshot the control entries, not the command bytes. The caller may
// immediately recycle the called control storage.
void nvn_command_call(struct command_buffer *c, int n, const NVNcommandHandle *handles) {
    check_recording_handles(c, n, handles);
    for (int i = 0; i < n; ++i) {
        struct command_list list = read_list(c->device, handles[i]);
        if (!list.size) continue;
        close_piece(c);
        struct command_piece p;
        while (next_piece(&list, &p)) add_piece(c, p);
    }
}
// Copy the records, not the addresses of their backing ranges. Reserving
// one whole record at a time allows copies to span command memory grants.
void nvn_command_copy(struct command_buffer *c, int n, const NVNcommandHandle *handles) {
    check_recording_handles(c, n, handles);
    for (int i = 0; i < n; ++i) {
        struct command_list list = read_list(c->device, handles[i]);
        struct command_piece p;
        while (next_piece(&list, &p)) {
            if (!dm_guest_range(p.bytes, p.size, 0)) dm_fail("nvnCommandBufferCopyCommands: invalid command backing");
            const uint8_t *bytes = p.bytes;
            for (size_t at = 0; at < p.size;) {
                struct command_header h;
                memcpy(&h, bytes + at, sizeof(h));
                if (h.size < sizeof(h) || h.size % 8 || h.size > p.size - at)
                    dm_fail("nvnCommandBufferCopyCommands: invalid record size");
                memmove(command_reserve(c, h.size), bytes + at, h.size);
                at += h.size;
            }
        }
    }
}
NVNcommandHandle nvn_command_end(struct command_buffer *c) {
    command_check(c, 1);
    if (!c->recording) dm_fail("nvnCommandBufferEndRecording: not recording");
    flush_closed_piece(c);
    struct command_piece p = command_tail(c);
    c->first_command = c->command_used;
    struct command_list list = {.magic = HEADLESS_COMMAND_LIST, .recording_mode = c->recording_mode,
        .device = c->device, .commands = p.bytes, .size = p.size};
    if (c->pieces) {
        add_piece(c, p);
        list.magic = MULTI_COMMAND_LIST;
        list.commands = c->first_piece;
        list.size = c->pieces * sizeof(p);
    }
    void *entry = control_reserve(c, sizeof(list), 0);
    memcpy(entry, &list, sizeof(list));
    c->recording = 0;
    return (uintptr_t)entry;
}
static void submit_buffer_to_texture(const struct headless_queue *q, struct command_copy_buffer_to_texture *copy) {
    const char *operation = "nvnQueueSubmitCommands: buffer-to-texture copy";
    if (copy->flags & ~(uint32_t)NVN_COPY_FLAGS_ENGINE_2D_BIT)
        dm_fail("%s: unsupported flags %#x", operation, copy->flags);
    struct headless_texture *t = &copy->destination.texture;
    if (t->magic != HEADLESS_TEXTURE || !texture_layout(&t->desc, &t->layout))
        dm_fail("%s: invalid texture description", operation);
    pool_check(t->pool, 0);
    if (t->pool->device != q->device) dm_fail("%s: texture belongs to another device", operation);
    const struct texture_view *view = &copy->destination.view;
    struct texture_region r = texture_region_geometry(t, view, copy->region, operation);
    texture_write_region(t, view, r, (const void *)(uintptr_t)copy->source, copy->row_stride,
                         copy->image_stride, operation);
}
static struct texture_transfer submit_texture_operand(const struct headless_queue *q, struct texture_binding *binding,
        NVNcopyRegion region, int write, const char *operation) {
    struct headless_texture *t = &binding->texture;
    if (t->magic != HEADLESS_TEXTURE || !texture_layout(&t->desc, &t->layout))
        dm_fail("%s: invalid texture description", operation);
    pool_check(t->pool, 0);
    if (t->pool->device != q->device) dm_fail("%s: texture belongs to another device", operation);
    const struct texture_region r = texture_region_geometry(t, &binding->view, region, operation);
    return texture_transfer_open(t, &binding->view, r, write, operation);
}
static const char texture_copy_operation[] = "nvnQueueSubmitCommands: texture-to-texture copy";
static void stage_texture_copy(const void *context, int write, void *bytes) {
    const struct texture_transfer *t = context;
    texture_transfer_linear(t, bytes, t->row, t->image_bytes, write, texture_copy_operation);
}
static void submit_texture_to_texture(const struct headless_queue *q, struct command_copy_texture_to_texture *copy) {
    const char *operation = texture_copy_operation;
    const uint32_t flags = copy->flags;
    const uint32_t known = NVN_COPY_FLAGS_LINEAR_FILTER_BIT | NVN_COPY_FLAGS_ENGINE_2D_BIT |
        NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT | NVN_COPY_FLAGS_MIRROR_Z_BIT;
    if (flags & ~known) dm_fail("%s: unsupported flags %#x", operation, flags);
    const struct texture_transfer s = submit_texture_operand(q, &copy->source, copy->regions[0], 0, operation);
    const struct texture_transfer d = submit_texture_operand(q, &copy->destination, copy->regions[1], 1, operation);
    if (s.images != d.images && !(s.volume && d.volume))
        dm_fail("%s: array layer counts differ", operation);
    const int xy_scale = copy->regions[0].width != copy->regions[1].width ||
                         copy->regions[0].height != copy->regions[1].height;
    if (s.format == d.format && (!(flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) || (!xy_scale && s.images == d.images))) {
        if ((s.block_width != 1 || s.block_height != 1) &&
            (xy_scale || s.images != d.images || (flags & (NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT))))
            dm_fail("%s: cannot scale or mirror compressed blocks", operation);
        texture_copy_raw(s, &d, flags, operation);
        return;
    }
    const struct dm_m4_blit_plane planes[2] = {
        {s.format, (uint32_t)copy->regions[0].width, (uint32_t)copy->regions[0].height, s.images,
            s.image_bytes * s.images, &s, stage_texture_copy},
        {d.format, (uint32_t)copy->regions[1].width, (uint32_t)copy->regions[1].height, d.images,
            d.image_bytes * d.images, &d, stage_texture_copy},
    };
    // Use the display's serialized transfer queue without loading any shaders.
    struct dm_m4_replay_session session;
    const char *error = dm_m4_replay_begin(q->device, &(const struct dm_m4_replay_keys){0}, &session);
    if (error) dm_fail("%s: %s", operation, error);
    const char *step;
    const VkResult result = dm_m4_texture_blit(session.draw, planes, s.volume && d.volume, flags, &step);
    dm_m4_replay_end();
    if (result != VK_SUCCESS) dm_fail("%s: %s (VkResult %d)", operation, step, (int)result);
}
struct queue_work {
    struct queue_work *next;
    size_t at, size;
    int present;
};
static uint8_t *queue_work_bytes(struct queue_work *w) { return (uint8_t *)(w + 1); }
struct queue_present_work {
    struct headless_window *window;
    struct headless_texture texture;
    NVNrectangle crop;
    int interval, index;
};
struct pending_queue {
    struct pending_queue *next;
    const struct headless_queue *queue;
    const struct nvn_device *device;
    struct queue_work *first, **tail;
    struct command_event wait_event; // Non-null only for a parked event wait.
    int event_ready;
};
static struct pending_queue *pending_queues;

static int event_satisfies(unsigned mode, uint32_t current, uint32_t target) {
    if (mode == NVN_EVENT_WAIT_MODE_EQUAL) return current == target;
    if (mode == NVN_EVENT_WAIT_MODE_GEQUAL_WRAP) return current - target < UINT32_C(0x80000000);
    dm_fail("nvnQueueSubmitCommands: invalid event wait mode %u", mode);
}
static void queue_note_event_signal(volatile LONG *word, uint32_t value) {
    // An exact wait can observe a signal even if its producer writes the same
    // word again before finishing this replay piece.
    for (struct pending_queue *p = pending_queues; p; p = p->next)
        if (p->wait_event.word == word && event_satisfies(p->wait_event.mode, value, p->wait_event.value))
            p->event_ready = 1;
}
static struct command_sync read_sync_record(const uint8_t *bytes, struct command_header h) {
    struct command_sync r;
    if (h.size != sizeof(r)) dm_fail("nvnQueueSubmitCommands: invalid sync record");
    memcpy(&r, bytes, sizeof(r));
    return r;
}
static const struct headless_sync *queued_sync(const struct headless_queue *q, const struct headless_sync *s) {
    if (!sync_valid(s) || s->device != q->device) dm_fail("nvnQueueSubmitCommands: invalid sync or device");
    return s;
}
static struct command_header submit_header(const uint8_t *bytes, size_t size) {
    struct command_header h;
    if (size < sizeof(h)) dm_fail("nvnQueueSubmitCommands: truncated record header");
    memcpy(&h, bytes, sizeof(h));
    if (h.size < sizeof(h) || h.size % 8 || h.size > size)
        dm_fail("nvnQueueSubmitCommands: invalid record size");
    return h;
}

// The direct path reads guest command memory without copying. If a wait
// cannot pass, the caller retains only the suffix, starting at this record.
static size_t submit_piece(const struct headless_queue *q, struct pending_queue *pending,
                           const uint8_t *bytes, size_t size,
                           struct dm_m4_draw_state **draw_state, size_t *copies, size_t *draws) {
    for (size_t at = 0; at < size;) {
        struct command_header h = submit_header(bytes + at, size - at);
        switch (h.op) {
        case COMMAND_CLEAR_BUFFER: {
            struct command_clear_buffer r;
            if (h.size != sizeof(r)) dm_fail("nvnQueueSubmitCommands: invalid buffer clear size");
            memcpy(&r, bytes + at, sizeof(r));
            uint32_t *dst = draw_resolve_buffer(NULL, r.dst, r.bytes, 1);
            if (!dst || ((r.dst | r.bytes) & 3) || r.pad) dm_fail("nvnQueueSubmitCommands: invalid buffer clear range");
            for (size_t i = 0; i < r.bytes / sizeof(*dst); ++i) dst[i] = r.value;
            break;
        }
        case COMMAND_UPDATE_UNIFORM: {
            struct command_uniform r;
            if (h.size < sizeof(r)) dm_fail("nvnQueueSubmitCommands: truncated uniform update");
            memcpy(&r, bytes + at, sizeof(r));
            if (r.bytes > h.size - sizeof(r) || h.size - sizeof(r) - r.bytes >= 8 ||
                !dm_guest_range((void *)(uintptr_t)r.dst, r.bytes, 1)) dm_fail("nvnQueueSubmitCommands: invalid uniform update");
            memcpy((void *)(uintptr_t)r.dst, bytes + at + sizeof(r), r.bytes);
            break;
        }
        case COMMAND_WAIT_EVENT:
        case COMMAND_SIGNAL_EVENT: {
            struct command_event r;
            if (h.size != sizeof(r)) dm_fail("nvnQueueSubmitCommands: invalid event record");
            memcpy(&r, bytes + at, sizeof(r));
            volatile LONG *word = event_storage(r.word);
            if (h.op == COMMAND_SIGNAL_EVENT) {
                queue_note_event_signal(word, event_publish(word, r.mode, r.value));
            } else {
                if (!(pending && pending->event_ready) && !event_satisfies(r.mode, event_load(word), r.value)) {
                    if (pending) pending->wait_event = r;
                    return at;
                }
                if (pending) {
                    pending->wait_event.word = NULL;
                    pending->event_ready = 0;
                }
            }
            break;
        }
        case COMMAND_WAIT_SYNC:
        case COMMAND_FENCE_SYNC: {
            const struct command_sync r = read_sync_record(bytes + at, h);
            if (pending) {
                struct queue_receipt *receipt = r.target.receipt;
                if (h.op == COMMAND_FENCE_SYNC) receipt_complete(receipt);
                else {
                    if (receipt && !receipt->complete) return at;
                    receipt_drop(receipt);
                }
            } else {
                const struct headless_sync *s = queued_sync(q, r.target.sync);
                if (h.op == COMMAND_FENCE_SYNC) sync_clear(s);
                else if (sync_pending(s)) return at;
            }
            break;
        }
        case COMMAND_BARRIER: {
            const uint32_t supported = NVN_BARRIER_ORDER_PRIMITIVES_BIT | NVN_BARRIER_ORDER_FRAGMENTS_BIT |
                NVN_BARRIER_ORDER_FRAGMENTS_TILED_BIT | NVN_BARRIER_ORDER_INDIRECT_DATA_BIT |
                NVN_BARRIER_INVALIDATE_TEXTURE_BIT | NVN_BARRIER_INVALIDATE_SHADER_BIT |
                NVN_BARRIER_INVALIDATE_TEXTURE_DESCRIPTOR_BIT | NVN_BARRIER_INVALIDATE_ZCULL_BIT;
            struct command_barrier barrier;
            if (h.size != sizeof(barrier)) dm_fail("nvnQueueSubmitCommands: invalid barrier size");
            memcpy(&barrier, bytes + at, sizeof(barrier));
            if (barrier.bits & ~supported) dm_fail("nvnQueueSubmitCommands: unsupported barrier mask %#x", barrier.bits);
            // CPU replay has already completed earlier records in order and
            // has no device caches to invalidate.
            break;
        }
        case COMMAND_COPY_BUFFER_TO_TEXTURE: {
            struct command_copy_buffer_to_texture copy;
            if (h.size != sizeof(copy)) dm_fail("nvnQueueSubmitCommands: invalid copy record size");
            memcpy(&copy, bytes + at, sizeof(copy));
            submit_buffer_to_texture(q, &copy);
            ++*copies;
            break;
        }
        case COMMAND_COPY_TEXTURE_TO_TEXTURE: {
            struct command_copy_texture_to_texture copy;
            if (h.size != sizeof(copy)) dm_fail("nvnQueueSubmitCommands: invalid texture copy record size");
            memcpy(&copy, bytes + at, sizeof(copy));
            submit_texture_to_texture(q, &copy);
            break;
        }
        case COMMAND_RESET_SAMPLES: {
            if (h.size != sizeof(h)) dm_fail("nvnQueueSubmitCommands: invalid reset counter size");
            if (!*draw_state) *draw_state = draw_queue_state(q);
            struct dm_m4_guest_resources *r = &CONTAINING_RECORD(*draw_state, struct draw_queue_state, state)->resources;
            r->samples_passed = 0;
            r->count_samples = 1;
            break;
        }
        case COMMAND_REPORT_COUNTER | (NVN_COUNTER_TYPE_SAMPLES_PASSED << 8):
        case COMMAND_REPORT_COUNTER | (NVN_COUNTER_TYPE_TIMESTAMP << 8):
        case COMMAND_REPORT_COUNTER | (NVN_COUNTER_TYPE_TIMESTAMP_TOP << 8): {
            struct command_report_counter command;
            if (h.size != sizeof(command)) dm_fail("nvnQueueSubmitCommands: invalid timestamp report size");
            memcpy(&command, bytes + at, sizeof(command));
            void *destination = (void *)(uintptr_t)command.destination;
            if (!dm_guest_range(destination, sizeof(NVNcounterData), 1))
                dm_fail("nvnQueueSubmitCommands: invalid timestamp report destination");
            // All earlier work has completed synchronously. TOP and BOTTOM
            // sample the same replay boundary; QPC includes host replay costs.
            NVNcounterData report = {.timestamp = dm_guest_time_ns()};
            if ((h.op >> 8) == NVN_COUNTER_TYPE_SAMPLES_PASSED) {
                if (!*draw_state) *draw_state = draw_queue_state(q);
                report.counter = CONTAINING_RECORD(*draw_state, struct draw_queue_state, state)->resources.samples_passed;
            }
            memcpy(destination, &report, sizeof(report));
            break;
        }
        case COMMAND_RENDER_ENABLE | (RENDER_IF_EQUAL << 8):
        case COMMAND_RENDER_ENABLE | (RENDER_IF_NOT_EQUAL << 8):
        case COMMAND_RENDER_ENABLE | (RENDER_ALWAYS << 8):
        case COMMAND_RENDER_ENABLE | (RENDER_NEVER << 8): {
            struct command_render_enable r;
            if (h.size != sizeof(r)) dm_fail("nvnQueueSubmitCommands: invalid render-enable record");
            memcpy(&r, bytes + at, sizeof(r));
            uint32_t mode = h.op >> 8, disabled = mode == RENDER_NEVER;
            if (mode <= RENDER_IF_NOT_EQUAL) {
                // All earlier writes are fenced at this point. Compare counters, not timestamps.
                const uint8_t *pair = (const uint8_t *)(uintptr_t)r.address;
                uint64_t a, b;
                if (!dm_guest_range(pair, sizeof(NVNcounterData) + sizeof(b), 0))
                    dm_fail("nvnQueueSubmitCommands: unmapped conditional render counters");
                memcpy(&a, pair, sizeof(a));
                memcpy(&b, pair + sizeof(NVNcounterData), sizeof(b));
                disabled = mode == RENDER_IF_EQUAL ? a != b : a == b;
            }
            if (!*draw_state) *draw_state = draw_queue_state(q);
            (*draw_state)->render_disabled = disabled;
            break;
        }
        case COMMAND_CLEAR_TEXTURE: {
            struct command_clear_texture r;
            if (h.size != sizeof(r)) dm_fail("nvnQueueSubmitCommands: invalid texture clear size");
            memcpy(&r, bytes + at, sizeof(r));
            struct draw_resolve_context ctx = {q->device, NULL};
            struct dm_m4_draw_resolver resolve = {.context = &ctx, .texture_storage = draw_resolve_storage};
            const char *err = dm_m4_clear_texture(&r.texture, &r.rect, &resolve, &r.clear);
            if (err) dm_fail("nvnQueueSubmitCommands: clear texture: %s", err);
            break;
        }
        case COMMAND_CLEAR_ATTACHMENT: {
            struct command_clear r;
            if (h.size != sizeof(r)) dm_fail("nvnQueueSubmitCommands: invalid attachment clear size");
            memcpy(&r, bytes + at, sizeof(r));
            if (!*draw_state) *draw_state = draw_queue_state(q);
            if ((*draw_state)->render_disabled) break;
            struct draw_resolve_context ctx = {q->device, *draw_state};
            struct dm_m4_draw_resolver resolve = {.context = &ctx, .texture_storage = draw_resolve_storage};
            const char *err = dm_m4_clear_attachment(*draw_state, &resolve, &r.clear);
            if (err) dm_fail("nvnQueueSubmitCommands: clear %u[%u]: %s", r.clear.type, r.clear.index, err);
            break;
        }
        case COMMAND_DRAW_ELEMENTS:
        case COMMAND_DRAW_INSTANCED:
        case COMMAND_DRAW_ARRAYS_INSTANCED:
            if (!*draw_state) *draw_state = draw_queue_state(q);
            if ((*draw_state)->render_disabled) break;
            *draws += (size_t)draw_replay_indexed(q, draw_state, h, bytes + at);
            break;
        case COMMAND_DRAW_MESH_TASKS:
        case COMMAND_DRAW_MESH_INDIRECT:
            if (!*draw_state) *draw_state = draw_queue_state(q);
            if ((*draw_state)->render_disabled) break;
            *draws += (size_t)draw_replay_mesh(q, draw_state, h, bytes + at);
            break;
        case COMMAND_DISPATCH_COMPUTE:
        case COMMAND_DISPATCH_INDIRECT:
            draw_replay_dispatch(q, draw_state, h, bytes + at + sizeof(h));
            break;
        default:
            if (!draw_reduce(q, draw_state, h, bytes + at + sizeof(h)))
                dm_fail("nvnQueueSubmitCommands: unknown command opcode %#x", h.op);
            break;
        }
        at += h.size;
    }
    return size;
}

static struct pending_queue *queue_pending(const struct headless_queue *q) {
    for (struct pending_queue *p = pending_queues; p; p = p->next)
        if (p->queue == q) return p;
    return NULL;
}
static void queue_device_finalizing(const struct nvn_device *d) {
    for (const struct pending_queue *p = pending_queues; p; p = p->next)
        if (p->device == d) dm_fail("nvnDeviceFinalize: device still has pending queue work");
}
static int reported_draw;
static void report_draws(size_t draws) {
    if (draws && !reported_draw) {
        reported_draw = 1;
        printf("[nvn] first indexed draw submission completed (%zu draws)\n", draws);
        fflush(stdout);
    }
}
static void queue_run_ready(void) {
    int progress;
    do {
        progress = 0;
        for (struct pending_queue **link = &pending_queues; *link;) {
            struct pending_queue *p = *link;
            struct dm_m4_draw_state *state = NULL;
            size_t copies = 0, draws = 0;
            if (!queue_valid(p->queue)) dm_fail("NVN queue: deferred work lost its queue or device");
            while (p->first) {
                struct queue_work *w = p->first;
                uint8_t *bytes = queue_work_bytes(w);
                if (w->present) {
                    const struct queue_present_work *present = (const void *)bytes;
                    if (!window_valid(present->window) || present->window->state.device != p->queue->device)
                        dm_fail("nvnQueuePresentTexture: deferred present lost its window or device");
                    replay_present(present->window, &present->texture, &present->crop, present->interval, present->index);
                    progress = 1;
                } else {
                    size_t n = submit_piece(p->queue, p, bytes + w->at, w->size - w->at, &state, &copies, &draws);
                    progress |= n != 0;
                    w->at += n;
                    if (w->at != w->size) break;
                }
                p->first = w->next;
                free(w);
            }
            report_draws(draws);
            if (p->first) link = &p->next;
            else {
                *link = p->next;
                free(p);
            }
        }
    } while (progress && pending_queues);
}
static DWORD WINAPI queue_worker(void *unused) {
    (void)unused;
    AcquireSRWLockExclusive(&submit_lock);
    for (;;) {
        while (!pending_queues) wait_for_submit_change(INFINITE);
        queue_run_ready();
        // Mapped event words can also be written outside nvnEventSignal.
        // Poll only while a queue is parked; no submit lock is held asleep.
        if (pending_queues) wait_for_submit_change(1);
    }
}
static struct pending_queue *queue_park(const struct headless_queue *q) {
    static int worker_started;
    if (!worker_started) {
        HANDLE thread = CreateThread(NULL, 0, queue_worker, NULL, 0, NULL);
        if (!thread) dm_fail("NVN queue: cannot start dependency worker (%lu)", GetLastError());
        CloseHandle(thread);
        worker_started = 1;
    }
    struct pending_queue *p = calloc(1, sizeof(*p));
    if (!p) dm_fail("NVN queue: cannot allocate a continuation");
    p->queue = q;
    p->device = q->device;
    p->tail = &p->first;
    p->next = pending_queues;
    pending_queues = p;
    WakeAllConditionVariable(&submit_changed);
    return p;
}
static struct queue_work *queue_append(struct pending_queue *p, const void *bytes, size_t size) {
    if (size > SIZE_MAX - sizeof(struct queue_work)) dm_fail("NVN queue: continuation is too large");
    struct queue_work *w = malloc(sizeof(*w) + size);
    if (!w) dm_fail("NVN queue: cannot retain %zu command bytes", size);
    w->next = NULL;
    w->at = 0;
    w->size = size;
    w->present = 0;
    memcpy(queue_work_bytes(w), bytes, size);
    *p->tail = w;
    p->tail = &w->next;
    return w;
}
static int queue_present_later(const struct headless_queue *q, struct headless_window *w,
                              const struct headless_texture *t, int index) {
    struct pending_queue *p = queue_pending(q);
    if (!p) return 0;
    const struct queue_present_work present = {w, *t, w->crop, w->state.interval, index};
    queue_append(p, &present, sizeof(present))->present = 1;
    w->queued |= 1u << index;
    return 1;
}
static void queue_retain_piece(struct pending_queue *p, const uint8_t *source, size_t size) {
    uint8_t *bytes = queue_work_bytes(queue_append(p, source, size));
    for (size_t at = 0; at < size;) {
        struct command_header h = submit_header(bytes + at, size - at);
        if (h.op == COMMAND_WAIT_SYNC || h.op == COMMAND_FENCE_SYNC) {
            struct command_sync r = read_sync_record(bytes + at, h);
            const struct headless_sync *s = queued_sync(p->queue, r.target.sync);
            r.target.receipt = h.op == COMMAND_FENCE_SYNC ? sync_fence_pending(s) : receipt_take(sync_pending(s));
            memcpy(bytes + at, &r, sizeof(r));
        }
        at += h.size;
    }
}
static void queue_append_sync(struct pending_queue *p, unsigned op, struct queue_receipt *r) {
    const struct command_sync command = {{op, sizeof(command)}, {.receipt = r}};
    queue_append(p, &command, sizeof(command));
}
static void queue_finish_locked(const struct headless_queue *q) {
    struct pending_queue *p = queue_pending(q);
    if (!p) return;
    struct queue_receipt *r = receipt_new();
    queue_append_sync(p, COMMAND_FENCE_SYNC, receipt_take(r));
    while (!r->complete) wait_for_submit_change(INFINITE);
    receipt_drop(r);
}
static void queue_close_locked(const struct headless_queue *q) {
    while (queue_pending(q)) queue_finish_locked(q);
}
void nvn_queue_flush(const struct headless_queue *q) {
    AcquireSRWLockExclusive(&submit_lock);
    if (!queue_valid(q)) dm_fail("nvnQueueFlush: invalid queue");
    queue_run_ready();
    ReleaseSRWLockExclusive(&submit_lock);
}
void nvn_queue_fence_sync(const struct headless_queue *q, struct headless_sync *s, NVNsyncCondition condition, int flags) {
    check_fence_options(condition, flags);
    AcquireSRWLockExclusive(&submit_lock);
    if (!queue_valid(q) || !sync_valid(s) || q->device != s->device) dm_fail("nvnQueueFenceSync: invalid queue or sync");
    struct pending_queue *p = queue_pending(q);
    if (p) queue_append_sync(p, COMMAND_FENCE_SYNC, sync_fence_pending(s));
    else sync_clear(s);
    ReleaseSRWLockExclusive(&submit_lock);
}
NVNboolean nvn_queue_wait_sync(const struct headless_queue *q, const struct headless_sync *s) {
    AcquireSRWLockExclusive(&submit_lock);
    int valid = queue_valid(q) && sync_valid(s) && q->device == s->device;
    if (valid) {
        struct queue_receipt *r = sync_pending(s);
        if (r) {
            struct pending_queue *p = queue_pending(q);
            if (!p) p = queue_park(q);
            queue_append_sync(p, COMMAND_WAIT_SYNC, receipt_take(r));
        }
    }
    ReleaseSRWLockExclusive(&submit_lock);
    return (NVNboolean)valid;
}
void nvn_command_handle_fini(const struct nvn_device *d, NVNcommandHandle handle) {
    // The production NVN2 entry returns without accessing the handle. The
    // headless list likewise lives entirely in caller-owned control memory.
    (void)d;
    (void)handle;
}
void nvn_queue_finish(const struct headless_queue *q) {
    AcquireSRWLockExclusive(&submit_lock);
    if (!queue_valid(q)) dm_fail("nvnQueueFinish: invalid queue");
    queue_finish_locked(q);
    ReleaseSRWLockExclusive(&submit_lock);
}
void nvn_queue_submit(const struct headless_queue *q, int count, const NVNcommandHandle *handles) {
    static int reported;
    AcquireSRWLockExclusive(&submit_lock);
    if (!queue_valid(q)) dm_fail("nvnQueueSubmitCommands: invalid queue");
    if (count < 0 || (count && !dm_guest_range(handles, (size_t)count * sizeof(*handles), 0)))
        dm_fail("nvnQueueSubmitCommands: invalid handle array");
    struct pending_queue *pending = queue_pending(q);
    struct dm_m4_draw_state *draw_state = NULL;
    size_t copies = 0, draws = 0;
    for (int i = 0; i < count; ++i) {
        struct command_list list = read_list(q->device, handles[i]);
        struct command_piece piece;
        while (next_piece(&list, &piece)) {
            const uint8_t *bytes = piece.bytes;
            if (piece.size % 8 || !dm_guest_range(bytes, piece.size, 0))
                dm_fail("nvnQueueSubmitCommands: invalid command piece");
            size_t n = pending ? 0 : submit_piece(q, NULL, bytes, piece.size, &draw_state, &copies, &draws);
            if (n != piece.size) {
                if (!pending) pending = queue_park(q);
                queue_retain_piece(pending, bytes + n, piece.size - n);
            }
        }
    }
    if (count && !reported) {
        reported = 1;
        printf("[nvn] first headless submission: %d command lists, %zu buffer-to-texture copies\n", count, copies);
        fflush(stdout);
    }
    report_draws(draws);
    queue_run_ready();
    ReleaseSRWLockExclusive(&submit_lock);
}
void nvn_command_callback(struct command_buffer *c, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC callback) {
    command_check(c, 1);
    c->callback = callback;
}
void nvn_command_callback_data(struct command_buffer *c, void *data) { command_check(c, 1); c->callback_data = data; }
void nvn_command_callback_enabled(struct command_buffer *c, NVNboolean enable) { command_check(c, 1); c->callback_enabled = enable != 0; }
void nvn_command_add_memory(struct command_buffer *c, const struct pool *p, ptrdiff_t offset, size_t size) {
    command_check(c, 1);
    // A guest can round its last grant up to the minimum command block size
    // even when its measured list is shorter. Validate actual bytes on reserve
    // and again when retaining the recorded span, not unwritten grant padding.
    if (!size || !pool_storage(p, offset, 1, 4, 1))
        dm_fail("nvnCommandBufferAddCommandMemory: invalid backing range (pool=%p base=%p length=%#zx offset=%#tx size=%#zx lr=%p)",
                (void *)p, dm_guest_range(p, sizeof(*p), 0) ? p->mem : NULL,
                dm_guest_range(p, sizeof(*p), 0) ? p->size : 0, offset, size, _ReturnAddress());
    // AddCommandMemory can be called from the command-memory callback. Save
    // the old span now, but do not invoke a control callback inside it.
    if (c->recording && c->command_used != c->first_command)
        c->closed_piece = command_tail(c);
    c->pool = p;
    c->offset = offset;
    c->command_size = size;
    c->command_used = c->first_command = 0;
}
void nvn_command_add_control(struct command_buffer *c, void *mem, size_t size) {
    command_check(c, 1);
    if (((uintptr_t)mem & 7) || !size || !dm_guest_range(mem, size, 1)) dm_fail("nvnCommandBufferAddControlMemory: invalid backing range");
    c->control = mem;
    c->control_size = size;
    c->control_used = 0;
}
size_t nvn_command_size(const struct command_buffer *c) { command_check(c, 0); return c->command_size; }
size_t nvn_command_control_size(const struct command_buffer *c) { command_check(c, 0); return c->control_size; }
size_t nvn_command_used(const struct command_buffer *c) { command_check(c, 0); return c->command_used; }
size_t nvn_command_control_used(const struct command_buffer *c) { command_check(c, 0); return c->control_used; }
size_t nvn_command_free(const struct command_buffer *c) { return nvn_command_size(c) - nvn_command_used(c); }
size_t nvn_command_control_free(const struct command_buffer *c) { return nvn_command_control_size(c) - nvn_command_control_used(c); }

struct buffer_builder { const struct nvn_device *device; const struct pool *pool; ptrdiff_t offset; size_t size; };
struct headless_buffer { uint32_t magic; const struct pool *pool; uint64_t reserved; ptrdiff_t offset; size_t size; };
#define HEADLESS_BUFFER UINT32_C(0x00465542)
static_assert(sizeof(struct buffer_builder) <= sizeof(NVNbufferBuilder), "buffer builder prefix");
static_assert(sizeof(struct headless_buffer) <= sizeof(NVNbuffer), "buffer prefix");
static_assert(offsetof(struct headless_buffer, offset) == 0x18 && offsetof(struct headless_buffer, size) == 0x20, "guest buffer fields");
static void buffer_builder_check(const struct buffer_builder *b) {
    if (!dm_guest_range(b, sizeof(*b), 1)) dm_fail("NVN buffer: invalid builder");
}
void nvn_buffer_defaults(struct buffer_builder *b) {
    buffer_builder_check(b);
    b->pool = NULL;
    b->offset = 0;
    b->size = 0;
}
void nvn_buffer_device(struct buffer_builder *b, const struct nvn_device *d) {
    buffer_builder_check(b);
    if (!dm_guest_range(d, sizeof(*d), 0) || d->magic != HEADLESS_NVN_DEVICE) dm_fail("nvnBufferBuilderSetDevice: invalid device");
    b->device = d;
}
void nvn_buffer_storage(struct buffer_builder *b, const struct pool *p, ptrdiff_t off, size_t size) {
    buffer_builder_check(b);
    b->pool = p;
    b->offset = off;
    b->size = size;
}
NVNboolean nvn_buffer_init(struct headless_buffer *buf, const struct buffer_builder *b) {
    if (!dm_guest_range(buf, sizeof(*buf), 1) || !dm_guest_range(b, sizeof(*b), 0) ||
        !dm_guest_range(b->device, sizeof(*b->device), 0) || b->device->magic != HEADLESS_NVN_DEVICE ||
        !pool_storage(b->pool, b->offset, 1, 1, 0) || (b->pool->flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL)) {
        fprintf(stderr, "[nvn] BufferInitialize rejected %p builder=%p (lr=%p)\n",
                (void *)buf, (const void *)b, _ReturnAddress());
        return 0;
    }
    // The driver does not validate the nominal end at init. Validate any real CPU range at use.
    *buf = (struct headless_buffer){ HEADLESS_BUFFER, b->pool, 0, b->offset, b->size };
    return 1;
}
static void buffer_validate(const struct headless_buffer *b, int write, const char *api, void *caller) {
    if (!dm_guest_range(b, sizeof(*b), write)) dm_fail("%s: invalid NVNbuffer %p (lr=%p)", api, (void *)b, caller);
    if (b->magic != HEADLESS_BUFFER)
        dm_fail("%s: invalid NVNbuffer %p magic=%#x pool=%p offset=%#tx size=%#zx (lr=%p)",
                api, (void *)b, b->magic, (void *)b->pool, b->offset, b->size, caller);
}
#define buffer_check(b, write) buffer_validate((b), (write), __func__, _ReturnAddress())
void nvn_buffer_fini(struct headless_buffer *b) {
    // Teardown walks optional buffer slots as well as live ones. A cleared slot owns nothing.
    if (dm_guest_range(b, sizeof(*b), 1) && !b->magic && !b->pool && !b->reserved && !b->offset && !b->size) return;
    buffer_check(b, 1);
    *b = (struct headless_buffer){0};
}
uint64_t nvn_buffer_address(const struct headless_buffer *b) {
    buffer_check(b, 0);
    void *mem = pool_storage(b->pool, b->offset, 1, 1, 0);
    if (!mem) dm_fail("nvnBufferGetAddress: invalid backing range");
    return (uintptr_t)mem;
}
void *nvn_buffer_map(const struct headless_buffer *b) {
    buffer_check(b, 0);
    pool_check(b->pool, 0);
    if (b->pool->flags & (POOL_CPU_NO_ACCESS | POOL_VIRTUAL)) return NULL;
    void *mem = pool_storage(b->pool, b->offset, b->size, 1, 1);
    if (!mem) dm_fail("nvnBufferMap: invalid backing range");
    return mem;
}
size_t nvn_buffer_size(const struct headless_buffer *b) { buffer_check(b, 0); return b->size; }
ptrdiff_t nvn_buffer_offset(const struct headless_buffer *b) { buffer_check(b, 0); return b->offset; }
const struct pool *nvn_buffer_pool(const struct headless_buffer *b) { buffer_check(b, 0); return b->pool; }
void nvn_buffer_cache_range(const struct headless_buffer *b, ptrdiff_t offset, size_t size) {
    buffer_check(b, 0);
    if (b->offset < 0 || offset < 0 || offset > PTRDIFF_MAX - b->offset ||
        (size_t)offset > b->size || size > b->size - (size_t)offset)
        dm_fail("NVN buffer cache operation: invalid range");
    nvn_pool_cache_range(b->pool, b->offset + offset, size);
}

extern const char m3_nvn_names[], m3_nvn_names_end[];
extern const unsigned char m3_nvn_stubs[], m3_nvn_stubs_end[];

static const char *next_nvn_name(const char *start, size_t *length) {
    const char *end = memchr(start, '\n', (uintptr_t)m3_nvn_names_end - (uintptr_t)start);
    const char *stop = end ? end : m3_nvn_names_end;
    *length = (uintptr_t)stop - (uintptr_t)start;
    if (*length && start[*length - 1] == '\r') --*length;
    return end ? end + 1 : m3_nvn_names_end;
}

_Noreturn void m3_missing_nvn(unsigned ordinal, uintptr_t lr, uint64_t a0, uint64_t a1, uint64_t a2) {
    const char *p = m3_nvn_names;
    for (unsigned i = 0; (uintptr_t)p < (uintptr_t)m3_nvn_names_end; ++i) {
        size_t length;
        const char *next = next_nvn_name(p, &length);
        if (i == ordinal)
            dm_fail("headless NVN call: %.*s is not implemented (lr=%#" PRIxPTR ", args=%#" PRIx64
                    ",%#" PRIx64 ",%#" PRIx64 ")", (int)length, p, lr, a0, a1, a2);
        p = next;
    }
    dm_fail("invalid NVN stub ordinal %u", ordinal);
}

uintptr_t nvn_headless_lookup(const char *guest_name) {
    char name[128];
    if (!dm_guest_string(guest_name, name, sizeof(name))) dm_fail("NVN lookup: invalid function name");
    uintptr_t entry = !strncmp(name, "nvn", 3) ? dm_m3_import(name) : 0;
    if (entry) return entry;
    size_t want = strlen(name), i = 0;
    for (const char *p = m3_nvn_names; (uintptr_t)p < (uintptr_t)m3_nvn_names_end; ++i) {
        size_t length;
        const char *next = next_nvn_name(p, &length);
        if (length == want && !memcmp(p, name, want)) {
            uintptr_t stub = (uintptr_t)m3_nvn_stubs + 8 * i;
            if (stub >= (uintptr_t)m3_nvn_stubs_end) dm_fail("NVN name table exceeds stub capacity");
            return stub;
        }
        p = next;
    }
    fprintf(stderr, "[RED] unknown NVN procedure: %s\n", name);
    return 0;
}
uintptr_t nvn_device_proc(const struct nvn_device *d, const char *name) {
    if (d && (!dm_guest_range(d, sizeof(*d), 0) || d->magic != HEADLESS_NVN_DEVICE))
        dm_fail("nvnDeviceGetProcAddress: invalid device");
    return nvn_headless_lookup(name);
}

#define M3_IMPORT(symbol, function) extern void m3_##function(void);
#define M3_ALIAS(symbol, function)
#include "m3_imports.inc"
#undef M3_ALIAS
#undef M3_IMPORT

uintptr_t dm_m3_import(const char *name) {
    static const struct { const char *name; void (*thunk)(void); } imports[] = {
#define M3_IMPORT(symbol, function) {symbol, m3_##function},
#define M3_ALIAS M3_IMPORT
#include "m3_imports.inc"
#undef M3_ALIAS
#undef M3_IMPORT
    };
    for (size_t i = 0; i < ARRAYSIZE(imports); ++i)
        if (!strcmp(name, imports[i].name)) return (uintptr_t)imports[i].thunk;
    return 0;
}
