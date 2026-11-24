// Win32 WSI and guest color-buffer transfer for the native Vulkan replay host.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "m4_window.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The bundled Vulkan headers omit vulkan_win32.h. These are the Win32
// declarations used here from Khronos Vulkan-Headers (Apache-2.0 OR MIT).
#define VK_KHR_WIN32_SURFACE_EXTENSION_NAME "VK_KHR_win32_surface"
typedef struct VkWin32SurfaceCreateInfoKHR {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    HINSTANCE hinstance;
    HWND hwnd;
} VkWin32SurfaceCreateInfoKHR;
typedef VkResult (VKAPI_PTR *PFN_vkCreateWin32SurfaceKHR)(VkInstance instance,
    const VkWin32SurfaceCreateInfoKHR *info, const VkAllocationCallbacks *allocator, VkSurfaceKHR *surface);

#define INSTANCE_API(X) \
    X(vkDestroyInstance) X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR) \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceImageFormatProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice)
#define DEVICE_API(X) \
    X(vkDestroyDevice) X(vkDeviceWaitIdle) X(vkGetDeviceQueue) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkFlushMappedMemoryRanges) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdBlitImage)

// The currently active prefix of fences covers even an acquire that was not
// followed by a successful submit or a submit whose present failed to enqueue.
enum { ACQUIRE, SUBMIT, PRESENT, FENCE_COUNT };

enum { CAPTURE_INTERVAL = 60 };
static const wchar_t capture_name[] = L"m4-latest.ppm";
static const wchar_t capture_temp[] = L"m4-latest.ppm.tmp";

struct frame_capture {
    wchar_t path[MAX_PATH], temp[MAX_PATH];
    uint64_t sequence;
    volatile LONG64 *present_count; // Optional supervisor-owned, process-wide WSI heartbeat.
    int failed;
};

struct dm_m4_window {
    HWND hwnd;
    int closed, keyboard_enabled;
    HMODULE loader;
    struct dm_m4_device native;
#define FIELD(name) PFN_##name name;
    INSTANCE_API(FIELD)
    DEVICE_API(FIELD)
#undef FIELD
    VkSurfaceKHR surface;
    VkSurfaceFormatKHR surface_format;
    VkPhysicalDeviceMemoryProperties memory;
    uint32_t max_dimension;
    VkSwapchainKHR swapchain;
    VkExtent2D extent, client;
    int recreate;
    VkImage *swap_images;
    VkFence fences[FENCE_COUNT];
    uint32_t pending_fences;
    VkSemaphore acquired, transferred;
    VkCommandPool command_pool;
    VkCommandBuffer command;
    VkBuffer upload;
    VkDeviceMemory upload_memory;
    void *mapped;
    size_t upload_capacity;
    VkMemoryPropertyFlags upload_flags;
    VkImage source;
    VkDeviceMemory source_memory;
    VkFormat source_format;
    VkExtent2D source_extent;
    struct frame_capture *capture;
};

static VkResult vk_error(VkResult result, const char *operation) {
    fprintf(stderr, "[m4 window] %s: VkResult %d\n", operation, (int)result);
    return result;
}

#define VK_TRY(call) do { \
    VkResult status_ = (call); \
    if (status_ != VK_SUCCESS) return vk_error(status_, #call); \
} while (0)

// A Vulkan output parameter may be poisoned on failure. Publish handles used
// by partial-init cleanup only after a successful call.
#define VK_OUTPUT(type, target, fn, ...) do { \
    type value_; \
    VK_TRY(fn(__VA_ARGS__, &value_)); \
    (target) = value_; \
} while (0)

static VkResult win32_error(const char *operation) {
    fprintf(stderr, "[m4 window] %s: Win32 error %lu\n", operation, GetLastError());
    return VK_ERROR_INITIALIZATION_FAILED;
}

// A focus transition invalidates the state even if a second transition
// restores it before the game polls. The low bit is foreground.
static volatile LONG focus_serial = 1;
uint32_t dm_m4_focus_serial(void) { return (uint32_t)InterlockedCompareExchange(&focus_serial, 0, 0); }
static SRWLOCK focus_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE focus_changed = CONDITION_VARIABLE_INIT;
void dm_m4_wait_focus_change(uint32_t observed) {
    AcquireSRWLockExclusive(&focus_lock);
    while (dm_m4_focus_serial() == observed)
        SleepConditionVariableSRW(&focus_changed, &focus_lock, INFINITE, 0);
    ReleaseSRWLockExclusive(&focus_lock);
}

// Only WM_KEY messages actually delivered to the focused game HWND are used.
// The guest reads on another thread; a new focus serial discards old history.
static SRWLOCK keyboard_lock = SRWLOCK_INIT;
static HWND keyboard_window;
static uint32_t keyboard_serial;
static uint64_t keyboard_keys[4];

uint32_t dm_m4_window_keys(uint64_t keys[4]) {
    AcquireSRWLockShared(&keyboard_lock);
    uint32_t serial = 0;
    if (keyboard_window && GetForegroundWindow() == keyboard_window) {
        serial = keyboard_serial;
        memcpy(keys, keyboard_keys, sizeof(keyboard_keys));
    }
    ReleaseSRWLockShared(&keyboard_lock);
    return serial;
}

static void window_keyboard_focus(HWND hwnd, int active) {
    AcquireSRWLockExclusive(&keyboard_lock);
    if (active || keyboard_window == hwnd) {
        keyboard_window = active ? hwnd : NULL;
        if (active && !++keyboard_serial) ++keyboard_serial;
        memset(keyboard_keys, 0, sizeof(keyboard_keys));
    }
    ReleaseSRWLockExclusive(&keyboard_lock);
}

