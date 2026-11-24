#ifndef DM_WINDOWS_M3_AUDIO_H
#define DM_WINDOWS_M3_AUDIO_H

#include <stddef.h>
#include <stdint.h>

// Horizon DSP service budget; zero means the revision or parameter is outside the sizing model.
size_t dm_audio_service_work_size(const void *parameter, uint32_t revision);

#endif
