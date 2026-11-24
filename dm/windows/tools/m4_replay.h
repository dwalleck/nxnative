#ifndef DM_WINDOWS_M4_REPLAY_H
#define DM_WINDOWS_M4_REPLAY_H

#include <stdint.h>

struct dm_m4_draw;
struct dm_m4_shader;
struct dm_m4_overlay;

struct dm_m4_replay_keys {
    const uint8_t *vs, *ps, *ms, *ts; // optional stages are NULL
};

struct dm_m4_replay_session {
    struct dm_m4_draw *draw;
    const struct dm_m4_shader *vs, *ps, *ms, *ts;
    struct dm_m4_overlay *overlay; // only for an unambiguous display borrow
};

struct dm_m4_compute_session {
    struct dm_m4_draw *draw;
    const struct dm_m4_shader *cs;
};

// On success the session borrows the device's draw context and shader modules
// exclusively until dm_m4_replay_end. On failure return a static diagnostic;
// no session is acquired.
const char *dm_m4_replay_begin(const void *guest_device, const struct dm_m4_replay_keys *keys,
                               struct dm_m4_replay_session *out);
const char *dm_m4_replay_begin_compute(const void *guest_device, const uint8_t cs[8],
                                       struct dm_m4_compute_session *out);
void dm_m4_replay_end(void);

#endif
