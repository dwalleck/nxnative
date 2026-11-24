#include "m3_save_store.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsqlite/winsqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// The tree and the backing blobs have separate identities: rename updates only
// the node, and deleting/recreating a path never reuses an open file's node ID.
// Fixed-size blobs let incremental I/O grow without copying the rest of the
// file. Absent blocks read as zero, including arbitrarily large sparse holes.
enum { ROOT_ID = 1, BLOCK_SIZE = 65536, HOST_PATH_CAP = 32768, SUFFIX_CAP = 80 };
static const char schema[] =
    "BEGIN IMMEDIATE;"
    "PRAGMA application_id=0x4e585356; PRAGMA user_version=1;" // NXSV
    "CREATE TABLE node("
    " id INTEGER PRIMARY KEY AUTOINCREMENT,"
    " parent INTEGER REFERENCES node(id) ON DELETE CASCADE,"
    " name BLOB NOT NULL, kind INTEGER NOT NULL CHECK(kind IN(0,1)),"
    " size INTEGER NOT NULL CHECK(size>=0 AND (kind=1 OR size=0)),"
    " UNIQUE(parent,name));"
    "CREATE TABLE block("
    " id INTEGER PRIMARY KEY, node INTEGER NOT NULL REFERENCES node(id) ON DELETE CASCADE,"
    " n INTEGER NOT NULL, data BLOB NOT NULL, UNIQUE(node,n));"
    "INSERT INTO node(id,parent,name,kind,size) VALUES(1,NULL,x'',0,0); COMMIT;";

#define SQL_STATEMENTS(X) \
    X(CHILD,       "SELECT id,kind,size FROM node WHERE parent=?1 AND name=?2") \
    X(NODE,        "SELECT id,kind,size FROM node WHERE id=?1") \
    X(INSERT,      "INSERT INTO node(parent,name,kind,size) VALUES(?1,?2,?3,?4)") \
    X(RENAME,      "UPDATE node SET parent=?1,name=?2 WHERE id=?3") \
    X(HAS_CHILD,   "SELECT 1 FROM node WHERE parent=?1 LIMIT 1") \
    X(DELETE,      "DELETE FROM node WHERE id=?1") \
    X(CLEAR,       "DELETE FROM node WHERE parent=?1") \
    X(DEPTH,       "WITH RECURSIVE tree(id,len) AS (VALUES(?1,0) UNION ALL" \
                   " SELECT node.id,tree.len+length(node.name)+1 FROM node JOIN tree ON node.parent=tree.id)" \
                   " SELECT 1 FROM tree WHERE len>?2 LIMIT 1") \
    X(LIST,        "SELECT name,kind,size FROM node WHERE parent=?1 AND ((1 << kind) & ?2)!=0 ORDER BY name") \
    X(SIZE,        "UPDATE node SET size=?2 WHERE id=?1") \
    X(BLOCK_ID,    "SELECT id FROM block WHERE node=?1 AND n=?2") \
    X(BLOCK_NEW,   "INSERT INTO block(node,n,data) VALUES(?1,?2,zeroblob(?3))") \
    X(BLOCK_PRUNE, "DELETE FROM block WHERE node=?1 AND n>=?2")
#define SQL_ENUM(name, sql) ST_##name,
enum { SQL_STATEMENTS(SQL_ENUM) ST_COUNT };
#undef SQL_ENUM
#define SQL_TEXT(name, sql) sql,
static const char *const statements[] = { SQL_STATEMENTS(SQL_TEXT) };
#undef SQL_TEXT
#undef SQL_STATEMENTS

typedef struct { sqlite3_int64 id; m3_save_type type; int64_t size; } node_info;
typedef struct {
    uint32_t generation, next_free;
    int is_dir, mode;
    union {
        sqlite3_int64 node;
        struct { m3_save_entry *entries; size_t count, pos; } dir;
    } state;
} save_slot;
struct m3_save_store {
    SRWLOCK lock;
    sqlite3 *db;
    sqlite3_stmt *stmt[ST_COUNT];
    save_slot *slots;
    uint32_t slots_used, slots_cap, first_free;
    int active, poisoned;
};
static volatile LONG64 next_handle_generation;
static volatile LONG64 next_init_file;

static int exec_sql(sqlite3 *db, const char *sql) {
    return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
}

static int separator(wchar_t c) { return c == L'/' || c == L'\\'; }

