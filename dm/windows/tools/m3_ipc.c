// Horizon service-manager TIPC. All calls hold the SVC lock.
#include "m2_host.h"
#include <stdio.h>
#include <string.h>

enum {
    IPC_BAD_HANDLE = 0xe401, IPC_BAD_BUFFER = 0xe601, IPC_BAD_MESSAGE = 0xe801,
    IPC_NOT_FOUND = 0xf201, IPC_CLOSED = 0xf601, IPC_NOT_IMPLEMENTED = 0x4201,
    SM_INVALID_CLIENT = 0x415, SM_INVALID_NAME = 0xc15, SM_NOT_REGISTERED = 0xe15,
    HIPC_CLOSE = 2, TIPC_CLOSE = 15, SM_REGISTER_CLIENT = 16, SM_GET_SERVICE = 17,
};
#define HIPC_SPECIAL UINT32_C(0x80000000)

struct sm_session { uint32_t handle; int registered, closed; };
static struct sm_session sessions[64];

static struct sm_session *find_session(uint32_t handle) {
    if (handle) for (size_t i = 0; i < ARRAYSIZE(sessions); ++i)
        if (sessions[i].handle == handle) return &sessions[i];
    return NULL;
}
uint32_t dm_ipc_connect(const char *guest_port, uint32_t *handle, int trace) {
    char port[12];
    *handle = 0;
    if (!dm_guest_string(guest_port, port, sizeof(port))) return IPC_BAD_BUFFER;
    if (strcmp(port, "sm:")) return IPC_NOT_FOUND;
    for (size_t i = 0; i < ARRAYSIZE(sessions); ++i) if (!sessions[i].handle) {
        *handle = dm_alloc_handle();
        if (!*handle) return 0xd201; // Kernel ResultOutOfHandles.
        sessions[i] = (struct sm_session){ .handle = *handle };
        if (trace) printf("[ipc] connected sm: (%#x)\n", *handle);
        return 0;
    }
    return 0xe01; // Kernel ResultOutOfSessions.
}
int dm_ipc_close(uint32_t handle) {
    struct sm_session *s = find_session(handle);
    if (!s) return 0;
    *s = (struct sm_session){0};
    return 1;
}

static size_t service_name(char name[9], const uint32_t words[2]) {
    memcpy(name, words, 8);
    name[8] = '\0';
    size_t n = strlen(name);
    for (size_t i = 0; i < 8; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (i < n ? (c < 0x21 || c > 0x7e) : c != 0) return 0;
    }
    return n;
}

uint32_t dm_ipc_send(uint32_t handle, void *buffer, int trace) {
    struct sm_session *s = find_session(handle);
    if (!s) return IPC_BAD_HANDLE;
    if (s->closed) return IPC_CLOSED;
    if (!dm_guest_range(buffer, DM_HIPC_TLS_BYTES, 1)) return IPC_BAD_BUFFER;
    uint32_t header[4];
    memcpy(header, buffer, sizeof(header));
    if ((header[0] == HIPC_CLOSE || header[0] == TIPC_CLOSE) && header[1] == 0) {
        s->closed = 1;
        return IPC_CLOSED;
    }

    // SM TIPC requests on SDK 22 carry only inline words or a PID descriptor.
    // Require those exact shapes before reading command payload or changing state.
    uint32_t result;
    switch (header[0]) {
    case HIPC_CLOSE: case TIPC_CLOSE:
        return IPC_BAD_MESSAGE; // A close packet has no payload.
    case SM_REGISTER_CLIENT:
        if (header[1] != HIPC_SPECIAL || header[2] != 1) return IPC_BAD_MESSAGE;
        // The kernel attaches the caller identity, not the PID bytes supplied by the guest.
        s->registered = 1;
        result = 0;
        if (trace) printf("[ipc] sm: RegisterClient\n");
        break;
    case SM_GET_SERVICE: {
        if (header[1] != 2) return IPC_BAD_MESSAGE;
        char name[9];
        if (!s->registered) result = SM_INVALID_CLIENT;
        else if (!service_name(name, header + 2)) result = SM_INVALID_NAME;
        else {
            fprintf(stderr, "[RED] sm: service '%s' is not registered\n", name);
            result = SM_NOT_REGISTERED;
        }
        break;
    }
    default:
        fprintf(stderr, "[RED] sm: unsupported IPC header %#x / %#x\n", header[0], header[1]);
        return IPC_NOT_IMPLEMENTED;
    }

    // TIPC response: no descriptors, one raw word containing the SM result.
    // A service error is distinct from SendSyncRequest's kernel return value.
    const uint32_t reply[] = {0, 1, result};
    memset(buffer, 0, DM_HIPC_TLS_BYTES);
    memcpy(buffer, reply, sizeof(reply));
    return 0;
}
