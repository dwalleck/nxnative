// Near-layout NSO/NRO images from patch_nso.py --target=windows-arm64.
#include "m2_host.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sym { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; };
struct dyn { int64_t tag; uint64_t value; };
struct rela { uint64_t offset, info; int64_t addend; };
static_assert(sizeof(struct sym) == 24 && sizeof(struct dyn) == 16 && sizeof(struct rela) == 24, "ELF64 layout");

static FILE *open_artifact(const char *dir, const char *name, const char *suffix, const char *mode) {
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s.winarm64.%s", dir, name, suffix);
    if (n < 0 || (size_t)n >= sizeof(path)) dm_fail("artifact path too long");
    FILE *f;
    if (fopen_s(&f, path, mode)) dm_fail("cannot open %s", path);
    return f;
}

static void *region(const struct dm_module *m, uint64_t off, size_t length, uint64_t begin, uint64_t end) {
    if (off < begin || off > end || length > end - off)
        dm_fail("%s: offset %#" PRIx64 " + %#zx outside module region", m->name, off, length);
    return m->base + off;
}

static void read_fixups(struct dm_module *m, FILE *f, uint32_t idx[5]) {
    char line[256], field[80], value[80];
    int win = 0;
    uint64_t key_size = 0;
    static const char *const keys[] = {"KEY_K18_IDX", "KEY_KTLS_IDX", "KEY_KTLR_IDX", "KEY_KGATE_IDX", "KEY_WIN_TLS_IDX"};
    for (unsigned i = 0; i < ARRAYSIZE(keys); ++i) idx[i] = UINT32_MAX;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf_s(line, "# %79s %79s", field, (unsigned)sizeof(field), value, (unsigned)sizeof(value)) != 2) continue;
        field[sizeof(field) - 1] = value[sizeof(value) - 1] = '\0';
        if (!strcmp(field, "TARGET")) win = !strcmp(value, "windows-arm64");
        if (!strcmp(field, "BUILD_ID") && strlen(value) == 32)
            memcpy(m->build_id, value, sizeof(m->build_id));
        if (!strcmp(field, "FILE_IMAGE_LEN") || !strcmp(field, "BACK_OFF")) dm_fail("%s: front/back layouts are not implemented", m->name);
        if (!strcmp(field, "TEXT_END")) m->text = strtoull(value, NULL, 0);
        if (!strcmp(field, "RO_OFF")) m->ro = strtoull(value, NULL, 0);
        if (!strcmp(field, "DATA_OFF")) m->data = strtoull(value, NULL, 0);
        if (!strcmp(field, "KEY_PAGE_OFF")) m->key = strtoull(value, NULL, 0);
        if (!strcmp(field, "KEY_PAGE_SIZE")) key_size = strtoull(value, NULL, 0);
        if (!strcmp(field, "FPS_STATE_OFF")) m->fps = strtoull(value, NULL, 0);
        if (!strcmp(field, "PATCH_OFF")) m->patch = strtoull(value, NULL, 0);
        for (unsigned i = 0; i < ARRAYSIZE(keys); ++i) if (!strcmp(field, keys[i])) {
            uint64_t v = strtoull(value, NULL, 0);
            if (v >= 0x4000 / 8) dm_fail("%s: TLS key out of range", m->name);
            idx[i] = (uint32_t)v;
        }
    }
    if (!win || key_size != 0x4000 || !m->text || m->text > m->ro || m->ro > m->data ||
        m->data > m->key || m->key > m->patch || m->patch - m->key < key_size ||
        ((m->ro | m->data | m->key) & (DM_PAGE - 1)) || (m->patch & 0xffff) ||
        (m->fps && (m->fps < m->key + key_size || m->fps >= m->patch || (m->fps & (DM_PAGE - 1)))))
        dm_fail("%s: invalid Windows near-layout fixups", m->name);
    for (unsigned i = 0; i < ARRAYSIZE(keys); ++i) {
        if (idx[i] == UINT32_MAX) dm_fail("%s: missing %s", m->name, keys[i]);
        for (unsigned j = 0; j < i; ++j) if (idx[i] == idx[j]) dm_fail("%s: duplicated TLS key", m->name);
    }
}