static int absolute_drive(const wchar_t *p) {
    return ((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) &&
           p[1] == L':' && separator(p[2]);
}

static int unc_share(const wchar_t *p) {
    const wchar_t *server = p;
    while (*p && !separator(*p)) ++p;
    return p != server && separator(*p) && p[1] && !separator(p[1]);
}

static int absolute_host_path(const wchar_t *p) {
    if (absolute_drive(p)) return 1;
    if (!separator(p[0]) || !separator(p[1])) return 0;
    if (p[2] == L'.') return 0; // Not the Win32 device namespace.
    if (p[2] != L'?') return unc_share(p + 2);
    if (!separator(p[3])) return 0;
    p += 4;
    return absolute_drive(p) || (_wcsnicmp(p, L"UNC\\", 4) == 0 && unc_share(p + 4));
}

static m3_save_result host_error(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? M3_SAVE_NOT_FOUND : M3_SAVE_IO;
}

static m3_save_result save_directory(const char *directory, wchar_t **out, size_t *length) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, directory, -1, NULL, 0);
    if (n < 4 || n > HOST_PATH_CAP) return M3_SAVE_INVALID;
    wchar_t *wide = malloc((size_t)n * sizeof(*wide));
    if (!wide) return M3_SAVE_IO;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, directory, -1, wide, n) ||
        !absolute_host_path(wide)) {
        free(wide);
        return M3_SAVE_INVALID;
    }
    // Win32 directory handles do not in general accept a trailing separator.
    // Retain it for drive roots, including the verbatim spelling.
    size_t minimum = absolute_drive(wide) ? 3u :
                     (n >= 8 && wide[2] == L'?' && absolute_drive(wide + 4)) ? 7u : 2u;
    for (int end = n - 1; end > (int)minimum && separator(wide[end - 1]); --end)
        wide[end - 1] = L'\0';
    HANDLE dir = CreateFileW(wide, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    DWORD error = dir == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    free(wide);
    if (dir == INVALID_HANDLE_VALUE) return host_error(error);
    BY_HANDLE_FILE_INFORMATION info;
    DWORD cap = 0;
    if (GetFileInformationByHandle(dir, &info) &&
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == FILE_ATTRIBUTE_DIRECTORY)
        cap = GetFinalPathNameByHandleW(dir, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!cap || cap > HOST_PATH_CAP - SUFFIX_CAP) {
        CloseHandle(dir);
        return M3_SAVE_INVALID;
    }
    wide = malloc(((size_t)cap + SUFFIX_CAP) * sizeof(*wide));
    DWORD got = wide ? GetFinalPathNameByHandleW(dir, wide, cap, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS) : 0;
    CloseHandle(dir);
    if (!got || got >= cap) {
        free(wide);
        return M3_SAVE_IO;
    }
    size_t len = got;
    if (!separator(wide[len - 1])) wide[len++] = L'\\';
    *out = wide;
    *length = len;
    return M3_SAVE_OK;
}

// Treat a same-named foreign file (including hard links and reparse points) as
// a conflict, not as an empty save. Inspect the ownership marker before SQLite
// can even perform hot-journal recovery.
static m3_save_result peek_file(const wchar_t *path, unsigned char *buf, DWORD cap, DWORD *got) {
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (f == INVALID_HANDLE_VALUE) return host_error(GetLastError());
    BY_HANDLE_FILE_INFORMATION info;
    int ok = GetFileInformationByHandle(f, &info) && info.nNumberOfLinks == 1 &&
             !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
             ReadFile(f, buf, cap, got, NULL);
    CloseHandle(f);
    return ok ? M3_SAVE_OK : M3_SAVE_IO;
}

static int no_sidecar(wchar_t *suffix, const wchar_t *extension, const wchar_t *path) {
    wcscpy_s(suffix, 9, extension);
    DWORD attrs = GetFileAttributesW(path);
    DWORD error = attrs == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    *suffix = 0;
    return error == ERROR_FILE_NOT_FOUND;
}

static int sidecars_safe(wchar_t *path, int owns_database) {
    wchar_t *suffix = path + wcslen(path);
    if (!no_sidecar(suffix, L"-wal", path) || !no_sidecar(suffix, L"-shm", path)) return 0;
    wcscpy_s(suffix, 9, L"-journal");
    unsigned char head[8] = {0};
    DWORD got;
    m3_save_result r = peek_file(path, head, sizeof head, &got);
    *suffix = 0;
    if (r == M3_SAVE_NOT_FOUND) return 1;
    if (!owns_database || r != M3_SAVE_OK) return 0;
    // A crash before the rollback journal sync can leave an empty/zero header.
    static const unsigned char magic[] = { 0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7 };
    static const unsigned char zeros[8] = {0};
    return !memcmp(head, zeros, sizeof head) || (got == sizeof magic && !memcmp(head, magic, sizeof magic));
}

static int open_database(sqlite3 **db, const wchar_t *path) {
    char *utf8 = NULL;
    if (path) {
        int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, NULL, 0, NULL, NULL);
        if (!n) return 0;
        utf8 = malloc((size_t)n);
        if (!utf8) return 0;
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1, utf8, n, NULL, NULL)) {
            free(utf8);
            return 0;
        }
    }
    int rc = sqlite3_open_v2(path ? utf8 : ":memory:", db, path ? SQLITE_OPEN_READWRITE :
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_MEMORY, NULL);
    free(utf8);
    return rc == SQLITE_OK && sqlite3_db_readonly(*db, "main") == 0 &&
           exec_sql(*db, "PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL; PRAGMA temp_store=MEMORY;");
}

// Initialize at an exclusively created sibling, then publish without replacing
// any file. A crash during first-use initialization cannot strand an empty file
// at the account's actual database name.
static int create_database(const wchar_t *path, size_t dir_len) {
    wchar_t *temp = malloc((dir_len + SUFFIX_CAP) * sizeof(*temp));
    if (!temp) return 0;
    memcpy(temp, path, dir_len * sizeof(*temp));
    HANDLE file;
    for (;;) {
        unsigned long long n = (unsigned long long)InterlockedIncrement64(&next_init_file);
        if (swprintf(temp + dir_len, SUFFIX_CAP, L"nxnative-init-%08lx-%016llx.sqlite3",
                     (unsigned long)GetCurrentProcessId(), n) < 0) {
            free(temp);
            return 0;
        }
        if (!sidecars_safe(temp, 0)) {
            free(temp);
            return 0;
        }
        file = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) {
            free(temp);
            return 0;
        }
    }
    CloseHandle(file);
    sqlite3 *db = NULL;
    int ok = open_database(&db, temp) && exec_sql(db, "PRAGMA journal_mode=DELETE;") && exec_sql(db, schema);
    if (db && sqlite3_close(db) != SQLITE_OK) ok = 0;
    if (ok) ok = MoveFileExW(temp, path, MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(temp);
    free(temp);
    return ok;
}

