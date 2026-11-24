// The controller part of DM_KEYS, DM_CTL and DM_REC. Touch and motion belong to the host.
#pragma once
#include "dm_input.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>

#define DM_KEY_HOLD_DEFAULT 0.15
static const char *const dm_npad_key_names[16] = {
    "A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "Plus", "Minus", "Left", "Up", "Right", "Down"
};

static inline int dm_key_equal(const char *a, const char *b) {
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { ++a; ++b; }
    return !*a && !*b;
}

// Parse one member of a chord. A stick is L/R followed by a direction or x/y in [-1,1], y up.
static inline int dm_parse_npad_key(const char *key, uint64_t *buttons,
                                    int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    for (unsigned bit = 0; bit < 16; ++bit) {
        if (dm_key_equal(key, dm_npad_key_names[bit])) { *buttons |= UINT64_C(1) << bit; return 1; }
    }
    const int side = tolower((unsigned char)key[0]);
    if (side != 'l' && side != 'r') return 0;
    int32_t *sx = side == 'l' ? lx : rx, *sy = side == 'l' ? ly : ry;
    if (dm_key_equal(key + 1, "Left")) *sx = -DM_STICK_MAX;
    else if (dm_key_equal(key + 1, "Right")) *sx = DM_STICK_MAX;
    else if (dm_key_equal(key + 1, "Up")) *sy = DM_STICK_MAX;
    else if (dm_key_equal(key + 1, "Down")) *sy = -DM_STICK_MAX;
    else {
        const char *x = key + 1;
        char *sep, *end;
        float fx = strtof(x, &sep);
        if (sep == x || *sep != '/') return 0;
        const char *y = sep + 1;
        float fy = strtof(y, &end);
        if (end == y || *end || !(fabsf(fx) <= 1 && fabsf(fy) <= 1)) return 0;
        *sx = (int32_t)lrintf(fx * DM_STICK_MAX);
        *sy = (int32_t)lrintf(fy * DM_STICK_MAX);
    }
    return 1;
}