static void window_keyboard_key(HWND hwnd, unsigned key, int down) {
    const uint64_t bit = UINT64_C(1) << (key % 64);
    AcquireSRWLockExclusive(&keyboard_lock);
    if (keyboard_window == hwnd) {
        if (down) keyboard_keys[key / 64] |= bit;
        else keyboard_keys[key / 64] &= ~bit;
    }
    ReleaseSRWLockExclusive(&keyboard_lock);
}
static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
    } else if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_DESTROY ||
               message == WM_KEYDOWN || message == WM_KEYUP || message == WM_SYSKEYUP) {
        const struct dm_m4_window *w = (const struct dm_m4_window *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (w && w->keyboard_enabled) {
            if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_DESTROY) {
                window_keyboard_focus(hwnd, message == WM_SETFOCUS);
            } else if (wparam < 256 && (message != WM_KEYDOWN || !(lparam & (1L << 30)))) {
                // Don't let a key held in another app enter as auto-repeat on focus.
                window_keyboard_key(hwnd, (unsigned)wparam, message == WM_KEYDOWN);
            }
        }
    } else if (message == WM_ACTIVATEAPP) {
        AcquireSRWLockExclusive(&focus_lock);
        if ((dm_m4_focus_serial() & 1u) != (wparam ? 1u : 0u)) {
            InterlockedIncrement(&focus_serial);
            WakeAllConditionVariable(&focus_changed);
        }
        ReleaseSRWLockExclusive(&focus_lock);
    } else if (message == WM_CLOSE) {
        struct dm_m4_window *w = (struct dm_m4_window *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        w->closed = 1;
        if (w->keyboard_enabled) window_keyboard_focus(hwnd, 0);
        // Keep the HWND alive until its Vulkan surface and swapchain are gone.
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    } else if (message == WM_PAINT) {
        PAINTSTRUCT paint;
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static const wchar_t window_class[] = L"NXNativeM4Window";
static INIT_ONCE window_class_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK register_window_class(PINIT_ONCE once, PVOID module, PVOID *context) {
    (void)once;
    (void)context;
    WNDCLASSW wc = { .lpfnWndProc = window_proc, .hInstance = module,
        .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .lpszClassName = window_class };
    return RegisterClassW(&wc) != 0;
}

static VkResult create_hwnd(struct dm_m4_window *w, const wchar_t *title, uint32_t width, uint32_t height) {
    wchar_t keyboard[2];
    w->keyboard_enabled = GetEnvironmentVariableW(L"DM_KEYBOARD", keyboard, ARRAYSIZE(keyboard)) == 1 &&
                          keyboard[0] == L'1';
    HINSTANCE module = GetModuleHandleW(NULL);
    if (!InitOnceExecuteOnce(&window_class_once, register_window_class, module, NULL))
        return win32_error("RegisterClassW");
    RECT rect = { .right = (LONG)width, .bottom = (LONG)height };
    if (!AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0))
        return win32_error("AdjustWindowRectEx");
    w->hwnd = CreateWindowExW(0, window_class, title ? title : L"nxnative", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        NULL, NULL, module, w);
    if (!w->hwnd) return win32_error("CreateWindowExW");
    return VK_SUCCESS;
}

static int has_extension(const VkExtensionProperties *extensions, uint32_t count, const char *name) {
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(extensions[i].extensionName, name)) return 1;
    return 0;
}