static m3_save_result open_persistent(sqlite3 **db, const char *directory, uint64_t lo, uint64_t hi) {
    wchar_t *path;
    size_t len;
    m3_save_result r = save_directory(directory, &path, &len);
    if (r != M3_SAVE_OK) return r;
    if (swprintf(path + len, SUFFIX_CAP, L"nxnative-save-%016llx%016llx.sqlite3",
                 (unsigned long long)hi, (unsigned long long)lo) < 0) {
        free(path);
        return M3_SAVE_IO;
    }
    unsigned char head[72];
    DWORD got;
    r = peek_file(path, head, sizeof head, &got);
    if (r == M3_SAVE_NOT_FOUND && sidecars_safe(path, 0)) {
        if (create_database(path, len)) r = peek_file(path, head, sizeof head, &got);
    }
    int ok = r == M3_SAVE_OK && got == sizeof head && !memcmp(head, "SQLite format 3", 16) &&
             head[18] == 1 && head[19] == 1 && !memcmp(head + 60, "\0\0\0\1", 4) &&
             !memcmp(head + 68, "NXSV", 4) && sidecars_safe(path, 1) && open_database(db, path);
    free(path);
    return ok ? M3_SAVE_OK : M3_SAVE_IO;
}

static sqlite3_stmt *statement(m3_save_store *s, unsigned query) {
    sqlite3_stmt **p = &s->stmt[query];
    if (!*p && sqlite3_prepare_v2(s->db, statements[query], -1, p, NULL) != SQLITE_OK) return NULL;
    return *p;
}

static void reset_statement(sqlite3_stmt *p) {
    sqlite3_reset(p);
    sqlite3_clear_bindings(p);
}

static m3_save_result execute_statement(sqlite3_stmt *p) {
    int rc = sqlite3_step(p);
    reset_statement(p);
    return rc == SQLITE_DONE ? M3_SAVE_OK : M3_SAVE_IO;
}

static void clear_handles(m3_save_store *s) {
    for (uint32_t i = 0; i < s->slots_used; ++i)
        if (s->slots[i].generation && s->slots[i].is_dir) free(s->slots[i].state.dir.entries);
    s->slots_used = s->first_free = 0;
}

void m3_save_unmount(m3_save_store *s) {
    if (!s) return;
    clear_handles(s);
    free(s->slots);
    for (unsigned i = 0; i < ST_COUNT; ++i) sqlite3_finalize(s->stmt[i]);
    if (s->db) sqlite3_close(s->db); // No outstanding statements or blobs: SQLite rolls back on close.
    free(s);
}

static int has_volume_root(sqlite3 *db) {
    sqlite3_stmt *p = NULL;
    int ok = sqlite3_prepare_v2(db, "SELECT 1 FROM node WHERE id=1 AND parent IS NULL"
                                   " AND name=x'' AND kind=0 AND size=0", -1, &p, NULL) == SQLITE_OK &&
             sqlite3_step(p) == SQLITE_ROW;
    sqlite3_finalize(p);
    return ok;
}

m3_save_result m3_save_mount(m3_save_store **out, const char *directory, uint64_t lo, uint64_t hi) {
    if (!out) return M3_SAVE_INVALID;
    *out = NULL;
    m3_save_store *s = calloc(1, sizeof(*s));
    if (!s) return M3_SAVE_IO;
    InitializeSRWLock(&s->lock);
    m3_save_result r = directory ? open_persistent(&s->db, directory, lo, hi) :
        (open_database(&s->db, NULL) && exec_sql(s->db, schema) ? M3_SAVE_OK : M3_SAVE_IO);
    if (r == M3_SAVE_OK) {
        if (exec_sql(s->db, "BEGIN IMMEDIATE") && has_volume_root(s->db)) s->active = 1;
        else r = M3_SAVE_IO;
    }
    if (r != M3_SAVE_OK) {
        m3_save_unmount(s);
        return r;
    }
    *out = s;
    return M3_SAVE_OK;
}

static m3_save_result rollback_locked(m3_save_store *s) {
    clear_handles(s);
    s->active = 0;
    s->poisoned = !sqlite3_get_autocommit(s->db) && !exec_sql(s->db, "ROLLBACK");
    return s->poisoned ? M3_SAVE_IO : M3_SAVE_OK;
}

m3_save_result m3_save_rollback(m3_save_store *s) {
    if (!s) return M3_SAVE_INVALID;
    AcquireSRWLockExclusive(&s->lock);
    m3_save_result r = rollback_locked(s);
    ReleaseSRWLockExclusive(&s->lock);
    return r;
}

m3_save_result m3_save_commit(m3_save_store *s) {
    if (!s) return M3_SAVE_INVALID;
    AcquireSRWLockExclusive(&s->lock);
    m3_save_result r = M3_SAVE_OK;
    if (s->poisoned || (s->active && (sqlite3_get_autocommit(s->db) || !exec_sql(s->db, "COMMIT")))) {
        rollback_locked(s);
        r = M3_SAVE_IO;
    }
    s->active = 0;
    ReleaseSRWLockExclusive(&s->lock);
    return r;
}