static int compare_gates(const void *a, const void *b) {
    uint64_t x = ((const struct dm_gate_site *)a)->ret, y = ((const struct dm_gate_site *)b)->ret;
    return (x > y) - (x < y);
}

static void read_gates(struct dm_module *m, FILE *f) {
    char line[128];
    for (; fgets(line, sizeof(line), f); ++m->ngates) {}
    if (!m->ngates) return;
    rewind(f);
    m->gates = calloc(m->ngates, sizeof(*m->gates));
    if (!m->gates) dm_fail("cannot allocate gate table");
    for (size_t i = 0; i < m->ngates; ++i) {
        struct dm_gate_site *g = &m->gates[i];
        if (!fgets(line, sizeof(line), f) || sscanf_s(line, "%" SCNx64 " %7s %u %" SCNx64,
            &g->ret, g->kind, (unsigned)sizeof(g->kind), &g->arg, &g->orig) != 4 ||
            g->ret < m->patch || g->ret >= m->size || g->orig >= m->text || ((g->ret | g->orig) & 3))
            dm_fail("%s: bad gate", m->name);
    }
    qsort(m->gates, m->ngates, sizeof(*m->gates), compare_gates);
    for (size_t i = 1; i < m->ngates; ++i)
        if (m->gates[i - 1].ret == m->gates[i].ret) dm_fail("%s: duplicate gate return address", m->name);
}

static void read_dynamic(struct dm_module *m) {
    uint32_t mod0;
    memcpy(&mod0, m->base + 4, sizeof(mod0));
    unsigned char *header = region(m, mod0, 8, 0, m->key);
    if (memcmp(header, "MOD0", 4)) dm_fail("%s: no MOD0 header", m->name);
    int32_t offset;
    memcpy(&offset, header + 4, sizeof(offset));
    uint64_t dyn = (uint64_t)((int64_t)mod0 + offset), hash = 0;
    int end = 0;
    for (unsigned i = 0; i < 4096 && !end; ++i, dyn += sizeof(struct dyn)) {
        struct dyn entry;
        memcpy(&entry, region(m, dyn, sizeof(entry), 0, m->key), sizeof(entry));
        switch (entry.tag) {
        case 0: end = 1; break;
        case 2: m->plt_size = entry.value; break;
        case 4: hash = entry.value; break;
        case 5: m->strings = entry.value; break;
        case 6: m->symbols = entry.value; break;
        case 7: m->rela = entry.value; break;
        case 8: m->rela_size = entry.value; break;
        case 10: m->strings_size = entry.value; break;
        case 12: m->init = entry.value; break;
        case 23: m->plt = entry.value; break;
        case 35: m->relr_size = entry.value; break;
        case 36: m->relr = entry.value; break;
        }
    }
    if (!end || !m->strings || !m->symbols || m->symbols >= m->strings || !m->strings_size)
        dm_fail("%s: no usable dynamic symbol table", m->name);
    if (hash) {
        uint32_t count;
        memcpy(&count, region(m, hash + 4, 4, 0, m->key), 4);
        m->nsyms = count;
    } else m->nsyms = (m->strings - m->symbols) / sizeof(struct sym);
    if (!m->nsyms || m->nsyms > (m->strings - m->symbols) / sizeof(struct sym)) dm_fail("%s: invalid symbol count", m->name);
    region(m, m->symbols, m->nsyms * sizeof(struct sym), 0, m->key);
    region(m, m->strings, m->strings_size, 0, m->key);
    if (m->init >= m->text) dm_fail("%s: DT_INIT outside code", m->name);
}

