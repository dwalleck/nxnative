// Decode constants from the NVN2 SDK's DeviceGetInteger switches without
// executing guest code. Match instruction data flow, not module offsets.
// This recognizes the SDK 22.2.8 layout, not arbitrary AArch64 switches.
#ifndef DM_WINDOWS_NVN2_INFO_H
#define DM_WINDOWS_NVN2_INFO_H
#include <stddef.h>
#include <stdint.h>

struct nvn2_switch { size_t cases; const uint8_t *table; unsigned count; };
struct nvn2_info {
    const uint32_t *code;
    size_t words, leave;
    struct nvn2_switch version, defaults;
    uint32_t default_init;
};

static inline int64_t nvn2_simm(uint32_t v, unsigned bits) {
    int64_t sign = INT64_C(1) << (bits - 1);
    return (int64_t)(v ^ (uint32_t)sign) - sign;
}

static inline int64_t nvn2_adr_imm(uint32_t w) {
    return nvn2_simm(((w >> 3) & 0x1ffffc) | ((w >> 29) & 3), 21);
}

// Common tail of the table switch: ADR cases, LDRB byte offset by item index,
// ADD cases, byte, LSL #2, BR cases. The caller already bounded the index.
static inline int nvn2_jump_tail(const uint32_t *p, unsigned table, unsigned index) {
    unsigned cases = p[0] & 31, offset = p[1] & 31;
    if (table == 31 || index == 31 || cases == 31 || offset == 31 ||
        cases == table || cases == index || cases == offset) return 0;
    return (p[0] & 0x9f000000) == 0x10000000 &&
           p[1] == (0x38606800u | index << 16 | table << 5 | offset) &&
           p[2] == (0x8b000800u | offset << 16 | cases << 5 | cases) &&
           p[3] == (0xd61f0000u | cases << 5);
}

static inline int64_t nvn2_b(unsigned opcode, size_t at) {
    return (int64_t)at + nvn2_simm(opcode & 0x3ffffff, 26);
}

static inline int nvn2_mov_const(uint32_t w, unsigned reg, uint32_t *v) {
    if (w == (0x2a1f03e0u | reg)) { *v = 0; return 1; }
    if ((w & 0xff80001f) == (0x52800000u | reg)) {
        *v = ((w >> 5) & 0xffffu) << ((w >> 17) & 16u);
        return 1;
    }
    // MOV bitmask alias. Validate 32-bit logical immediate fields (N must be zero).
    if ((w & 0xffc003ff) != (0x320003e0u | reg)) return 0;
    unsigned imms = (w >> 10) & 63, immr = (w >> 16) & 63;
    unsigned size = 32;
    while (size > 1 && (imms & size)) size >>= 1;
    unsigned n = (imms & (size - 1)) + 1, r = immr & (size - 1);
    if (size < 2 || n >= size) return 0;
    uint32_t mask = UINT32_MAX >> (32 - size), bits = (UINT32_C(1) << n) - 1;
    *v = 0;
    bits = ((bits >> r) | (r ? bits << (size - r) : 0)) & mask;
    for (unsigned i = 0; i < 32; i += size) *v |= bits << i;
    return 1;
}

static inline int nvn2_follow_const(const uint32_t *code, size_t words, size_t target,
        unsigned reg, uint32_t *v, size_t *exit) {
    if (target >= words || !nvn2_mov_const(code[target], reg, v)) return 0;
    ++target;
    if (target < words && (code[target] & 0xff80001f) == (0x72800000u | reg)) {
        unsigned shift = (code[target] >> 17) & 16u;
        *v = (*v & ~(UINT32_C(0xffff) << shift)) | (((code[target] >> 5) & 0xffffu) << shift);
        ++target;
    }
    if (target >= words || (code[target] & 0xfc000000) != 0x14000000) return 0;
    int64_t dst = nvn2_b(code[target], target);
    if (dst < 0 || (uint64_t)dst >= words) return 0;
    *exit = (size_t)dst;
    return 1;
}

static inline struct nvn2_switch nvn2_table(const uint32_t *code, size_t words,
        size_t ro, size_t data, size_t adrp, size_t adr, unsigned limit) {
    int64_t table = (int64_t)((adrp * 4) & ~(size_t)0xfff) + nvn2_adr_imm(code[adrp]) * 4096 + ((code[adrp + 1] >> 10) & 0xfff);
    int64_t cases = (int64_t)(adr * 4) + nvn2_adr_imm(code[adr]);
    if (table < 0 || (uint64_t)table < ro || (uint64_t)table >= data || limit >= data - (uint64_t)table ||
        cases < 0 || (cases & 3) || (uint64_t)cases / 4 >= words) return (struct nvn2_switch){0};
    return (struct nvn2_switch){ (size_t)cases / 4, (const uint8_t *)code + table, limit + 1 };
}

static inline struct nvn2_switch nvn2_find_defaults(const uint32_t *code, size_t words,
        size_t ro, size_t data, size_t p, uint32_t *init) {
    if (words - p < 12 || code[p] != 0x2a0003e8 || code[p + 1] != 0x51000909 || code[p + 2] != 0x2a1f03e0 ||
        (code[p + 3] & 0xffc003ff) != 0x7100013f || (code[p + 4] & 0xff00001f) != 0x54000008 ||
        (code[p + 5] & 0x9f00001f) != 0x9000000a || (code[p + 6] & 0xffc003ff) != 0x9100014a ||
        !nvn2_mov_const(code[p + 7], 8, init) || !nvn2_jump_tail(code + p + 8, 10, 9)) return (struct nvn2_switch){0};
    unsigned limit = (code[p + 3] >> 10) & 0xfff;
    if (limit > 255) return (struct nvn2_switch){0};
    return nvn2_table(code, words, ro, data, p + 5, p + 8, limit);
}