// BEGIN IMMEDIATE excludes another writer for this account. Start lazily after
// Commit so an unrelated later lock failure cannot misreport a completed commit.
static int begin_locked(m3_save_store *s) {
    if (s->poisoned) return 0;
    if (s->active) return !sqlite3_get_autocommit(s->db);
    s->active = exec_sql(s->db, "BEGIN IMMEDIATE");
    return s->active;
}

static m3_save_result end_locked(m3_save_store *s, m3_save_result r, int mutation) {
    // SQLite may have rolled back the whole transaction itself. In either case
    // a failed mutation must not allow an unchecked later Commit to publish it.
    if (r == M3_SAVE_IO && (mutation || (s->active && sqlite3_get_autocommit(s->db)))) s->poisoned = 1;
    ReleaseSRWLockExclusive(&s->lock);
    return r;
}

static m3_save_result normalize(const char *path, char out[M3_SAVE_PATH_MAX + 1]) {
    if (!path || (*path != '/' && *path != '\\')) return M3_SAVE_INVALID;
    size_t raw = 0, n = 0;
    while (raw <= M3_SAVE_PATH_MAX && path[raw]) ++raw;
    if (raw > M3_SAVE_PATH_MAX) return M3_SAVE_INVALID;
    const char *end = path + raw;
    while (path != end) {
        if (*path == '/' || *path == '\\') { ++path; continue; }
        const char *first = path;
        while (path != end && *path != '/' && *path != '\\') ++path;
        size_t len = (size_t)(path - first);
        if (len == 1 && first[0] == '.') continue;
        if (len == 2 && first[0] == '.' && first[1] == '.') {
            if (!n) return M3_SAVE_NOT_FOUND;
            while (out[--n] != '/') {}
            continue;
        }
        out[n++] = '/';
        memcpy(out + n, first, len);
        n += len;
    }
    if (!n) out[n++] = '/';
    out[n] = 0;
    return M3_SAVE_OK;
}

static m3_save_result step_node(sqlite3_stmt *p, node_info *out) {
    int rc = sqlite3_step(p);
    if (rc == SQLITE_ROW) {
        out->id = sqlite3_column_int64(p, 0);
        out->type = (m3_save_type)sqlite3_column_int(p, 1);
        out->size = sqlite3_column_int64(p, 2);
    }
    reset_statement(p);
    return rc == SQLITE_ROW ? M3_SAVE_OK : rc == SQLITE_DONE ? M3_SAVE_NOT_FOUND : M3_SAVE_IO;
}

static m3_save_result find_path(m3_save_store *s, const char *path, size_t len, node_info *out) {
    *out = (node_info){ ROOT_ID, M3_SAVE_DIR, 0 };
    const char *end = path + len;
    for (const char *first = path + 1; first < end;) {
        if (out->type != M3_SAVE_DIR) return M3_SAVE_NOT_FOUND;
        const char *next = memchr(first, '/', (size_t)(end - first));
        if (!next) next = end;
        sqlite3_stmt *p = statement(s, ST_CHILD);
        if (!p) return M3_SAVE_IO;
        sqlite3_bind_int64(p, 1, out->id);
        if (sqlite3_bind_blob(p, 2, first, (int)(next - first), SQLITE_STATIC) != SQLITE_OK) {
            reset_statement(p);
            return M3_SAVE_IO;
        }
        m3_save_result r = step_node(p, out);
        if (r != M3_SAVE_OK) return r;
        if (next == end) break;
        first = next + 1;
    }
    return M3_SAVE_OK;
}

static m3_save_result find_parent(m3_save_store *s, const char *path, node_info *parent, const char **name) {
    const char *slash = strrchr(path, '/');
    *name = slash + 1;
    if (!**name) return M3_SAVE_EXISTS;
    size_t len = (size_t)(slash - path);
    m3_save_result r = find_path(s, path, len ? len : 1, parent);
    return r == M3_SAVE_OK && parent->type != M3_SAVE_DIR ? M3_SAVE_NOT_FOUND : r;
}

m3_save_result m3_save_type_at(m3_save_store *s, const char *path, m3_save_type *out) {
    if (!s || !out) return M3_SAVE_INVALID;
    char norm[M3_SAVE_PATH_MAX + 1];
    m3_save_result r = normalize(path, norm);
    if (r != M3_SAVE_OK) return r;
    AcquireSRWLockExclusive(&s->lock);
    node_info node = {0};
    r = begin_locked(s) ? find_path(s, norm, strlen(norm), &node) : M3_SAVE_IO;
    if (r == M3_SAVE_OK) *out = node.type;
    return end_locked(s, r, 0);
}

static m3_save_result insert_or_move(m3_save_store *s, const char *path, m3_save_type type,
                                     int64_t size, sqlite3_int64 rename_id) {
    node_info parent;
    const char *name;
    m3_save_result r = find_parent(s, path, &parent, &name);
    if (r != M3_SAVE_OK) return r;
    sqlite3_stmt *p = statement(s, rename_id ? ST_RENAME : ST_INSERT);
    if (!p) return M3_SAVE_IO;
    sqlite3_bind_int64(p, 1, parent.id);
    if (sqlite3_bind_blob(p, 2, name, (int)strlen(name), SQLITE_STATIC) != SQLITE_OK) {
        reset_statement(p);
        return M3_SAVE_IO;
    }
    if (rename_id) sqlite3_bind_int64(p, 3, rename_id);
    else {
        sqlite3_bind_int(p, 3, type);
        sqlite3_bind_int64(p, 4, size);
    }
    int rc = sqlite3_step(p);
    int ext = rc == SQLITE_CONSTRAINT ? sqlite3_extended_errcode(s->db) : rc;
    reset_statement(p);
    return rc == SQLITE_DONE ? M3_SAVE_OK : ext == SQLITE_CONSTRAINT_UNIQUE ? M3_SAVE_EXISTS : M3_SAVE_IO;
}