static VkResult create_instance(struct dm_m4_window *w, int *maintenance_khr, int *maintenance_ext) {
    w->loader = LoadLibraryExW(L"vulkan-1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!w->loader) return win32_error("LoadLibraryExW(vulkan-1.dll)");
    w->native.get_instance_proc = (PFN_vkGetInstanceProcAddr)GetProcAddress(w->loader, "vkGetInstanceProcAddr");
    if (!w->native.get_instance_proc) return win32_error("GetProcAddress(vkGetInstanceProcAddr)");
#define GLOBAL(name) PFN_##name name = (PFN_##name)w->native.get_instance_proc(VK_NULL_HANDLE, #name); \
    if (!name) return vk_error(VK_ERROR_INITIALIZATION_FAILED, "Missing " #name)
    GLOBAL(vkCreateInstance);
    GLOBAL(vkEnumerateInstanceExtensionProperties);
#undef GLOBAL
    uint32_t count;
    VK_TRY(vkEnumerateInstanceExtensionProperties(NULL, &count, NULL));
    VkExtensionProperties *available = calloc(count ? count : 1, sizeof(*available));
    if (!available) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult status = vkEnumerateInstanceExtensionProperties(NULL, &count, available);
    if (status == VK_SUCCESS) {
        *maintenance_khr = has_extension(available, count, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        *maintenance_ext = has_extension(available, count, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    }
    free(available);
    if (status != VK_SUCCESS) return vk_error(status, "vkEnumerateInstanceExtensionProperties");
    // Vulkan 1.2 core alone does not allow provably safe present-resource teardown.
    if (!*maintenance_khr && !*maintenance_ext)
        return vk_error(VK_ERROR_EXTENSION_NOT_PRESENT, "No Vulkan surface-maintenance extension");
    const char *extensions[5] = {
        VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME
    };
    count = 3;
    if (*maintenance_khr) extensions[count++] = VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
    if (*maintenance_ext) extensions[count++] = VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME;
    const VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "nxnative", .apiVersion = VK_API_VERSION_1_2 };
    const VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app, .enabledExtensionCount = count, .ppEnabledExtensionNames = extensions };
    VK_OUTPUT(VkInstance, w->native.instance, vkCreateInstance, &ci, NULL);
#define LOAD(name) w->name = (PFN_##name)w->native.get_instance_proc(w->native.instance, #name); \
    if (!w->name) return vk_error(VK_ERROR_INITIALIZATION_FAILED, "Missing " #name);
    INSTANCE_API(LOAD)
#undef LOAD
    w->native.get_device_proc = (PFN_vkGetDeviceProcAddr)w->native.get_instance_proc(w->native.instance, "vkGetDeviceProcAddr");
    if (!w->native.get_device_proc) return vk_error(VK_ERROR_INITIALIZATION_FAILED, "Missing vkGetDeviceProcAddr");
    return VK_SUCCESS;
}

static VkDeviceSize host_import_alignment(const struct dm_m4_window *w, VkPhysicalDevice gpu) {
    PFN_vkGetPhysicalDeviceExternalBufferProperties get_external =
        (PFN_vkGetPhysicalDeviceExternalBufferProperties)w->native.get_instance_proc(
            w->native.instance, "vkGetPhysicalDeviceExternalBufferProperties");
    if (!get_external) return 0;
    const VkPhysicalDeviceExternalBufferInfo info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .usage = DM_M4_HOST_IMPORT_USAGE,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
    };
    VkExternalBufferProperties external = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
    get_external(gpu, &info, &external);
    const VkExternalMemoryProperties *memory = &external.externalMemoryProperties;
    if (!(memory->compatibleHandleTypes & info.handleType) ||
        !(memory->externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
        (memory->externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT))
        return 0;
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT,
    };
    VkPhysicalDeviceProperties2 props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &host,
    };
    w->vkGetPhysicalDeviceProperties2(gpu, &props);
    return host.minImportedHostPointerAlignment;
}

static VkResult create_device(struct dm_m4_window *w, const struct dm_m4_device_requirements *requirements,
                              int maintenance_khr, int maintenance_ext) {
    uint32_t count;
    VK_TRY(w->vkEnumeratePhysicalDevices(w->native.instance, &count, NULL));
    if (!count) return vk_error(VK_ERROR_INITIALIZATION_FAILED, "No Vulkan physical devices");
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult status = w->vkEnumeratePhysicalDevices(w->native.instance, &count, devices);
    if (status == VK_SUCCESS) {
        for (uint32_t i = 0; i < count; ++i) {
            VkPhysicalDeviceDriverProperties driver = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
            VkPhysicalDeviceProperties2 props = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                .pNext = &driver };
            w->vkGetPhysicalDeviceProperties2(devices[i], &props);
            if (driver.driverID == VK_DRIVER_ID_NVIDIA_PROPRIETARY && props.properties.apiVersion >= VK_API_VERSION_1_2) {
                w->native.physical = devices[i];
                w->max_dimension = props.properties.limits.maxImageDimension2D;
                break;
            }
        }
    }
    free(devices);
    if (status != VK_SUCCESS) return vk_error(status, "vkEnumeratePhysicalDevices");
    const VkPhysicalDevice gpu = w->native.physical;
    if (!gpu) return vk_error(VK_ERROR_INITIALIZATION_FAILED, "No NVIDIA proprietary Vulkan 1.2+ device");

    w->vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, NULL);
    if (!count) return vk_error(VK_ERROR_INITIALIZATION_FAILED, "No Vulkan queue families");
    const uint32_t capacity = count;
    VkQueueFamilyProperties *families = calloc(capacity, sizeof(*families));
    if (!families) return VK_ERROR_OUT_OF_HOST_MEMORY;
    w->vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families);
    if (count > capacity) { free(families); return VK_ERROR_INITIALIZATION_FAILED; }
    uint32_t family;
    for (family = 0; family < count; ++family) {
        if (!families[family].queueCount || !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 supported;
        status = w->vkGetPhysicalDeviceSurfaceSupportKHR(gpu, family, w->surface, &supported);
        if (status != VK_SUCCESS || supported) break;
    }
    free(families);
    if (status != VK_SUCCESS) return vk_error(status, "vkGetPhysicalDeviceSurfaceSupportKHR");
    if (family == count) return vk_error(VK_ERROR_INITIALIZATION_FAILED, "No graphics/present queue family");
    w->native.queue_family = family;

    VK_TRY(w->vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, NULL));
    VkExtensionProperties *available = calloc(count ? count : 1, sizeof(*available));
    if (!available) return VK_ERROR_OUT_OF_HOST_MEMORY;
    status = w->vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, available);
    const char *maintenance = NULL;
    VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT list_restart = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT };
    VkPhysicalDeviceRobustness2FeaturesEXT nulls = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    int query_nulls = 0;
    int query_list_restart = 0;
    VkDeviceSize import_alignment = 0;
    if (status == VK_SUCCESS) {
        if (maintenance_khr && has_extension(available, count, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))
            maintenance = VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
        else if (maintenance_ext && has_extension(available, count, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME))
            maintenance = VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
        query_list_restart = requirements &&
            has_extension(available, count, VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME);
        query_nulls = requirements && has_extension(available, count, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
        if (requirements && has_extension(available, count, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME))
            import_alignment = host_import_alignment(w, gpu);
    }
    free(available);
    if (status != VK_SUCCESS) return vk_error(status, "vkEnumerateDeviceExtensionProperties");
    if (!maintenance) return vk_error(VK_ERROR_EXTENSION_NOT_PRESENT, "No NVIDIA swapchain-maintenance extension");

    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR maint = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR };
    if (query_list_restart) maint.pNext = &list_restart;
    if (query_nulls) { nulls.pNext = maint.pNext; maint.pNext = &nulls; }
    VkPhysicalDeviceFeatures2 features = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &maint };
    w->vkGetPhysicalDeviceFeatures2(gpu, &features);
    if (!maint.swapchainMaintenance1) return vk_error(VK_ERROR_FEATURE_NOT_PRESENT, "No swapchainMaintenance1 feature");
    count = requirements ? requirements->extension_count : 0;
    const char *base_extensions[5] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, maintenance};
    uint32_t base_count = 2;
    if (list_restart.primitiveTopologyListRestart) base_extensions[base_count++] = VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME;
    if (nulls.nullDescriptor) base_extensions[base_count++] = VK_EXT_ROBUSTNESS_2_EXTENSION_NAME;
    if (import_alignment) base_extensions[base_count++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    const char **extensions = count ? calloc((size_t)count + base_count, sizeof(*extensions)) : base_extensions;
    if (!extensions) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (count) {
        memcpy(extensions, base_extensions, base_count * sizeof(*extensions));
        memcpy(extensions + base_count, requirements->extensions, count * sizeof(*extensions));
    }
    VkPhysicalDeviceFeatures enabled = {0};
    if (requirements) {
        if (requirements->enabled_features) {
            enabled = *requirements->enabled_features;
            enabled.fullDrawIndexUint32 |= features.features.fullDrawIndexUint32;
        }
        maint.pNext = (void *)requirements->device_create_next;
        if (nulls.nullDescriptor) {
            nulls.robustBufferAccess2 = nulls.robustImageAccess2 = VK_FALSE;
            nulls.pNext = maint.pNext;
            maint.pNext = &nulls;
        }
        if (list_restart.primitiveTopologyListRestart) {
            list_restart.primitiveTopologyPatchListRestart = VK_FALSE;
            list_restart.pNext = maint.pNext;
            maint.pNext = &list_restart;
        }
    }
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queue = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority };
    const VkDeviceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &maint,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue,
        .enabledExtensionCount = count + base_count, .ppEnabledExtensionNames = extensions,
        .pEnabledFeatures = requirements && requirements->enabled_features ? &enabled : NULL };
    VkDevice device;
    status = w->vkCreateDevice(gpu, &ci, NULL, &device);
    if (count) free(extensions);
    if (status != VK_SUCCESS) return vk_error(status, "vkCreateDevice");
    w->native.device = device;
    w->native.primitive_topology_list_restart = list_restart.primitiveTopologyListRestart;
    w->native.full_draw_index_uint32 = enabled.fullDrawIndexUint32;
    w->native.null_descriptor = nulls.nullDescriptor;
    w->native.host_import_alignment = import_alignment;
