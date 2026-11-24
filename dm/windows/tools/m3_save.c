// Windows SDK save/temporary mounts. The host launcher's path is configuration;
// everything after the guest's mount prefix is a key in the save database.
#include "m3_save.h"
#include "../../host/account_users.h"
#include <stdio.h>
#include <stdlib.h>

static struct fs_save_mount mounts[16];
static m3_save_store *account_store;
static m3_save_store *temporary_store;

uint32_t m3_fs_save_result(m3_save_result result) {
    switch (result) {
    case M3_SAVE_OK: return FS_OK;
    case M3_SAVE_NOT_FOUND: return FS_NOT_FOUND;
    case M3_SAVE_EXISTS: return FS_EXISTS;
    case M3_SAVE_NOT_EMPTY: return FS_DIRECTORY_NOT_EMPTY;
    case M3_SAVE_OUT_OF_RANGE: return FS_OUT_OF_RANGE;
    case M3_SAVE_APPEND_REQUIRED: return FS_APPEND_REQUIRED;
    default: return FS_FAILURE;
    }
}

const struct fs_save_mount *m3_fs_save_mount_locked(const char *name, size_t length) {
    for (size_t i = 0; i < ARRAYSIZE(mounts); ++i)
        if (mounts[i].store && strlen(mounts[i].name) == length && !memcmp(name, mounts[i].name, length))
            return &mounts[i];
    return NULL;
}

const struct fs_save_mount *m3_fs_save_path_locked(const char *path, const char **subpath) {
    const char *colon = strchr(path, ':');
    if (!colon || colon[1] != '/') return NULL;
    *subpath = colon + 1;
    return m3_fs_save_mount_locked(path, (size_t)(colon - path));
}

static int copy_mount(const char *guest, char name[FS_MOUNT_CAPACITY]) {
    if (!m3_fs_copy_path(guest, name, FS_MOUNT_CAPACITY) || !*name || *name == '@' || strpbrk(name, ":/"))
        return 0;
    if (!name[1] && ((*name >= 'a' && *name <= 'z') || (*name >= 'A' && *name <= 'Z'))) return 0;
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, NULL, 0) > 0;
}

static uint32_t open_account_store(m3_save_store **out) {
    wchar_t *wide = NULL;
    size_t count = 0;
    uint32_t result = FS_TARGET_NOT_FOUND;
    if (!_wdupenv_s(&wide, &count, L"DM_SAVE") && wide && *wide) {
        int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
        char *path = bytes > 0 ? malloc((size_t)bytes) : NULL;
        if (path && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, path, bytes, NULL, NULL)) {
            m3_save_result status = m3_save_mount(out, path, DM_UID0, DM_UID1);
            if (status != M3_SAVE_INVALID && status != M3_SAVE_NOT_FOUND) result = m3_fs_save_result(status);
        }
        free(path);
    }
    free(wide);
    if (result != FS_OK)
        fprintf(stderr, "[RED] fs: DM_SAVE must name an existing absolute, writable Windows save directory (result %#x)\n", result);
    return result;
}

static uint32_t mount_storage(const char *guest_name, int temporary) {
    char name[FS_MOUNT_CAPACITY];
    if (!copy_mount(guest_name, name)) return FS_INVALID_MOUNT;
    size_t length = strlen(name);
    AcquireSRWLockExclusive(&m3_fs_lock);
    uint32_t result = FS_MOUNT_EXISTS;
    if (!m3_fs_save_mount_locked(name, length) && !m3_fs_rom_mounted_locked(name, length)) {
        size_t slot = 0;
        while (slot < ARRAYSIZE(mounts) && mounts[slot].store) ++slot;
        result = FS_OPEN_LIMIT;
        if (slot < ARRAYSIZE(mounts)) {
            m3_save_store **store = temporary ? &temporary_store : &account_store;
            result = FS_OK;
            if (!*store) result = temporary ? m3_fs_save_result(m3_save_mount(store, NULL, 0, 0)) : open_account_store(store);
            if (result == FS_OK) {
                memcpy(mounts[slot].name, name, length + 1);
                mounts[slot].store = *store;
                printf("[fs] mounted %s: %s\n", name, temporary ? "temporary storage" : "account save data");
                fflush(stdout);
            }
        }
    }
    ReleaseSRWLockExclusive(&m3_fs_lock);
    return result;
}

static int launch_user(const void *guest_uid) {
    return dm_guest_range(guest_uid, 2 * sizeof(uint64_t), 0) && dm_uid_user(guest_uid) == 0;
}

uint32_t fs_mount_save(const char *name, const void *uid) {
    return launch_user(uid) ? mount_storage(name, 0) : FS_TARGET_NOT_FOUND;
}

uint32_t fs_mount_temp(const char *name) { return mount_storage(name, 1); }

uint32_t fs_ensure_save(const void *uid) {
    if (!launch_user(uid)) return FS_TARGET_NOT_FOUND;
    AcquireSRWLockExclusive(&m3_fs_lock);
    uint32_t result = FS_OK;
    if (!account_store) {
        m3_save_store *store = NULL;
        result = open_account_store(&store);
        if (store) m3_save_unmount(store);
    }
    ReleaseSRWLockExclusive(&m3_fs_lock);
    return result;
}