m3_save_result m3_save_create(m3_save_store *s, const char *path, m3_save_type type, int64_t size) {
    if (!s || size < 0 || (type != M3_SAVE_FILE && (type != M3_SAVE_DIR || size))) return M3_SAVE_INVALID;
    char norm[M3_SAVE_PATH_MAX + 1];
    m3_save_result r = normalize(path, norm);
    if (r != M3_SAVE_OK) return r;
    AcquireSRWLockExclusive(&s->lock);
    r = begin_locked(s) ? insert_or_move(s, norm, type, size, 0) : M3_SAVE_IO;
    return end_locked(s, r, 1);
}

static m3_save_result remove_locked(m3_save_store *s, const char *path, m3_save_type type,
                                    int recursive, int keep_dir) {
    node_info node;
    m3_save_result r = find_path(s, path, strlen(path), &node);
    if (r != M3_SAVE_OK) return r;
    if (node.type != type) return type == M3_SAVE_DIR ? M3_SAVE_NOT_FOUND : M3_SAVE_INVALID;
    if (node.id == ROOT_ID && !keep_dir) return M3_SAVE_INVALID;
    if (type == M3_SAVE_DIR && !recursive) {
        sqlite3_stmt *p = statement(s, ST_HAS_CHILD);
        if (!p) return M3_SAVE_IO;
        sqlite3_bind_int64(p, 1, node.id);
        int rc = sqlite3_step(p);
        reset_statement(p);
        if (rc != SQLITE_DONE) return rc == SQLITE_ROW ? M3_SAVE_NOT_EMPTY : M3_SAVE_IO;
    }
    sqlite3_stmt *p = statement(s, keep_dir ? ST_CLEAR : ST_DELETE);
    if (!p) return M3_SAVE_IO;
    sqlite3_bind_int64(p, 1, node.id);
    return execute_statement(p);
}

m3_save_result m3_save_remove(m3_save_store *s, const char *path, m3_save_type type, int recursive, int keep_dir) {
    if (!s || (type != M3_SAVE_DIR && (type != M3_SAVE_FILE || recursive || keep_dir)) ||
        (keep_dir && !recursive)) return M3_SAVE_INVALID;
    char norm[M3_SAVE_PATH_MAX + 1];
    m3_save_result r = normalize(path, norm);
    if (r != M3_SAVE_OK) return r;
    AcquireSRWLockExclusive(&s->lock);
    r = begin_locked(s) ? remove_locked(s, norm, type, recursive, keep_dir) : M3_SAVE_IO;
    return end_locked(s, r, 1);
}

static m3_save_result rename_locked(m3_save_store *s, const char *from, const char *to, m3_save_type type) {
    size_t from_len = strlen(from), to_len = strlen(to);
    node_info node;
    m3_save_result r = find_path(s, from, from_len, &node);
    if (r != M3_SAVE_OK) return r;
    if (node.type != type) return M3_SAVE_NOT_FOUND;
    if (node.id == ROOT_ID) return M3_SAVE_INVALID;
    if (from_len == to_len && !memcmp(from, to, from_len)) return M3_SAVE_EXISTS;
    if (type == M3_SAVE_DIR && to_len > from_len) {
        if (to[from_len] == '/' && !memcmp(from, to, from_len)) return M3_SAVE_INVALID;
        // Every descendant must remain addressable (also bounds FK cascade depth).
        sqlite3_stmt *p = statement(s, ST_DEPTH);
        if (!p) return M3_SAVE_IO;
        sqlite3_bind_int64(p, 1, node.id);
        sqlite3_bind_int64(p, 2, (sqlite3_int64)(M3_SAVE_PATH_MAX - to_len));
        int rc = sqlite3_step(p);
        reset_statement(p);
        if (rc != SQLITE_DONE) return rc == SQLITE_ROW ? M3_SAVE_INVALID : M3_SAVE_IO;
    }
    return insert_or_move(s, to, type, 0, node.id);
}

m3_save_result m3_save_rename(m3_save_store *s, const char *from, const char *to, m3_save_type type) {
    if (!s || (type != M3_SAVE_DIR && type != M3_SAVE_FILE)) return M3_SAVE_INVALID;
    char src[M3_SAVE_PATH_MAX + 1], dst[M3_SAVE_PATH_MAX + 1];
    m3_save_result r = normalize(from, src);
    if (r == M3_SAVE_OK) r = normalize(to, dst);
    if (r != M3_SAVE_OK) return r;
    AcquireSRWLockExclusive(&s->lock);
    r = begin_locked(s) ? rename_locked(s, src, dst, type) : M3_SAVE_IO;
    return end_locked(s, r, 1);
}

static save_slot *get_slot(m3_save_store *s, m3_save_handle h, int is_dir) {
    uint32_t index = (uint32_t)h;
    if (!index || index > s->slots_used) return NULL;
    save_slot *slot = &s->slots[index - 1];
    return slot->generation && slot->generation == (uint32_t)(h >> 32) && slot->is_dir == is_dir ? slot : NULL;
}