#define LOAD(name) w->name = (PFN_##name)w->native.get_device_proc(w->native.device, #name); \
    if (!w->name) return vk_error(VK_ERROR_INITIALIZATION_FAILED, "Missing " #name);
    DEVICE_API(LOAD)
#undef LOAD
    w->vkGetDeviceQueue(w->native.device, family, 0, &w->native.queue);
    w->vkGetPhysicalDeviceMemoryProperties(gpu, &w->memory);
    return VK_SUCCESS;
}

static VkResult create_frame_resources(struct dm_m4_window *w) {
    const VkDevice device = w->native.device;
    const VkCommandPoolCreateInfo pool = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = w->native.queue_family };
    VK_OUTPUT(VkCommandPool, w->command_pool, w->vkCreateCommandPool, device, &pool, NULL);
    const VkCommandBufferAllocateInfo cmd = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = w->command_pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VK_OUTPUT(VkCommandBuffer, w->command, w->vkAllocateCommandBuffers, device, &cmd);
    const VkFenceCreateInfo fence = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    for (unsigned i = 0; i < FENCE_COUNT; ++i)
        VK_OUTPUT(VkFence, w->fences[i], w->vkCreateFence, device, &fence, NULL);
    const VkSemaphoreCreateInfo sem = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VK_OUTPUT(VkSemaphore, w->acquired, w->vkCreateSemaphore, device, &sem, NULL);
    VK_OUTPUT(VkSemaphore, w->transferred, w->vkCreateSemaphore, device, &sem, NULL);
    return VK_SUCCESS;
}

static int same_extent(VkExtent2D a, VkExtent2D b) {
    return a.width == b.width && a.height == b.height;
}

static uint32_t clamp_extent(uint32_t value, uint32_t min, uint32_t max) {
    return value < min ? min : value > max ? max : value;
}

static VkResult choose_surface_format(struct dm_m4_window *w) {
    uint32_t count = 0;
    VK_TRY(w->vkGetPhysicalDeviceSurfaceFormatsKHR(w->native.physical, w->surface, &count, NULL));
    if (!count) return vk_error(VK_ERROR_FORMAT_NOT_SUPPORTED, "No surface formats");
    VkSurfaceFormatKHR *formats = calloc(count, sizeof(*formats));
    if (!formats) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult status = w->vkGetPhysicalDeviceSurfaceFormatsKHR(w->native.physical, w->surface, &count, formats);
    const VkFormat preferred[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB };
    w->surface_format.format = VK_FORMAT_UNDEFINED;
    if (status == VK_SUCCESS) {
        for (unsigned i = 0; i < ARRAYSIZE(preferred) && w->surface_format.format == VK_FORMAT_UNDEFINED; ++i) {
            for (uint32_t j = 0; j < count; ++j) {
                if (formats[j].colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR || formats[j].format != preferred[i]) continue;
                VkFormatProperties props;
                w->vkGetPhysicalDeviceFormatProperties(w->native.physical, preferred[i], &props);
                const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
                if ((props.optimalTilingFeatures & need) == need) w->surface_format = formats[j];
                break;
            }
        }
    }
    free(formats);
    if (status != VK_SUCCESS) return vk_error(status, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (w->surface_format.format == VK_FORMAT_UNDEFINED)
        return vk_error(VK_ERROR_FORMAT_NOT_SUPPORTED, "No transferable RGBA8/BGRA8 SDR surface format");
    return VK_SUCCESS;
}

static VkResult rebuild_swapchain(struct dm_m4_window *w, VkExtent2D client) {
    VkSurfaceCapabilitiesKHR caps;
    VK_TRY(w->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(w->native.physical, w->surface, &caps));
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = clamp_extent(client.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = clamp_extent(client.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (!extent.width || !extent.height) return VK_NOT_READY;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR))
        return vk_error(VK_ERROR_FORMAT_NOT_SUPPORTED, "Surface cannot present opaque transfer destinations");
    VkResult status = choose_surface_format(w);
    if (status == VK_INCOMPLETE) return VK_NOT_READY;
    if (status != VK_SUCCESS) return status;
    uint32_t images = caps.minImageCount;
    if (!caps.maxImageCount || images < caps.maxImageCount) ++images;
    const VkSwapchainCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = w->surface, .minImageCount = images, .imageFormat = w->surface_format.format,
        .imageColorSpace = w->surface_format.colorSpace, .imageExtent = extent, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT, .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ?
            VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE, .oldSwapchain = w->swapchain };
    VkSwapchainKHR swapchain;
    status = w->vkCreateSwapchainKHR(w->native.device, &ci, NULL, &swapchain);
    // Once passed as oldSwapchain the previous swapchain is retired even on failure.
    if (w->swapchain) w->vkDestroySwapchainKHR(w->native.device, w->swapchain, NULL);
    w->swapchain = VK_NULL_HANDLE;
    free(w->swap_images);
    w->swap_images = NULL;
    if (status == VK_ERROR_OUT_OF_DATE_KHR) return VK_NOT_READY;
    if (status != VK_SUCCESS) return vk_error(status, "vkCreateSwapchainKHR");
    w->swapchain = swapchain;
    VK_TRY(w->vkGetSwapchainImagesKHR(w->native.device, swapchain, &images, NULL));
    w->swap_images = calloc(images, sizeof(*w->swap_images));
    if (!w->swap_images) return VK_ERROR_OUT_OF_HOST_MEMORY;
    status = w->vkGetSwapchainImagesKHR(w->native.device, swapchain, &images, w->swap_images);
    if (status == VK_INCOMPLETE) return VK_NOT_READY;
    if (status != VK_SUCCESS) return vk_error(status, "vkGetSwapchainImagesKHR");
    w->extent = extent;
    w->client = client;
    w->recreate = 0;
    return VK_SUCCESS;
}

