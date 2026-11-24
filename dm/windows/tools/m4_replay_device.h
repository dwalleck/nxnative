#ifndef DM_M4_REPLAY_DEVICE_H
#define DM_M4_REPLAY_DEVICE_H
#include "m4_window.h"
#include "m4_replay.h"

struct dm_replay_device;
// No shader library configured: success with both outputs NULL (scanout only).
VkResult dm_replay_config(struct dm_replay_device **out, const struct dm_m4_device_requirements **req);
VkResult dm_replay_start(struct dm_replay_device *replay, const struct dm_m4_device *device);
const char *dm_replay_shaders(struct dm_replay_device *replay, const struct dm_m4_replay_keys *keys,
                              struct dm_m4_replay_session *out);
const char *dm_replay_compute_shader(struct dm_replay_device *replay, const uint8_t cs[8],
                                     struct dm_m4_compute_session *out);
void dm_replay_destroy(struct dm_replay_device *replay);
#endif
