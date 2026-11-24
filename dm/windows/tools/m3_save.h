#ifndef M3_SAVE_H
#define M3_SAVE_H

#include "m2_host.h"
#include "m3_save_store.h"

// Shared SDK filesystem ABI and mount-table lock. `*_locked` calls require it.
enum {
    FS_OK = 0,
    FS_NOT_FOUND = (1 << 9) | 2, FS_EXISTS = (2 << 9) | 2,
    FS_DIRECTORY_NOT_EMPTY = (8 << 9) | 2,
    FS_MOUNT_EXISTS = (60 << 9) | 2,
    FS_TARGET_NOT_FOUND = (1002 << 9) | 2,
    FS_OUT_OF_RANGE = (3005 << 9) | 2,
    FS_INVALID_ARGUMENT = (6001 << 9) | 2,
    FS_RENAME_OTHER = (6034 << 9) | 2,
    FS_INVALID_MOUNT = (6065 << 9) | 2, FS_INVALID_MODE = (6072 << 9) | 2,
    FS_APPEND_REQUIRED = (6201 << 9) | 2,
    FS_READ_NOT_PERMITTED = (6202 << 9) | 2, FS_WRITE_NOT_PERMITTED = (6203 << 9) | 2,
    FS_ROM_READ_ONLY = (6364 << 9) | 2,
    FS_WRITE_FILE_OPEN = (6457 << 9) | 2,
    FS_OPEN_LIMIT = (6709 << 9) | 2, FS_NOT_MOUNTED = (6905 << 9) | 2,
    FS_FAILURE = 0x2a2,
    FS_MOUNT_CAPACITY = 16, FS_PATH_CAPACITY = 0x400
};
#define FS_LIST_NO_SIZE UINT32_C(0x80000000)

struct fs_dir_entry {
    char name[0x301];
    uint8_t pad[3];
    int8_t type;
    uint8_t pad2[3];
    int64_t size;
};
_Static_assert(offsetof(struct fs_dir_entry, type) == 0x304 &&
               offsetof(struct fs_dir_entry, size) == 0x308 &&
               sizeof(struct fs_dir_entry) == 0x310, "SDK DirectoryEntry layout");

struct fs_save_mount {
    char name[FS_MOUNT_CAPACITY];
    m3_save_store *store;
};

extern SRWLOCK m3_fs_lock;
int m3_fs_copy_path(const char *guest, char *out, size_t capacity);
int m3_fs_rom_mounted_locked(const char *name, size_t length);
int64_t m3_fs_rom_find_locked(const char *path, const char *rest);
// With store set, test any alias of that store for a writable file at Commit;
// with mount set, test only that name for open files or directories at Unmount.
int m3_fs_save_busy_locked(const struct fs_save_mount *mount, const m3_save_store *store);
const struct fs_save_mount *m3_fs_save_mount_locked(const char *name, size_t length);
const struct fs_save_mount *m3_fs_save_path_locked(const char *path, const char **subpath);
void m3_fs_save_unmount_locked(const char *name);
uint32_t m3_fs_save_result(m3_save_result result);

#endif