static void free_upload(struct dm_m4_window *w) {
    if (w->mapped) w->vkUnmapMemory(w->native.device, w->upload_memory);
    if (w->upload) w->vkDestroyBuffer(w->native.device, w->upload, NULL);
    if (w->upload_memory) w->vkFreeMemory(w->native.device, w->upload_memory, NULL);
    w->mapped = NULL;
    w->upload = VK_NULL_HANDLE;
    w->upload_memory = VK_NULL_HANDLE;
    w->upload_capacity = 0;
}

static void free_source(struct dm_m4_window *w) {
    if (w->source) w->vkDestroyImage(w->native.device, w->source, NULL);
    if (w->source_memory) w->vkFreeMemory(w->native.device, w->source_memory, NULL);
    w->source = VK_NULL_HANDLE;
    w->source_memory = VK_NULL_HANDLE;
}

static uint32_t memory_type(const struct dm_m4_window *w, uint32_t bits,
                            VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) {
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < w->memory.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = w->memory.memoryTypes[i].propertyFlags;
        if (!(bits & (UINT32_C(1) << i)) || (flags & required) != required) continue;
        if ((flags & preferred) == preferred) return i;
        if (type == UINT32_MAX) type = i;
    }
    return type;
}

static VkResult ensure_upload(struct dm_m4_window *w, size_t bytes) {
    if (bytes <= w->upload_capacity) return VK_SUCCESS;
    free_upload(w);
    const VkDevice device = w->native.device;
    const VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    VK_OUTPUT(VkBuffer, w->upload, w->vkCreateBuffer, device, &ci, NULL);
    VkMemoryRequirements req;
    w->vkGetBufferMemoryRequirements(device, w->upload, &req);
    const uint32_t type = memory_type(w, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) return vk_error(VK_ERROR_FEATURE_NOT_PRESENT, "No host-visible upload memory");
    const VkMemoryDedicatedAllocateInfo dedicated = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .buffer = w->upload };
    const VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated, .allocationSize = req.size, .memoryTypeIndex = type };
    VK_OUTPUT(VkDeviceMemory, w->upload_memory, w->vkAllocateMemory, device, &alloc, NULL);
    VK_TRY(w->vkBindBufferMemory(device, w->upload, w->upload_memory, 0));
    VK_OUTPUT(void *, w->mapped, w->vkMapMemory, device, w->upload_memory, 0, VK_WHOLE_SIZE, 0);
    w->upload_capacity = bytes;
    w->upload_flags = w->memory.memoryTypes[type].propertyFlags;
    return VK_SUCCESS;
}

static VkResult ensure_source(struct dm_m4_window *w, VkExtent2D extent, VkFormat format) {
    if (w->source && same_extent(w->source_extent, extent) && format == w->source_format) return VK_SUCCESS;
    VkFormatProperties format_props;
    w->vkGetPhysicalDeviceFormatProperties(w->native.physical, format, &format_props);
    if (!(format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT))
        return vk_error(VK_ERROR_FORMAT_NOT_SUPPORTED, "Guest format cannot be blitted");
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageFormatProperties image_props;
    VK_TRY(w->vkGetPhysicalDeviceImageFormatProperties(w->native.physical, format, VK_IMAGE_TYPE_2D,
                                                       VK_IMAGE_TILING_OPTIMAL, usage, 0, &image_props));
    if (extent.width > image_props.maxExtent.width || extent.height > image_props.maxExtent.height)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    free_source(w);
    const VkDevice device = w->native.device;
    const VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = format, .extent = { extent.width, extent.height, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage };
    VK_OUTPUT(VkImage, w->source, w->vkCreateImage, device, &ci, NULL);
    VkMemoryRequirements req;
    w->vkGetImageMemoryRequirements(device, w->source, &req);
    const uint32_t type = memory_type(w, req.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) return vk_error(VK_ERROR_FEATURE_NOT_PRESENT, "No blit source memory type");
    const VkMemoryDedicatedAllocateInfo dedicated = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .image = w->source };
    const VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &dedicated, .allocationSize = req.size, .memoryTypeIndex = type };
    VK_OUTPUT(VkDeviceMemory, w->source_memory, w->vkAllocateMemory, device, &alloc, NULL);
    VK_TRY(w->vkBindImageMemory(device, w->source, w->source_memory, 0));
    w->source_extent = extent;
    w->source_format = format;
    return VK_SUCCESS;
}