void dm_module_load(struct dm_module *m, const char *dir, unsigned char *base, size_t available,
                    const uint64_t keys[5]) {
    uint32_t idx[5];
    FILE *f = open_artifact(dir, m->name, "fixups.tsv", "r");
    read_fixups(m, f, idx);
    fclose(f);
    f = open_artifact(dir, m->name, "patched.bin", "rb");
    if (fseek(f, 0, SEEK_END)) dm_fail("%s: cannot size image", m->name);
    long len = ftell(f);
    if (len <= 0 || (uint64_t)len < m->patch || (uint64_t)len > available || fseek(f, 0, SEEK_SET))
        dm_fail("%s: not a near-layout image fitting in the module region", m->name);
    m->size = (uint64_t)len;
    m->base = VirtualAlloc(base, (size_t)m->size, MEM_COMMIT, PAGE_READWRITE);
    if (!m->base || fread(m->base, 1, (size_t)m->size, f) != m->size) dm_fail("%s: cannot load image", m->name);
    fclose(f);
    for (unsigned i = 0; i < ARRAYSIZE(idx); ++i) memcpy(m->base + m->key + idx[i] * 8u, &keys[i], 8);
    f = open_artifact(dir, m->name, "gates.tsv", "r");
    read_gates(m, f);
    fclose(f);
    read_dynamic(m);
    printf("%s: base %p size %#" PRIx64 " gates %zu\n", m->name, m->base, m->size, m->ngates);
}

static struct sym symbol(const struct dm_module *m, uint64_t index) {
    struct sym s;
    if (index >= m->nsyms) dm_fail("%s: symbol index out of bounds", m->name);
    memcpy(&s, m->base + m->symbols + index * sizeof(s), sizeof(s));
    return s;
}

static const char *symbol_name(const struct dm_module *m, struct sym s) {
    if (s.name >= m->strings_size) dm_fail("%s: invalid symbol name offset", m->name);
    const char *name = (const char *)(m->base + m->strings + s.name);
    if (!memchr(name, 0, m->strings_size - s.name)) dm_fail("%s: unterminated symbol", m->name);
    return name;
}

static uintptr_t find_symbol(const struct dm_module *modules, size_t n, const char *name, int exported) {
    for (size_t j = 0; j < n; ++j) {
        const struct dm_module *m = &modules[j];
        for (uint64_t i = 1; i < m->nsyms; ++i) {
            struct sym s = symbol(m, i);
            if (!s.shndx || (exported && (!(s.info >> 4) || (s.other & 3) == 1 || (s.other & 3) == 2)) ||
                strcmp(name, symbol_name(m, s))) continue;
            return (uintptr_t)region(m, s.value, 0, 0, m->key);
        }
    }
    return 0;
}

uintptr_t dm_find_symbol(const struct dm_module *modules, size_t n, const char *name) {
    return find_symbol(modules, n, name, 0);
}

static void relative(const struct dm_module *m, uint64_t off) {
    uint64_t v;
    void *dst = region(m, off, sizeof(v), m->data, m->key);
    memcpy(&v, dst, sizeof(v));
    v += (uintptr_t)m->base;
    memcpy(dst, &v, sizeof(v));
}

static size_t relocate_rela(struct dm_module *m, struct dm_module *mods, size_t n, uint64_t off, uint64_t size) {
    if (!size) return 0;
    if (size % sizeof(struct rela)) dm_fail("%s: bad RELA size", m->name);
    unsigned char *p = region(m, off, size, 0, m->key);
    for (uint64_t i = 0; i < size; i += sizeof(struct rela)) {
        struct rela r;
        memcpy(&r, p + i, sizeof(r));
        uint32_t type = (uint32_t)r.info;
        if (!type) continue;
        uint64_t v;
        if (type == 1027) v = (uintptr_t)m->base + r.addend;  // R_AARCH64_RELATIVE
        else {
            if (type != 257 && type != 1025 && type != 1026) dm_fail("%s: unsupported relocation %u", m->name, type);
            struct sym s = symbol(m, r.info >> 32);
            const char *name = symbol_name(m, s);
            v = (s.info >> 4) && !(s.other & 3) ? dm_boot_import(name) : 0;
            if (!v && (s.info >> 4) && !(s.other & 3)) v = find_symbol(mods, n, name, 1);
            if (!v && s.shndx) v = (uintptr_t)region(m, s.value, 0, 0, m->key);
            if (!v && (s.info >> 4) != 2) dm_fail("%s: unresolved strong import %s", m->name, name);
            if (type != 1026) v += r.addend;
        }
        memcpy(region(m, r.offset, sizeof(v), m->data, m->key), &v, sizeof(v));
    }
    return size / sizeof(struct rela);
}