static save_slot *new_slot(m3_save_store *s, m3_save_handle *out, int is_dir, int mode) {
    LONG64 generation = InterlockedIncrement64(&next_handle_generation);
    if (generation > (LONG64)UINT32_MAX) return NULL;
    if (!s->first_free && s->slots_used == s->slots_cap) {
        if (s->slots_cap > UINT32_MAX / 2) return NULL;
        uint32_t cap = s->slots_cap ? s->slots_cap * 2 : 16;
        save_slot *slots = realloc(s->slots, (size_t)cap * sizeof(*slots));
        if (!slots) return NULL;
        s->slots = slots;
        s->slots_cap = cap;
    }
    uint32_t i = s->first_free ? s->first_free : ++s->slots_used;
    save_slot *slot = &s->slots[i - 1];
    if (s->first_free) s->first_free = slot->next_free;
    slot->generation = (uint32_t)generation;
    slot->is_dir = is_dir;
    slot->mode = mode;
    *out = ((uint64_t)slot->generation << 32) | i;
    return slot;
}

static m3_save_result close_handle(m3_save_store *s, m3_save_handle h, int is_dir) {
    if (!s) return M3_SAVE_INVALID;
    AcquireSRWLockExclusive(&s->lock);
    save_slot *slot = get_slot(s, h, is_dir);
    if (slot) {
        if (is_dir) free(slot->state.dir.entries);
        slot->generation = 0;
        slot->next_free = s->first_free;
        s->first_free = (uint32_t)h;
    }
    return end_locked(s, slot ? M3_SAVE_OK : M3_SAVE_INVALID, 0);
}

m3_save_result m3_save_open_file(m3_save_store *s, const char *path, int mode, m3_save_handle *out) {
    if (!out) return M3_SAVE_INVALID;
    *out = 0;
    if (!s || !(mode & (M3_SAVE_READ | M3_SAVE_WRITE)) ||
        (mode & ~(M3_SAVE_READ | M3_SAVE_WRITE | M3_SAVE_APPEND)) ||
        ((mode & M3_SAVE_APPEND) && !(mode & M3_SAVE_WRITE))) return M3_SAVE_INVALID;
    char norm[M3_SAVE_PATH_MAX + 1];
    m3_save_result r = normalize(path, norm);
    if (r != M3_SAVE_OK) return r;
    AcquireSRWLockExclusive(&s->lock);
    node_info node = {0};
    r = begin_locked(s) ? find_path(s, norm, strlen(norm), &node) : M3_SAVE_IO;
    if (r == M3_SAVE_OK && node.type != M3_SAVE_FILE) r = M3_SAVE_NOT_FOUND;
    if (r == M3_SAVE_OK) {
        save_slot *slot = new_slot(s, out, 0, mode);
        if (slot) slot->state.node = node.id;
        else r = M3_SAVE_IO;
    }
    return end_locked(s, r, 0);
}

m3_save_result m3_save_close_file(m3_save_store *s, m3_save_handle h) { return close_handle(s, h, 0); }

static m3_save_result open_node(m3_save_store *s, const save_slot *slot, node_info *out) {
    if (!begin_locked(s)) return M3_SAVE_IO;
    sqlite3_stmt *p = statement(s, ST_NODE);
    if (!p) return M3_SAVE_IO;
    sqlite3_bind_int64(p, 1, slot->state.node);
    m3_save_result r = step_node(p, out);
    return r == M3_SAVE_OK && out->type != M3_SAVE_FILE ? M3_SAVE_INVALID : r;
}

m3_save_result m3_save_file_mode(m3_save_store *s, m3_save_handle h, int *out) {
    if (!s || !out) return M3_SAVE_INVALID;
    AcquireSRWLockExclusive(&s->lock);
    const save_slot *slot = get_slot(s, h, 0);
    if (slot) *out = slot->mode;
    return end_locked(s, slot ? M3_SAVE_OK : M3_SAVE_INVALID, 0);
}

m3_save_result m3_save_file_size(m3_save_store *s, m3_save_handle h, int64_t *out) {
    if (!s || !out) return M3_SAVE_INVALID;
    AcquireSRWLockExclusive(&s->lock);
    const save_slot *slot = get_slot(s, h, 0);
    node_info node = {0};
    m3_save_result r = slot ? open_node(s, slot, &node) : M3_SAVE_INVALID;
    if (r == M3_SAVE_OK) *out = node.size;
    return end_locked(s, r, 0);
}

static m3_save_result block_id(m3_save_store *s, sqlite3_int64 node, int64_t block, sqlite3_int64 *out) {
    sqlite3_stmt *p = statement(s, ST_BLOCK_ID);
    if (!p) return M3_SAVE_IO;
    sqlite3_bind_int64(p, 1, node);
    sqlite3_bind_int64(p, 2, block);
    int rc = sqlite3_step(p);
    if (rc == SQLITE_ROW) *out = sqlite3_column_int64(p, 0);
    reset_statement(p);
    return rc == SQLITE_ROW ? M3_SAVE_OK : rc == SQLITE_DONE ? M3_SAVE_NOT_FOUND : M3_SAVE_IO;
}