static void image_barrier(const struct dm_m4_window *w, VkImage image, VkImageLayout old_layout,
                          VkImageLayout new_layout, VkAccessFlags from, VkAccessFlags to,
                          VkPipelineStageFlags dst_stage) {
    const VkImageMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = from, .dstAccessMask = to, .oldLayout = old_layout, .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    w->vkCmdPipelineBarrier(w->command, VK_PIPELINE_STAGE_TRANSFER_BIT, dst_stage, 0,
                            0, NULL, 0, NULL, 1, &barrier);
}

static VkResult record_frame(const struct dm_m4_window *w, VkImage destination, VkExtent2D extent, int blit) {
    const VkCommandBuffer cmd = w->command;
    const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK_TRY(w->vkResetCommandBuffer(cmd, 0));
    VK_TRY(w->vkBeginCommandBuffer(cmd, &begin));
    const VkImageLayout write = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const VkImageLayout read = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    image_barrier(w, destination, VK_IMAGE_LAYOUT_UNDEFINED, write, 0, VK_ACCESS_TRANSFER_WRITE_BIT, transfer);
    const VkImage upload_target = blit ? w->source : destination;
    if (blit) image_barrier(w, upload_target, VK_IMAGE_LAYOUT_UNDEFINED, write, 0, VK_ACCESS_TRANSFER_WRITE_BIT, transfer);
    const VkImageSubresourceLayers color = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 };
    const VkBufferImageCopy copy = { .imageSubresource = color, .imageExtent = { extent.width, extent.height, 1 } };
    w->vkCmdCopyBufferToImage(cmd, w->upload, upload_target, write, 1, &copy);
    if (blit) {
        image_barrier(w, upload_target, write, read, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, transfer);
        const VkImageBlit region = {
            .srcSubresource = color, .srcOffsets = { {0}, { (int32_t)extent.width, (int32_t)extent.height, 1 } },
            .dstSubresource = color, .dstOffsets = { {0}, { (int32_t)w->extent.width, (int32_t)w->extent.height, 1 } }
        };
        w->vkCmdBlitImage(cmd, upload_target, read, destination, write, 1, &region, VK_FILTER_NEAREST);
    }
    image_barrier(w, destination, write, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    VK_TRY(w->vkEndCommandBuffer(cmd));
    return VK_SUCCESS;
}

static VkResult wait_frame(struct dm_m4_window *w) {
    if (w->pending_fences) {
        VK_TRY(w->vkWaitForFences(w->native.device, w->pending_fences, w->fences, VK_TRUE, UINT64_MAX));
        w->pending_fences = 0;
    }
    return VK_SUCCESS;
}

static struct frame_capture *open_capture(void) {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    const DWORD len = GetEnvironmentVariableW(L"DM_CAPTURE_DIR", dir, ARRAYSIZE(dir));
    if (!len) return NULL;
    DWORD end = len < ARRAYSIZE(dir) ? GetFullPathNameW(dir, ARRAYSIZE(path), path, NULL) : 0;
    if (!end || end >= ARRAYSIZE(path)) goto invalid;
    const DWORD attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) goto invalid;
    if (path[end - 1] != L'\\') path[end++] = L'\\';
    if (end + ARRAYSIZE(capture_temp) > ARRAYSIZE(path)) goto invalid;

    struct frame_capture *capture = calloc(1, sizeof(*capture));
    if (!capture) {
        fprintf(stderr, "[m4 window] cannot allocate DM_CAPTURE_DIR state\n");
        return NULL;
    }
    memcpy(capture->path, path, end * sizeof(*path));
    memcpy(capture->temp, path, end * sizeof(*path));
    memcpy(capture->path + end, capture_name, sizeof(capture_name));
    memcpy(capture->temp + end, capture_temp, sizeof(capture_temp));
    wchar_t name[96];
    const DWORD count = GetEnvironmentVariableW(L"DM_PRESENT_COUNTER", name, ARRAYSIZE(name));
    if (count) {
        HANDLE section = count < ARRAYSIZE(name) ? OpenFileMappingW(FILE_MAP_WRITE, FALSE, name) : NULL;
        if (section) {
            capture->present_count = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, sizeof(LONG64));
            CloseHandle(section);
        }
        if (capture->present_count) InterlockedCompareExchange64(capture->present_count, 0, -1);
        else fprintf(stderr, "[RED] could not open diagnostic present counter\n");
    }
    return capture;

invalid:
    fprintf(stderr, "[m4 window] DM_CAPTURE_DIR must be an existing directory with room for the capture filename\n");
    return NULL;
}

static int write_capture(const struct frame_capture *capture, const unsigned char *pixels,
                         VkExtent2D extent, int bgra) {
    FILE *file;
    if (_wfopen_s(&file, capture->temp, L"wb") || !file) return 0;
    int ok = fprintf(file, "P6\n# Vulkan present %llu, source %s\n%u %u\n255\n",
                     (unsigned long long)capture->sequence, bgra ? "BGRA8" : "RGBA8",
                     extent.width, extent.height) > 0;
    // PPM is top-down RGB; discard alpha and write contiguous batches.
    const unsigned red = bgra ? 2 : 0, blue = 2 - red;
    unsigned char rgb[3 * 4096];
    for (size_t left = (size_t)extent.width * extent.height; ok && left;) {
        const size_t count = min(left, sizeof(rgb) / 3);
        for (size_t i = 0; i < count; ++i, pixels += 4) {
            rgb[3 * i] = pixels[red];
            rgb[3 * i + 1] = pixels[1];
            rgb[3 * i + 2] = pixels[blue];
        }
        ok = fwrite(rgb, 3, count, file) == count;
        left -= count;
    }
    if (fclose(file) == 0 && ok &&
        MoveFileExW(capture->temp, capture->path, MOVEFILE_REPLACE_EXISTING)) return 1;
    DeleteFileW(capture->temp);
    return 0;
}