void dm_module_relocate(struct dm_module *m, struct dm_module *modules, size_t n) {
    size_t count = 0;
    if (m->relr_size) {
        uint64_t where = 0;
        if (m->relr_size % 8) dm_fail("%s: bad RELR size", m->name);
        const unsigned char *p = region(m, m->relr, m->relr_size, 0, m->key);
        for (uint64_t i = 0; i < m->relr_size; i += 8) {
            uint64_t entry;
            memcpy(&entry, p + i, 8);
            if (!(entry & 1)) { relative(m, entry); where = entry + 8; ++count; }
            else {
                if (!where) dm_fail("%s: RELR bitmap has no base", m->name);
                for (unsigned bit = 0; (entry >>= 1); ++bit)
                    if (entry & 1) { relative(m, where + 8 * bit); ++count; }
                where += 63 * 8;
            }
        }
    }
    size_t rela = relocate_rela(m, modules, n, m->rela, m->rela_size);
    size_t plt = relocate_rela(m, modules, n, m->plt, m->plt_size);
    printf("%s: reloc rela=%zu relr=%zu jump=%zu\n", m->name, rela, count, plt);
}

static void protect(const struct dm_module *m, uint64_t start, uint64_t end, DWORD prot) {
    DWORD old;
    if (end > start && !VirtualProtect(m->base + start, end - start, prot, &old))
        dm_fail("%s: VirtualProtect %#" PRIx64 "..%#" PRIx64 " error %lu", m->name, start, end, GetLastError());
}

void dm_module_seal(const struct dm_module *m) {
    protect(m, 0, m->ro, PAGE_EXECUTE_READ);
    protect(m, m->ro, m->data, PAGE_READONLY);
    protect(m, m->key, m->fps ? m->fps : m->patch, PAGE_READONLY);
    protect(m, m->patch, m->size, PAGE_EXECUTE_READ);
    if (!FlushInstructionCache(GetCurrentProcess(), m->base, m->size)) dm_fail("%s: FlushInstructionCache failed", m->name);
}

const struct dm_gate_site *dm_find_gate(const struct dm_module *m, uint64_t ret) {
    const struct dm_gate_site key = { .ret = ret };
    return m->ngates ? bsearch(&key, m->gates, m->ngates, sizeof(*m->gates), compare_gates) : NULL;
}

int dm_module_region(const struct dm_module *m, uint64_t addr, uint64_t *base, uint64_t *size, uint32_t *state, uint32_t *perm) {
    uint64_t mod = (uintptr_t)m->base;
    uint64_t mapped_size = (m->size + DM_PAGE - 1) & ~(uint64_t)(DM_PAGE - 1);
    if (addr < mod || addr - mod >= mapped_size) return 0;
    uint64_t off = addr - mod, lo, hi;
    if (off < m->ro) { lo = 0; hi = m->ro; *state = 3; *perm = 5; }
    else if (off < m->data) { lo = m->ro; hi = m->data; *state = 3; *perm = 1; }
    else if (off < m->key) { lo = m->data; hi = m->key; *state = 4; *perm = 3; }
    else if (off >= m->patch) { lo = m->patch; hi = mapped_size; *state = 4; *perm = 5; }
    else if (!m->fps || off < m->fps) { lo = m->key; hi = m->fps ? m->fps : m->patch; *state = 4; *perm = 1; }
    else { lo = m->fps; hi = m->patch; *state = 4; *perm = 3; }
    *base = mod + lo;
    *size = hi - lo;
    return 1;
}
