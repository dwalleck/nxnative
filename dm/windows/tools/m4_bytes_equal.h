#pragma once
#include <arm64_neon.h>
#include <stdint.h>
#include <string.h>

// Texture snapshots need equality, not lexicographic order. Reduce one cache
// line per test; never read padding, the next plane, or past a mapped page.
static inline int m4_bytes_equal(const void *left, const void *right, size_t size)
{
    const uint8_t *a = left, *b = right;
    while (size >= 64) {
        uint8x16_t diff = veorq_u8(vld1q_u8(a), vld1q_u8(b));
        for (size_t i = 16; i < 64; i += 16)
            diff = vorrq_u8(diff, veorq_u8(vld1q_u8(a + i), vld1q_u8(b + i)));
        if (vmaxvq_u8(diff)) return 0;
        a += 64;
        b += 64;
        size -= 64;
    }
    return !size || !memcmp(a, b, size);
}