static void capture_present(struct dm_m4_window *w, VkExtent2D extent, int bgra) {
    struct frame_capture *capture = w->capture;
    const uint64_t sequence = ++capture->sequence;
    if (capture->present_count) InterlockedIncrement64(capture->present_count);
    // Moving the last capture aside requests the next *presented* frame,
    // without writing every frame of a long opening movie to disk.
    if (sequence != 1 && sequence % CAPTURE_INTERVAL &&
        (capture->failed || GetFileAttributesW(capture->path) != INVALID_FILE_ATTRIBUTES ||
         GetLastError() != ERROR_FILE_NOT_FOUND)) return;
    if (write_capture(capture, w->mapped, extent, bgra)) {
        capture->failed = 0;
    } else if (!capture->failed) {
        fprintf(stderr, "[m4 window] could not update DM_CAPTURE_DIR/m4-latest.ppm\n");
        capture->failed = 1;
    }
}

VkResult dm_m4_window_create(struct dm_m4_window **out, const wchar_t *title,
                             uint32_t width, uint32_t height,
                             const struct dm_m4_device_requirements *requirements) {
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = NULL;
    // Leave headroom for Win32 to add non-client borders to the signed dimensions.
    if (!width || !height || width > (uint32_t)(INT_MAX / 2) || height > (uint32_t)(INT_MAX / 2) ||
        (requirements && (requirements->extension_count > UINT32_MAX - 2 ||
         (requirements->extension_count && !requirements->extensions)))) return VK_ERROR_INITIALIZATION_FAILED;
    struct dm_m4_window *w = calloc(1, sizeof(*w));
    if (!w) return VK_ERROR_OUT_OF_HOST_MEMORY;
    int maintenance_khr = 0, maintenance_ext = 0;
    VkResult status = create_hwnd(w, title, width, height);
    if (status == VK_SUCCESS) status = create_instance(w, &maintenance_khr, &maintenance_ext);
    if (status == VK_SUCCESS) {
        const VkWin32SurfaceCreateInfoKHR ci = { .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
            .hinstance = GetModuleHandleW(NULL), .hwnd = w->hwnd };
        VkSurfaceKHR surface;
        status = w->vkCreateWin32SurfaceKHR(w->native.instance, &ci, NULL, &surface);
        if (status == VK_SUCCESS) w->surface = surface;
        else vk_error(status, "vkCreateWin32SurfaceKHR");
    }
    if (status == VK_SUCCESS) status = create_device(w, requirements, maintenance_khr, maintenance_ext);
    if (status == VK_SUCCESS) status = create_frame_resources(w);
    if (status != VK_SUCCESS) {
        dm_m4_window_destroy(w);
        return status;
    }
    w->capture = open_capture();
    ShowWindow(w->hwnd, SW_SHOW);
    *out = w;
    return VK_SUCCESS;
}

const struct dm_m4_device *dm_m4_window_get_device(const struct dm_m4_window *w) {
    return &w->native;
}

int dm_m4_window_poll(struct dm_m4_window *w) {
    MSG message;
    // The display wait watches the entire thread queue. Leaving messages for
    // helper HWNDs or the thread itself pending can starve the frame event.
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            w->closed = 1;
            PostQuitMessage((int)message.wParam);
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return !w->closed;
}

