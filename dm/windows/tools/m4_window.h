#ifndef DM_WINDOWS_M4_WINDOW_H
#define DM_WINDOWS_M4_WINDOW_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

struct dm_m4_window;
uint32_t dm_m4_focus_serial(void);
void dm_m4_wait_focus_change(uint32_t observed);
// Snapshot 256 virtual keys and return a focus generation; returns zero and
// leaves keys untouched without an opted-in foreground game window. Any thread.
uint32_t dm_m4_window_keys(uint64_t keys[4]);

// Optional replay requirements for VkDeviceCreateInfo. The presenter adds and owns
// the swapchain extensions and swapchain-maintenance feature. The caller's lists
// and feature chain need only remain valid during create.
struct dm_m4_device_requirements {
    uint32_t extension_count;
    const char *const *extensions;
    const VkPhysicalDeviceFeatures *enabled_features;
    const void *device_create_next;
};

// Match the external-buffer capability query to the draw import's exact usage.
#define DM_M4_HOST_IMPORT_USAGE (VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | \
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | \
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)

struct dm_m4_device {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    PFN_vkGetInstanceProcAddr get_instance_proc;
    PFN_vkGetDeviceProcAddr get_device_proc;
    // Enabled opportunistically when creating a replay device.
    VkBool32 primitive_topology_list_restart;
    VkBool32 full_draw_index_uint32;
    VkBool32 null_descriptor;
    VkDeviceSize host_import_alignment; // zero if coherent host import cannot be attempted
};

// All window operations run on the thread that calls create. Pass NULL for the
// default title or if no extra device requirements are needed. Requires a
// proprietary NVIDIA Vulkan 1.2+ driver with KHR or EXT swapchain maintenance1.
// Width/height request the initial client size. On failure *out is NULL.
VkResult dm_m4_window_create(struct dm_m4_window **out, const wchar_t *title,
                             uint32_t width, uint32_t height,
                             const struct dm_m4_device_requirements *requirements);

// Borrowed for native replay; never destroy these handles. Synchronize host
// access to queue against present, and retire/destroy replay resources before
// destroying the window. Replay is responsible for making CPU input readable.
const struct dm_m4_device *dm_m4_window_get_device(const struct dm_m4_window *window);

// Pumps this window's messages. Returns 0 after close; call this even when no
// guest frames are ready, then destroy on the same thread.
int dm_m4_window_poll(struct dm_m4_window *window);

// Present top-down, byte-interleaved RGBA or BGRA8 (R8G8B8A8/B8G8R8A8 UNORM or
// SRGB). These are display-encoded channel bytes, not a linear-light conversion.
// row_pitch is in bytes and must cover width*4. Source storage may be reused after
// return. The whole guest image scales to the client area, ignoring alpha for
// window composition. Present pumps messages; VK_SUCCESS means queued, and
// VK_NOT_READY means no frame queued (minimized, acquire timeout or resize).
// A negative result is terminal; stop presenting and destroy the window.
VkResult dm_m4_window_present(struct dm_m4_window *window, const void *pixels,
                              uint32_t width, uint32_t height, size_t row_pitch,
                              VkFormat format);

// For tiled guest textures, fill exactly row_pitch*height bytes directly in
// mapped staging (tight pitch), after acquire. The pointer is valid only during
// fill; do not reenter the window. Return VK_SUCCESS only if all rows are ready.
// A negative callback result aborts without presenting and is terminal.
typedef VkResult (*dm_m4_fill_frame)(void *userdata, void *pixels, size_t row_pitch);
VkResult dm_m4_window_present_fill(struct dm_m4_window *window, uint32_t width, uint32_t height,
                                   VkFormat format, dm_m4_fill_frame fill, void *userdata);

// Optional observer runs only after WSI accepts the upload, while its mapped
// pixels still belong to this attempt. The surface extent reflects any scaling.
typedef void (*dm_m4_accepted_frame)(void *userdata, const void *pixels,
                                     VkExtent2D source, VkExtent2D surface, int bgra);
VkResult dm_m4_window_present_observe(struct dm_m4_window *window, uint32_t width, uint32_t height,
                                      VkFormat format, dm_m4_fill_frame fill, void *userdata,
                                      dm_m4_accepted_frame accepted, void *observer);

void dm_m4_window_destroy(struct dm_m4_window *window);

#endif