static m3_save_result blob_io(m3_save_store *s, sqlite3_int64 id, void *output, const void *input, int off, int n) {
    sqlite3_blob *blob;
    if (sqlite3_blob_open(s->db, "main", "block", "data", id, input != NULL, &blob) != SQLITE_OK)
        return M3_SAVE_IO;
    int rc = sqlite3_blob_bytes(blob) != BLOCK_SIZE ? SQLITE_CORRUPT : input ?
             sqlite3_blob_write(blob, input, n, off) : sqlite3_blob_read(blob, output, n, off);
    int closed = sqlite3_blob_close(blob);
    return rc == SQLITE_OK && closed == SQLITE_OK ? M3_SAVE_OK : M3_SAVE_IO;
}

static m3_save_result transfer(m3_save_store *s, sqlite3_int64 node, int64_t off,
                               void *output, const void *input, size_t len) {
    while (len) {
        int64_t block = off / BLOCK_SIZE;
        int within = (int)(off % BLOCK_SIZE);
        int n = BLOCK_SIZE - within;
        if (len < (size_t)n) n = (int)len;
        sqlite3_int64 id;
        m3_save_result r = block_id(s, node, block, &id);
        if (r == M3_SAVE_NOT_FOUND && input) {
            sqlite3_stmt *p = statement(s, ST_BLOCK_NEW);
            if (!p) return M3_SAVE_IO;
            sqlite3_bind_int64(p, 1, node);
            sqlite3_bind_int64(p, 2, block);
            sqlite3_bind_int(p, 3, BLOCK_SIZE);
            r = execute_statement(p);
            if (r == M3_SAVE_OK) id = sqlite3_last_insert_rowid(s->db);
        }
        if (r == M3_SAVE_OK) r = blob_io(s, id, output, input, within, n);
        else if (r == M3_SAVE_NOT_FOUND) {
            memset(output, 0, (size_t)n);
            r = M3_SAVE_OK;
        }
        if (r != M3_SAVE_OK) return r;
        if (input) input = (const unsigned char *)input + n;
        else output = (unsigned char *)output + n;
        off += n;
        len -= (size_t)n;
    }
    return M3_SAVE_OK;
}

static m3_save_result update_size(m3_save_store *s, sqlite3_int64 id, int64_t size) {
    sqlite3_stmt *p = statement(s, ST_SIZE);
    if (!p) return M3_SAVE_IO;
    sqlite3_bind_int64(p, 1, id);
    sqlite3_bind_int64(p, 2, size);
    return execute_statement(p);
}

static m3_save_result resize(m3_save_store *s, const node_info *node, int64_t size) {
    if (size == node->size) return M3_SAVE_OK;
    if (size < node->size) {
        int64_t block = size / BLOCK_SIZE;
        int tail = (int)(size % BLOCK_SIZE);
        sqlite3_stmt *p = statement(s, ST_BLOCK_PRUNE);
        if (!p) return M3_SAVE_IO;
        sqlite3_bind_int64(p, 1, node->id);
        sqlite3_bind_int64(p, 2, block + (tail != 0));
        m3_save_result r = execute_statement(p);
        if (r != M3_SAVE_OK) return r;
        if (tail) {
            sqlite3_int64 id;
            r = block_id(s, node->id, block, &id);
            if (r == M3_SAVE_OK) {
                static const unsigned char zeros[BLOCK_SIZE] = {0};
                r = blob_io(s, id, NULL, zeros, tail, BLOCK_SIZE - tail);
            }
            if (r != M3_SAVE_OK && r != M3_SAVE_NOT_FOUND) return r;
        }
    }
    return update_size(s, node->id, size);
}

m3_save_result m3_save_set_size(m3_save_store *s, m3_save_handle h, int64_t size) {
    if (!s || size < 0) return M3_SAVE_INVALID;
    AcquireSRWLockExclusive(&s->lock);
    const save_slot *slot = get_slot(s, h, 0);
    node_info node;
    m3_save_result r = slot && (slot->mode & M3_SAVE_WRITE) ? open_node(s, slot, &node) : M3_SAVE_INVALID;
    if (r == M3_SAVE_OK) r = resize(s, &node, size);
    return end_locked(s, r, 1);
}

m3_save_result m3_save_flush(m3_save_store *s, m3_save_handle h) {
    if (!s) return M3_SAVE_INVALID;
    AcquireSRWLockExclusive(&s->lock);
    m3_save_result r = M3_SAVE_INVALID;
    if (get_slot(s, h, 0)) {
        r = M3_SAVE_OK;
        if (s->poisoned || (s->active && (sqlite3_get_autocommit(s->db) ||
                                         sqlite3_db_cacheflush(s->db) != SQLITE_OK))) r = M3_SAVE_IO;
    }
    return end_locked(s, r, 1);
}

static int valid_transfer(int64_t off, const void *buf, size_t len) {
    return off >= 0 && len <= (uint64_t)(INT64_MAX - off) && (buf || !len);
}

m3_save_result m3_save_read(m3_save_store *s, m3_save_handle h, int64_t off, void *buf, size_t len, size_t *got) {
    if (got) *got = 0;
    if (!s || !valid_transfer(off, buf, len)) return M3_SAVE_INVALID;
    AcquireSRWLockExclusive(&s->lock);
    const save_slot *slot = get_slot(s, h, 0);
    node_info node = {0};
    m3_save_result r = slot && (slot->mode & M3_SAVE_READ) ? open_node(s, slot, &node) : M3_SAVE_INVALID;
    if (r == M3_SAVE_OK) {
        uint64_t available = off < node.size ? (uint64_t)(node.size - off) : 0;
        size_t actual = available < len ? (size_t)available : len;
        if (len && (off > node.size || (!got && actual != len))) r = M3_SAVE_OUT_OF_RANGE;
        else {
            r = transfer(s, node.id, off, buf, NULL, actual);
            if (r == M3_SAVE_OK && got) *got = actual;
        }
    }
    return end_locked(s, r, 0);
}