VkResult dm_m4_window_present_observe(struct dm_m4_window *w, uint32_t width, uint32_t height,
                                      VkFormat format, dm_m4_fill_frame fill, void *userdata,
                                      dm_m4_accepted_frame accepted, void *observer) {
    if (!dm_m4_window_poll(w)) return VK_ERROR_SURFACE_LOST_KHR;
    if (!fill || !width || !height || width > w->max_dimension || height > w->max_dimension)
        return VK_ERROR_INITIALIZATION_FAILED;
    const size_t row = (size_t)width * 4;
    if (height > SIZE_MAX / row) return VK_ERROR_INITIALIZATION_FAILED;
    const int bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
    if (!bgra && format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    RECT rect;
    if (!GetClientRect(w->hwnd, &rect)) return win32_error("GetClientRect");
    if (IsIconic(w->hwnd) || rect.right <= 0 || rect.bottom <= 0) return VK_NOT_READY;
    const VkExtent2D client = { (uint32_t)rect.right, (uint32_t)rect.bottom };
    const VkExtent2D extent = { width, height };
    VkResult status = wait_frame(w);
    if (status != VK_SUCCESS) return status;
    if (!w->swapchain || w->recreate || !same_extent(w->client, client)) {
        w->recreate = 1;
        status = rebuild_swapchain(w, client);
        if (status != VK_SUCCESS) return status;
    }
    const VkFormat surface = w->surface_format.format;
    const int surface_bgra = surface == VK_FORMAT_B8G8R8A8_UNORM || surface == VK_FORMAT_B8G8R8A8_SRGB;
    const int blit = bgra != surface_bgra || !same_extent(extent, w->extent);
    if (blit) {
        // Tag both images with the same encoding so a blit swaps channels/scales
        // without turning display-encoded source bytes into linear-light values.
        const int srgb = surface == VK_FORMAT_B8G8R8A8_SRGB || surface == VK_FORMAT_R8G8B8A8_SRGB;
        const VkFormat source = bgra ? (srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM) :
                                      (srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
        status = ensure_source(w, extent, source);
        if (status != VK_SUCCESS) return status;
    }
    const size_t bytes = row * height;
    status = ensure_upload(w, bytes);
    if (status != VK_SUCCESS) return status;
    const VkDevice device = w->native.device;
    VK_TRY(w->vkResetFences(device, FENCE_COUNT, w->fences));
    uint32_t index;
    status = w->vkAcquireNextImageKHR(device, w->swapchain, UINT64_C(16666667), w->acquired,
                                      w->fences[ACQUIRE], &index);
    if (status == VK_NOT_READY || status == VK_TIMEOUT) return VK_NOT_READY;
    if (status == VK_ERROR_OUT_OF_DATE_KHR) {
        w->recreate = 1;
        return VK_NOT_READY;
    }
    if (status != VK_SUCCESS && status != VK_SUBOPTIMAL_KHR) return vk_error(status, "vkAcquireNextImageKHR");
    w->pending_fences = ACQUIRE + 1;
    w->recreate = status == VK_SUBOPTIMAL_KHR;

    status = fill(userdata, w->mapped, row);
    if (status != VK_SUCCESS) return status < 0 ? status : VK_ERROR_INITIALIZATION_FAILED;
    if (!(w->upload_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
        const VkMappedMemoryRange flush = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = w->upload_memory, .size = VK_WHOLE_SIZE };
        VK_TRY(w->vkFlushMappedMemoryRanges(device, 1, &flush));
    }
    status = record_frame(w, w->swap_images[index], extent, blit);
    if (status != VK_SUCCESS) return status;
    const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &w->acquired, .pWaitDstStageMask = &wait,
        .commandBufferCount = 1, .pCommandBuffers = &w->command,
        .signalSemaphoreCount = 1, .pSignalSemaphores = &w->transferred };
    VK_TRY(w->vkQueueSubmit(w->native.queue, 1, &submit, w->fences[SUBMIT]));
    w->pending_fences = SUBMIT + 1;
    const VkSwapchainPresentFenceInfoKHR fence = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR,
        .swapchainCount = 1, .pFences = &w->fences[PRESENT] };
    const VkPresentInfoKHR present = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = &fence,
        .waitSemaphoreCount = 1, .pWaitSemaphores = &w->transferred,
        .swapchainCount = 1, .pSwapchains = &w->swapchain, .pImageIndices = &index };
    status = w->vkQueuePresentKHR(w->native.queue, &present);
    // Out-of-date and surface-lost presents still enqueue their queue operations.
    if (status == VK_SUCCESS || status == VK_SUBOPTIMAL_KHR || status == VK_ERROR_OUT_OF_DATE_KHR ||
        status == VK_ERROR_SURFACE_LOST_KHR) w->pending_fences = PRESENT + 1;
    if (status == VK_ERROR_OUT_OF_DATE_KHR || status == VK_SUBOPTIMAL_KHR) w->recreate = 1;
    if (status == VK_ERROR_OUT_OF_DATE_KHR) return VK_NOT_READY;
    if (status == VK_SUBOPTIMAL_KHR || status == VK_SUCCESS) {
        // The present was accepted. Its transfer only reads this mapped buffer,
        // which stays intact until the next frame can reuse it.
        if (w->capture) capture_present(w, extent, bgra);
        if (accepted) accepted(observer, w->mapped, extent, w->extent, bgra);
        return VK_SUCCESS;
    }
    return vk_error(status, "vkQueuePresentKHR");
}

VkResult dm_m4_window_present_fill(struct dm_m4_window *w, uint32_t width, uint32_t height,
                                   VkFormat format, dm_m4_fill_frame fill, void *userdata) {
    return dm_m4_window_present_observe(w, width, height, format, fill, userdata, NULL, NULL);
}

struct linear_frame {
    const void *pixels;
    size_t row_pitch;
    uint32_t height;
};

static VkResult copy_linear_frame(void *userdata, void *pixels, size_t row) {
    const struct linear_frame *frame = userdata;
    if (frame->row_pitch == row) {
        memcpy(pixels, frame->pixels, row * frame->height);
    } else {
        unsigned char *dst = pixels;
        const unsigned char *src = frame->pixels;
        for (uint32_t y = 0; y < frame->height; ++y)
            memcpy(dst + row * y, src + frame->row_pitch * y, row);
    }
    return VK_SUCCESS;
}

VkResult dm_m4_window_present(struct dm_m4_window *w, const void *pixels,
                              uint32_t width, uint32_t height, size_t row_pitch, VkFormat format) {
    const size_t row = (size_t)width * 4;
    if (!pixels || row_pitch < row || (height > 1 && row_pitch > (SIZE_MAX - row) / (height - 1)))
        return VK_ERROR_INITIALIZATION_FAILED;
    struct linear_frame frame = { .pixels = pixels, .row_pitch = row_pitch, .height = height };
    return dm_m4_window_present_fill(w, width, height, format, copy_linear_frame, &frame);
}

void dm_m4_window_destroy(struct dm_m4_window *w) {
    if (!w) return;
    const VkDevice device = w->native.device;
    if (device) {
        if (wait_frame(w) != VK_SUCCESS && w->vkDeviceWaitIdle) w->vkDeviceWaitIdle(device);
        free_source(w);
        free_upload(w);
        if (w->swapchain) w->vkDestroySwapchainKHR(device, w->swapchain, NULL);
        free(w->swap_images);
        if (w->command_pool) w->vkDestroyCommandPool(device, w->command_pool, NULL);
        if (w->transferred) w->vkDestroySemaphore(device, w->transferred, NULL);
        if (w->acquired) w->vkDestroySemaphore(device, w->acquired, NULL);
        for (unsigned i = 0; i < FENCE_COUNT; ++i)
            if (w->fences[i]) w->vkDestroyFence(device, w->fences[i], NULL);
        if (w->vkDestroyDevice) w->vkDestroyDevice(device, NULL);
    }
    if (w->surface) w->vkDestroySurfaceKHR(w->native.instance, w->surface, NULL);
    if (w->native.instance && w->vkDestroyInstance) w->vkDestroyInstance(w->native.instance, NULL);
    if (w->hwnd) DestroyWindow(w->hwnd);
    if (w->loader) FreeLibrary(w->loader);
    if (w->capture && w->capture->present_count) UnmapViewOfFile((const void *)w->capture->present_count);
    free(w->capture);
    free(w);
}