static inline int nvn2_default(const struct nvn2_info *s, unsigned item, int *value) {
    if (!s->defaults.table || item < 2 || item - 2 >= s->defaults.count) return 0;
    size_t at = s->defaults.cases + s->defaults.table[item - 2], out = at;
    if (at >= s->words) return 0;
    uint32_t v = s->default_init;
    if (s->code[at] == 0xd65f03c0) { *value = 0; return 1; } // Ret without writing the pre-cleared result.
    if (s->code[at] != 0x52800020u && !nvn2_follow_const(s->code, s->words, at, 8, &v, &out)) {
        // Last case can fall directly into the common store.
        if (!nvn2_mov_const(s->code[at], 8, &v)) return 0;
        out = at + 1;
    }
    if (out + 2 >= s->words || s->code[out] != 0x52800020 || s->code[out + 1] != 0xb9000028 ||
        s->code[out + 2] != 0xd65f03c0) return 0;
    *value = (int)v;
    return 1;
}

static inline int nvn2_leaves_w0(const uint32_t *code, size_t words, size_t at) {
    // Leaf RET, or the SDK's frame restore (x19/x20, fp/lr, then SP and return).
    if (at >= words) return 0;
    if (code[at] == 0xd65f03c0) return 1;
    return words - at >= 4 && (code[at] & 0xffc07fff) == 0xa9404ff4 &&
           (code[at + 1] & 0xffc07fff) == 0xa9407bfd && (code[at + 2] & 0xffc003ff) == 0x910003ff &&
           (code[at + 3] == 0xd65f03c0 || code[at + 3] == 0xd65f0fff || code[at + 3] == 0xd65f0bff);
}

static inline int nvn2_case(const struct nvn2_info *s, unsigned item, int *value) {
    if (!s->version.table || item >= s->version.count) return 0;
    if (!item) { *value = 231; return 1; }
    size_t at = s->version.cases + s->version.table[item], out;
    uint32_t v;
    if ((at + 1 == s->leave && nvn2_mov_const(s->code[at], 0, &v)) ||
        (nvn2_follow_const(s->code, s->words, at, 0, &v, &out) && out == s->leave)) {
        *value = (int)v;
        return 1;
    }
    // The outer switch forwards the value it fetched into its stack slot.
    if (at + 1 < s->words && s->code[at] == 0xb85fc3a0 && (s->code[at + 1] & 0xfc000000) == 0x14000000 &&
        nvn2_b(s->code[at + 1], at + 1) == (int64_t)s->leave) return nvn2_default(s, item, value);
    return 0;
}

static inline struct nvn2_info nvn2_find_info(const uint32_t *code, size_t words, size_t ro, size_t data) {
    struct nvn2_info found = {0};
    for (size_t p = 5; p + 4 < words; ++p) {
        if (code[p] != 0x52801ce0) continue;                 // mov w0, #231
        unsigned index = code[p - 1] & 31, limit = (code[p - 5] >> 10) & 0xfff;
        unsigned src = (code[p - 5] >> 5) & 31, table_reg = code[p - 3] & 31;
        if ((code[p - 5] & 0xffc0001f) != 0x7100001f || limit < 1 || limit > 255 ||
            (code[p - 4] & 0xff00001f) != 0x54000008 ||      // b.hi
            (code[p - 3] & 0x9f000000) != 0x90000000 ||      // adrp table
            (code[p - 2] & 0xffc003ff) != (0x91000000u | table_reg << 5 | table_reg) ||
            !index || !table_reg || index == table_reg || !(code[p + 1] & 31) || !(code[p + 2] & 31) ||
            code[p - 1] != (0x2a0003e0u | src << 16 | index) || !nvn2_jump_tail(code + p + 1, table_reg, index)) continue;
        struct nvn2_info s = { .code = code, .words = words };
        s.version = nvn2_table(code, words, ro, data, p - 3, p + 1, limit);
        if (!s.version.table) continue;
        // Major is preloaded before the switch and its case leaves without writing w0.
        s.leave = s.version.cases + s.version.table[0];
        uint32_t minor;
        size_t out;
        if (!nvn2_follow_const(code, words, s.version.cases + s.version.table[1], 0, &minor, &out) ||
            !minor || out != s.leave || !nvn2_leaves_w0(code, words, s.leave)) continue;
        // Verified instruction path for the helper: pass w0's item and a zeroed result slot at fp-4.
        if (p >= 9 && code[p - 9] == 0xd10013a1 && code[p - 8] == (0x2a0003e0u | src) &&
            code[p - 7] == 0xb81fc3bf && (code[p - 6] & 0xfc000000) == 0x94000000) {
            int64_t helper = nvn2_b(code[p - 6], p - 6);
            if (helper >= 0 && (uint64_t)helper < words)
                s.defaults = nvn2_find_defaults(code, words, ro, data, (size_t)helper, &s.default_init);
        }
        if (found.version.table) return (struct nvn2_info){0}; // never choose between ambiguous switches
        found = s;
    }
    return found;
}
#endif
