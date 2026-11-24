#ifndef M3_SAVE_STORE_H
#define M3_SAVE_STORE_H

#include <stddef.h>
#include <stdint.h>

// One mounted volume per store. Its operations are serialized, but callers must
// exclude destruction from concurrent calls. No guest ABI layouts are used here.
typedef struct m3_save_store m3_save_store;
typedef uint64_t m3_save_handle;
typedef enum {
    M3_SAVE_OK = 0,
    M3_SAVE_NOT_FOUND,
    M3_SAVE_EXISTS,
    M3_SAVE_INVALID,
    M3_SAVE_IO,
    M3_SAVE_OUT_OF_RANGE,
    M3_SAVE_APPEND_REQUIRED,
    M3_SAVE_NOT_EMPTY
} m3_save_result;
typedef enum { M3_SAVE_DIR = 0, M3_SAVE_FILE = 1 } m3_save_type;
enum {
    M3_SAVE_READ = 1, M3_SAVE_WRITE = 2, M3_SAVE_APPEND = 4,
    M3_SAVE_LIST_DIR = 1, M3_SAVE_LIST_FILE = 2,
    M3_SAVE_NAME_MAX = 0x300, M3_SAVE_PATH_MAX = 0x300
};
typedef struct {
    char name[M3_SAVE_NAME_MAX + 1];
    m3_save_type type;
    int64_t size;
} m3_save_entry;

// directory is the explicitly configured absolute UTF-8 DM_SAVE directory; it
// must already exist. uid_lo/uid_hi select one database. NULL mounts an in-memory
// volume with no host filesystem effects. Failed opens leave *out NULL.
m3_save_result m3_save_mount(m3_save_store **out, const char *directory,
                             uint64_t uid_lo, uint64_t uid_hi);
void m3_save_unmount(m3_save_store *s); // Rolls back changes since the last commit.
m3_save_result m3_save_commit(m3_save_store *s);
// Discards uncommitted changes and invalidates all handles from this store.
m3_save_result m3_save_rollback(m3_save_store *s);

// Paths start with '/', contain no mount prefix, and use byte-exact names. Both
// separator spellings are accepted, and . and .. normalize without escaping '/'.
m3_save_result m3_save_type_at(m3_save_store *s, const char *path, m3_save_type *out);
m3_save_result m3_save_create(m3_save_store *s, const char *path, m3_save_type type, int64_t size);
// recursive deletes the subtree; keep_dir instead keeps the specified directory.
m3_save_result m3_save_remove(m3_save_store *s, const char *path, m3_save_type type,
                              int recursive, int keep_dir);
m3_save_result m3_save_rename(m3_save_store *s, const char *from, const char *to, m3_save_type type);

m3_save_result m3_save_open_file(m3_save_store *s, const char *path, int mode, m3_save_handle *out);
m3_save_result m3_save_close_file(m3_save_store *s, m3_save_handle h);
// Spill staged dirty pages without publishing the transaction.
m3_save_result m3_save_flush(m3_save_store *s, m3_save_handle h);
m3_save_result m3_save_file_mode(m3_save_store *s, m3_save_handle h, int *out);
m3_save_result m3_save_file_size(m3_save_store *s, m3_save_handle h, int64_t *out);
m3_save_result m3_save_set_size(m3_save_store *s, m3_save_handle h, int64_t size);
// NULL got requires an exact read. A counted read at EOF succeeds with zero
// bytes; a nonempty read starting beyond EOF is out of range.
m3_save_result m3_save_read(m3_save_store *s, m3_save_handle h, int64_t offset,
                            void *buf, size_t size, size_t *got);
// WRITE permits overwrites; WRITE|APPEND also permits extending at the offset.
m3_save_result m3_save_write(m3_save_store *s, m3_save_handle h, int64_t offset,
                             const void *buf, size_t size);

// Opens a stable snapshot of immediate children, ordered by unsigned name bytes.
m3_save_result m3_save_open_dir(m3_save_store *s, const char *path, int mode, m3_save_handle *out);
m3_save_result m3_save_close_dir(m3_save_store *s, m3_save_handle h);
m3_save_result m3_save_dir_count(m3_save_store *s, m3_save_handle h, int64_t *out);
m3_save_result m3_save_read_dir(m3_save_store *s, m3_save_handle h, m3_save_entry *entries,
                                int64_t max, int64_t *got);

#endif