m3_save_result m3_save_write(m3_save_store *s, m3_save_handle h, int64_t off, const void *buf, size_t len) {
    if (!s || !valid_transfer(off, buf, len)) return M3_SAVE_INVALID;
    AcquireSRWLockExclusive(&s->lock);
    const save_slot *slot = get_slot(s, h, 0);
    node_info node = {0};
    m3_save_result r = slot && (slot->mode & M3_SAVE_WRITE) ? open_node(s, slot, &node) : M3_SAVE_INVALID;
    if (r == M3_SAVE_OK && len) {
        int64_t end = off + (int64_t)len;
        if (end > node.size && !(slot->mode & M3_SAVE_APPEND)) r = M3_SAVE_APPEND_REQUIRED;
        else {
            r = transfer(s, node.id, off, NULL, buf, len);
            if (r == M3_SAVE_OK && end > node.size) r = update_size(s, node.id, end);
        }
    }
    return end_locked(s, r, 1);
}

static m3_save_result snapshot(m3_save_store *s, sqlite3_int64 id, int mode, m3_save_handle *out) {
    sqlite3_stmt *p = statement(s, ST_LIST);
    if (!p) return M3_SAVE_IO;
    sqlite3_bind_int64(p, 1, id);
    sqlite3_bind_int(p, 2, mode);
    m3_save_entry *entries = NULL;
    size_t count = 0, cap = 0;
    int rc;
    while ((rc = sqlite3_step(p)) == SQLITE_ROW) {
        const void *name = sqlite3_column_blob(p, 0);
        int n = sqlite3_column_bytes(p, 0);
        if (!name || n <= 0 || n > M3_SAVE_NAME_MAX) { rc = SQLITE_CORRUPT; break; }
        if (count == cap) {
            if (cap > SIZE_MAX / (2 * sizeof(*entries))) { rc = SQLITE_NOMEM; break; }
            cap = cap ? cap * 2 : 8;
            m3_save_entry *grown = realloc(entries, cap * sizeof(*entries));
            if (!grown) { rc = SQLITE_NOMEM; break; }
            entries = grown;
        }
        m3_save_entry *e = &entries[count++];
        memset(e, 0, sizeof(*e));
        memcpy(e->name, name, (size_t)n);
        e->type = (m3_save_type)sqlite3_column_int(p, 1);
        e->size = sqlite3_column_int64(p, 2);
    }
    reset_statement(p);
    save_slot *slot = rc == SQLITE_DONE ? new_slot(s, out, 1, mode) : NULL;
    if (!slot) { free(entries); return M3_SAVE_IO; }
    slot->state.dir.entries = entries;
    slot->state.dir.count = count;
    slot->state.dir.pos = 0;
    return M3_SAVE_OK;
}

m3_save_result m3_save_open_dir(m3_save_store *s, const char *path, int mode, m3_save_handle *out) {
    if (!out) return M3_SAVE_INVALID;
    *out = 0;
    if (!s || !mode || (mode & ~(M3_SAVE_LIST_DIR | M3_SAVE_LIST_FILE))) return M3_SAVE_INVALID;
    char norm[M3_SAVE_PATH_MAX + 1];
    m3_save_result r = normalize(path, norm);
    if (r != M3_SAVE_OK) return r;
    AcquireSRWLockExclusive(&s->lock);
    node_info node = {0};
    r = begin_locked(s) ? find_path(s, norm, strlen(norm), &node) : M3_SAVE_IO;
    if (r == M3_SAVE_OK)
        r = node.type == M3_SAVE_DIR ? snapshot(s, node.id, mode, out) : M3_SAVE_NOT_FOUND;
    return end_locked(s, r, 0);
}

m3_save_result m3_save_close_dir(m3_save_store *s, m3_save_handle h) { return close_handle(s, h, 1); }

m3_save_result m3_save_dir_count(m3_save_store *s, m3_save_handle h, int64_t *out) {
    if (!s || !out) return M3_SAVE_INVALID;
    AcquireSRWLockExclusive(&s->lock);
    const save_slot *slot = get_slot(s, h, 1);
    if (slot) *out = (int64_t)slot->state.dir.count;
    return end_locked(s, slot ? M3_SAVE_OK : M3_SAVE_INVALID, 0);
}

m3_save_result m3_save_read_dir(m3_save_store *s, m3_save_handle h, m3_save_entry *entries,
                                int64_t max, int64_t *got) {
    if (!got) return M3_SAVE_INVALID;
    *got = 0;
    if (!s || max < 0 || (uint64_t)max > SIZE_MAX / sizeof(*entries) || (max && !entries)) return M3_SAVE_INVALID;
    AcquireSRWLockExclusive(&s->lock);
    save_slot *slot = get_slot(s, h, 1);
    if (slot) {
        size_t count = slot->state.dir.count - slot->state.dir.pos;
        if ((size_t)max < count) count = (size_t)max;
        if (count) memcpy(entries, slot->state.dir.entries + slot->state.dir.pos, count * sizeof(*entries));
        slot->state.dir.pos += count;
        *got = (int64_t)count;
    }
    return end_locked(s, slot ? M3_SAVE_OK : M3_SAVE_INVALID, 0);
}