void m3_fs_save_unmount_locked(const char *name) {
    for (size_t i = 0; i < ARRAYSIZE(mounts); ++i) {
        struct fs_save_mount *m = &mounts[i];
        if (!m->store || strcmp(m->name, name)) continue;
        if (m3_fs_save_busy_locked(m, NULL)) dm_fail("fs: unmount with open handles on %s", name);
        m3_save_store *store = m->store;
        m->store = NULL;
        for (size_t j = 0; j < ARRAYSIZE(mounts); ++j)
            if (mounts[j].store == store) return;
        if (store == temporary_store) {
            // Committed temporary files last for the launch, not on disk.
            if (m3_save_rollback(store) == M3_SAVE_OK) return;
            temporary_store = NULL;
        } else account_store = NULL;
        m3_save_unmount(store);
        return;
    }
}

static uint32_t commit_locked(const struct fs_save_mount *mount) {
    if (m3_fs_save_busy_locked(NULL, mount->store)) return FS_WRITE_FILE_OPEN;
    return m3_fs_save_result(m3_save_commit(mount->store));
}

uint32_t fs_commit(const char *guest) {
    char name[FS_MOUNT_CAPACITY];
    if (!copy_mount(guest, name)) return FS_INVALID_MOUNT;
    size_t length = strlen(name);
    AcquireSRWLockExclusive(&m3_fs_lock);
    const struct fs_save_mount *mount = m3_fs_save_mount_locked(name, length);
    uint32_t result = mount ? commit_locked(mount) :
                      m3_fs_rom_mounted_locked(name, length) ? FS_OK : FS_NOT_MOUNTED;
    ReleaseSRWLockExclusive(&m3_fs_lock);
    return result;
}

uint32_t fs_commit_opt(const char *name, const uint32_t *option) {
    // Bit 0 clears the restored-from-backup flag; these stores never restore a backup.
    if (!dm_guest_range(option, sizeof(*option), 0) || (*option & ~UINT32_C(1))) return FS_FAILURE;
    return fs_commit(name);
}

static uint32_t mutate_path(const char *guest, m3_save_type type, int64_t size,
                            int create, int recursive, int keep_dir) {
    char path[FS_PATH_CAPACITY];
    if (!m3_fs_copy_path(guest, path, sizeof(path))) return FS_FAILURE;
    AcquireSRWLockExclusive(&m3_fs_lock);
    const char *rest = NULL;
    const struct fs_save_mount *mount = m3_fs_save_path_locked(path, &rest);
    uint32_t result;
    if (mount) {
        result = m3_fs_save_result(create ? m3_save_create(mount->store, rest, type, size) :
                                  m3_save_remove(mount->store, rest, type, recursive, keep_dir));
    } else if (rest && m3_fs_rom_mounted_locked(path, (size_t)(rest - path - 1))) {
        result = create && m3_fs_rom_find_locked(path, rest) >= 0 ? FS_EXISTS : FS_ROM_READ_ONLY;
    } else result = FS_NOT_MOUNTED;
    ReleaseSRWLockExclusive(&m3_fs_lock);
    return result;
}

uint32_t fs_create_file(const char *path, int64_t size) {
    return size < 0 ? FS_OUT_OF_RANGE : mutate_path(path, M3_SAVE_FILE, size, 1, 0, 0);
}

uint32_t fs_create_file_opt(const char *path, int64_t size, unsigned option) {
    // A big file does not need a FAT concatenation attribute in a logical store.
    if (option & ~1u) return FS_FAILURE;
    return fs_create_file(path, size);
}

uint32_t fs_create_dir(const char *path) { return mutate_path(path, M3_SAVE_DIR, 0, 1, 0, 0); }
uint32_t fs_delete_file(const char *path) { return mutate_path(path, M3_SAVE_FILE, 0, 0, 0, 0); }
uint32_t fs_delete_dir(const char *path) { return mutate_path(path, M3_SAVE_DIR, 0, 0, 0, 0); }
uint32_t fs_delete_tree(const char *path) { return mutate_path(path, M3_SAVE_DIR, 0, 0, 1, 0); }
uint32_t fs_clean_tree(const char *path) { return mutate_path(path, M3_SAVE_DIR, 0, 0, 1, 1); }

static uint32_t rename_path(const char *from_guest, const char *to_guest, m3_save_type type) {
    char from[FS_PATH_CAPACITY], to[FS_PATH_CAPACITY];
    if (!m3_fs_copy_path(from_guest, from, sizeof(from)) || !m3_fs_copy_path(to_guest, to, sizeof(to)))
        return FS_FAILURE;
    AcquireSRWLockExclusive(&m3_fs_lock);
    const char *src = NULL, *dst = NULL;
    const struct fs_save_mount *a = m3_fs_save_path_locked(from, &src);
    const struct fs_save_mount *b = m3_fs_save_path_locked(to, &dst);
    uint32_t result = FS_NOT_MOUNTED;
    if (src && dst) {
        size_t an = (size_t)(src - from - 1), bn = (size_t)(dst - to - 1);
        int ar = !a && m3_fs_rom_mounted_locked(from, an);
        int br = !b && m3_fs_rom_mounted_locked(to, bn);
        if ((a || ar) && (b || br)) {
            if (a != b) result = FS_RENAME_OTHER;
            else if (a) result = m3_fs_save_result(m3_save_rename(a->store, src, dst, type));
            else result = FS_ROM_READ_ONLY;
        }
    }
    ReleaseSRWLockExclusive(&m3_fs_lock);
    return result;
}

uint32_t fs_rename_file(const char *from, const char *to) { return rename_path(from, to, M3_SAVE_FILE); }
uint32_t fs_rename_dir(const char *from, const char *to) { return rename_path(from, to, M3_SAVE_DIR); }
