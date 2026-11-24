// Experimental native ARM64 bring-up host, before graphics or SDK service HLE.
#ifndef DM_WINDOWS_M2_HOST_H
#define DM_WINDOWS_M2_HOST_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

#define DM_PAGE 0x1000u
#define DM_MODULE_ROOM 0x10000000u
#define DM_GUEST_LIMIT (UINT64_C(1) << 39)

struct dm_gate_site {
    uint64_t ret, orig;
    unsigned arg;
    char kind[8];
};

struct dm_module {
    const char *name;
    char build_id[33];
    unsigned char *base;
    uint64_t text, ro, data, key, patch, size, fps;
    uint64_t strings, strings_size, symbols, nsyms, init;
    uint64_t rela, rela_size, relr, relr_size, plt, plt_size;
    struct dm_gate_site *gates;
    size_t ngates;
};

_Noreturn void dm_fail(const char *format, ...);
uint64_t dm_guest_time_ns(void);  // Same process-relative QPC epoch as svcGetSystemTick.
void dm_module_load(struct dm_module *m, const char *dir, unsigned char *base, size_t available,
                    const uint64_t keys[5]);
void dm_module_relocate(struct dm_module *m, struct dm_module *modules, size_t n);
void dm_module_seal(const struct dm_module *m);
uintptr_t dm_find_symbol(const struct dm_module *modules, size_t n, const char *name);
uintptr_t dm_boot_import(const char *name);
uintptr_t dm_m3_import(const char *name);
void dm_m3_nvn_load(const struct dm_module *module);
// Lookup a defined executable symbol in the SDK, bypassing native imports.
uintptr_t dm_guest_sdk_symbol(const char *name);
struct dm_guest_result { uint64_t x0, x1; };
// Only on an active gate of a registered guest thread. Hold no locks the
// callback may reacquire. args has eight readable words (zero-fill unused), or is NULL.
// Returns 1 after invocation, or 0 without invoking or modifying result.
int dm_guest_call(uintptr_t entry, const uint64_t args[8], struct dm_guest_result *result);
#define DM_HIPC_TLS_BYTES 0x100u
// IPC entry points and handle allocation run under the kernel SVC lock.
uint32_t dm_alloc_handle(void);
// Duplicate a readable kernel event for a guest service client; acquires the SVC lock.
uint32_t dm_dup_readable_event(uint32_t handle);
// Native service workers can signal or clear an SDK-created event without a guest stack.
uint32_t dm_signal_event(uint32_t writable);
uint32_t dm_clear_event(uint32_t writable);
void *dm_service_alloc(size_t size);
void dm_service_free(void *ptr, size_t size);
uint32_t dm_ipc_connect(const char *guest_port, uint32_t *handle, int trace);
int dm_ipc_close(uint32_t handle);
uint32_t dm_ipc_send(uint32_t handle, void *buffer, int trace);
int dm_guest_range(const void *ptr, size_t size, int write);
// Positive access proofs may be reused at the same epoch, for the same span
// and permissions. GPU callers also hold the replay lock through submission.
uint64_t dm_guest_access_generation(void);
// Bracket protection/decommit with replay exclusion. The begin waits for
// outstanding draws and releases any Vulkan imports that borrow this range.
void dm_guest_gpu_unmap_begin(const void *base, size_t size);
void dm_guest_gpu_unmap_end(void);
int dm_guest_string(const char *input, char *out, size_t capacity);
const struct dm_gate_site *dm_find_gate(const struct dm_module *m, uint64_t ret);
int dm_module_region(const struct dm_module *m, uint64_t addr, uint64_t *base, uint64_t *size,
                     uint32_t *state, uint32_t *perm);
#endif
