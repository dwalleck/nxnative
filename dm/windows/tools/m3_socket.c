// Horizon BSD descriptors used by the SDK's offline networking threads.
#include "m2_host.h"
#include <string.h>
#include <stdlib.h>

// Horizon uses Linux errno numbers with BSD socket structures and constants.
enum {
    SK_EBADF = 9, SK_EAGAIN = 11, SK_EFAULT = 14, SK_EINVAL = 22, SK_EMFILE = 24, SK_EPIPE = 32,
    SK_ENOTSOCK = 88, SK_EDESTADDRREQ = 89, SK_EMSGSIZE = 90, SK_ENOPROTOOPT = 92,
    SK_EPROTONOSUPPORT = 93, SK_EOPNOTSUPP = 95, SK_EAFNOSUPPORT = 97, SK_EADDRINUSE = 98,
    SK_ENETDOWN = 100, SK_ENOBUFS = 105, SK_EISCONN = 106, SK_ENOTCONN = 107,
    SK_POLLIN = 0x01, SK_POLLOUT = 0x04, SK_POLLNVAL = 0x20, SK_POLLRDNORM = 0x40,
    SK_EFD_SEMAPHORE = 1, SK_EFD_CLOEXEC = 2, SK_EFD_NONBLOCK = 4,
    SK_FD_CLOEXEC = 1, SK_O_NONBLOCK = 0x800,
    SK_BAD_STATE = 0x41b, SK_BAD_CONFIG = 0x61b,
    // Socket APIs own these descriptors. Ocarina's plain libc read/close calls
    // only use its SDK file descriptors, so those entry points stay in the SDK.
    SK_FD_LIMIT = 1024, SK_FD_COUNT = 64,
    SK_AF_INET = 2, SK_AF_INET6 = 28, SK_STREAM = 1, SK_DGRAM = 2, SK_UDP = 17,
    SK_MSG_PEEK = 2, SK_MSG_DONTROUTE = 4, SK_MSG_TRUNC = 0x10,
    SK_MSG_DONTWAIT = 0x80, SK_MSG_NOSIGNAL = 0x20000,
    SK_MAX_DATAGRAM = 65507, SK_QUEUE_BYTES = 256 * 1024, SK_QUEUE_PACKETS = 256,
};

struct socket_config_prefix {
    uint64_t version_flags;
    uintptr_t work;
    uint64_t work_size, minimum_size;
};
struct socket_pollfd { int32_t fd; int16_t events, revents; };
struct socket_timeval { int64_t seconds, microseconds; };
_Static_assert(sizeof(struct socket_pollfd) == 8, "Horizon pollfd ABI");

struct socket_address { uint8_t ip[16]; uint16_t port; int family; };
struct socket_packet {
    struct socket_packet *next;
    struct socket_address source;
    size_t size;
};

struct descriptor {
    uint64_t generation, value;
    int flags, fdflags;
    struct socket_address local, peer;
    struct socket_packet *head, *tail;
    size_t queued, receive_limit;
    unsigned packets;
    uint64_t receive_timeout;
    int shut_read, shut_write, reuse, no_sigpipe;
};
static struct descriptor descriptors[SK_FD_COUNT];
static SRWLOCK socket_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE socket_changed = CONDITION_VARIABLE_INIT;
static uint64_t generation, lifecycle;
static uint16_t ephemeral_port = 49152;
static int initialized;

static INIT_ONCE sdk_once = INIT_ONCE_STATIC_INIT;
static uintptr_t sdk_errno;
static __declspec(thread) int fixture_errno;

static BOOL CALLBACK socket_load_sdk(PINIT_ONCE once, PVOID param, PVOID *context) {
    (void)once; (void)param; (void)context;
    sdk_errno = dm_guest_sdk_symbol("__errno_location");
    return TRUE;
}

// The guest libc and nnsocket* use the same errno slot. Do not call guest code
// under socket_lock; SDK calls can reenter the host through the normal gates.
static int *socket_errno_location(void) {
    InitOnceExecuteOnce(&sdk_once, socket_load_sdk, NULL, NULL);
    if (!sdk_errno) return &fixture_errno;
    struct dm_guest_result result;
    if (!dm_guest_call(sdk_errno, NULL, &result)) dm_fail("socket: cannot enter SDK errno accessor");
    return (int *)(uintptr_t)result.x0;
}

static int socket_error(int error) {
    *socket_errno_location() = error;
    return -1;
}

static struct descriptor *descriptor_locked(int fd) {
    if (fd < 0 || fd >= SK_FD_COUNT || !descriptors[fd].generation) return NULL;
    return &descriptors[fd];
}

static int allocate_locked(void) {
    if (!initialized) return -SK_EINVAL;
    for (int fd = 0; fd < SK_FD_COUNT; ++fd) {
        struct descriptor *d = &descriptors[fd];
        if (d->generation) continue;
        *d = (struct descriptor){0};
        d->generation = ++generation;
        return fd;
    }
    return -SK_EMFILE;
}

static void clear_locked(struct descriptor *d) {
    struct socket_packet *packet = d->head;
    while (packet) {
        struct socket_packet *next = packet->next;
        free(packet);
        packet = next;
    }
    d->head = NULL;
    d->generation = 0;
}

uint32_t socket_initialize(const void *config) {
    struct socket_config_prefix cfg;
    AcquireSRWLockExclusive(&socket_lock);
    uint32_t result;
    if (initialized) result = SK_BAD_STATE;
    else if (!dm_guest_range(config, sizeof(cfg), 0)) result = SK_BAD_CONFIG;
    else {
        memcpy(&cfg, config, sizeof(cfg));
        if (!cfg.work || (cfg.work & (DM_PAGE - 1)) || cfg.work_size < DM_PAGE ||
            (cfg.work_size & (DM_PAGE - 1)) || cfg.work_size < cfg.minimum_size ||
            !dm_guest_range((void *)cfg.work, cfg.work_size, 1)) result = SK_BAD_CONFIG;
        else { initialized = 1; result = 0; }
    }
    ReleaseSRWLockExclusive(&socket_lock);
    return result;
}

int socket_is_initialized(void) {
    AcquireSRWLockShared(&socket_lock);
    int result = initialized;
    ReleaseSRWLockShared(&socket_lock);
    return result;
}

uint32_t socket_finalize(void) {
    AcquireSRWLockExclusive(&socket_lock);
    uint32_t result = SK_BAD_STATE;
    if (initialized) {
        initialized = 0;
        ++lifecycle;
        for (size_t i = 0; i < ARRAYSIZE(descriptors); ++i) clear_locked(&descriptors[i]);
        WakeAllConditionVariable(&socket_changed);
        result = 0;
    }
    ReleaseSRWLockExclusive(&socket_lock);
    return result;
}

int socket_get_errno(void) {
    return socket_is_initialized() ? *socket_errno_location() : SK_EINVAL;
}

void socket_set_errno(int error) {
    if (socket_is_initialized()) *socket_errno_location() = error;
}

int socket_create(int family, int type, int protocol) {
    if (family != SK_AF_INET && family != SK_AF_INET6) return socket_error(SK_EAFNOSUPPORT);
    // The offline host has no stream transport; don't issue dummy descriptors.
    if (type == SK_STREAM && (!protocol || protocol == 6)) return socket_error(SK_ENETDOWN);
    if (type != SK_DGRAM || (protocol && protocol != SK_UDP)) return socket_error(SK_EPROTONOSUPPORT);
    AcquireSRWLockExclusive(&socket_lock);
    int fd = allocate_locked();
    if (fd >= 0) {
        descriptors[fd].local.family = family;
        descriptors[fd].receive_limit = SK_QUEUE_BYTES;
    }
    ReleaseSRWLockExclusive(&socket_lock);
    return fd < 0 ? socket_error(-fd) : fd;
}

int socket_eventfd(uint64_t value, int flags) {
    if (value == UINT64_MAX || (flags & ~(SK_EFD_SEMAPHORE | SK_EFD_CLOEXEC | SK_EFD_NONBLOCK)))
        return socket_error(SK_EINVAL);
    AcquireSRWLockExclusive(&socket_lock);
    int fd = allocate_locked();
    if (fd >= 0) {
        struct descriptor *d = &descriptors[fd];
        d->value = value;
        d->flags = flags & ~SK_EFD_CLOEXEC;
        d->fdflags = (flags & SK_EFD_CLOEXEC) ? SK_FD_CLOEXEC : 0;
    }
    ReleaseSRWLockExclusive(&socket_lock);
    return fd < 0 ? socket_error(-fd) : fd;
}

int socket_close(int fd) {
    AcquireSRWLockExclusive(&socket_lock);
    struct descriptor *d = descriptor_locked(fd);
    if (d) {
        clear_locked(d);
        WakeAllConditionVariable(&socket_changed);
    }
    ReleaseSRWLockExclusive(&socket_lock);
    return d ? 0 : socket_error(SK_EBADF);
}

static int event_transfer(int fd, uint64_t *value, int write) {
    AcquireSRWLockExclusive(&socket_lock);
    struct descriptor *d = descriptor_locked(fd);
    int error = SK_EBADF;
    if (d && d->local.family) { error = SK_ENOTSOCK; d = NULL; }
    if (d) {
        uint64_t identity = d->generation;
        do {
            if (write && *value < UINT64_MAX - d->value) {
                d->value += *value;
                if (*value) WakeAllConditionVariable(&socket_changed);
                error = 0;
                break;
            }
            if (!write && d->value) {
                *value = (d->flags & SK_EFD_SEMAPHORE) ? 1 : d->value;
                d->value -= *value;
                WakeAllConditionVariable(&socket_changed);
                error = 0;
                break;
            }
            if (d->flags & SK_EFD_NONBLOCK) { error = SK_EAGAIN; break; }
            if (!SleepConditionVariableSRW(&socket_changed, &socket_lock, INFINITE, 0))
                dm_fail("socket: event wait failed (%lu)", GetLastError());
        } while (d->generation == identity);
    }
    ReleaseSRWLockExclusive(&socket_lock);
    return error ? socket_error(error) : 0;
}

int socket_eventfd_read(int fd, void *output) {
    if (!dm_guest_range(output, sizeof(uint64_t), 1)) return socket_error(SK_EFAULT);
    uint64_t value;
    if (event_transfer(fd, &value, 0)) return -1;
    memcpy(output, &value, sizeof(value));
    return 0;
}

int socket_eventfd_write(int fd, uint64_t value) {
    if (value == UINT64_MAX) return socket_error(SK_EINVAL);
    return event_transfer(fd, &value, 1);
}

int socket_fcntl(int fd, int command, int value) {
    AcquireSRWLockExclusive(&socket_lock);
    struct descriptor *d = descriptor_locked(fd);
    int result = 0, error = 0;
    if (!d) error = SK_EBADF;
    else switch (command) {
    case 1: result = d->fdflags; break;                                      // F_GETFD
    case 2: d->fdflags = value & SK_FD_CLOEXEC; break;                        // F_SETFD
    case 3: result = (d->flags & SK_EFD_NONBLOCK ? SK_O_NONBLOCK : 0) | 2; break; // F_GETFL, O_RDWR
    case 4:                                                                // F_SETFL
        d->flags = (d->flags & ~SK_EFD_NONBLOCK) | (value & SK_O_NONBLOCK ? SK_EFD_NONBLOCK : 0);
        WakeAllConditionVariable(&socket_changed);
        break;
    default: error = SK_EINVAL; break;
    }
    ReleaseSRWLockExclusive(&socket_lock);
    return error ? socket_error(error) : result;
}


static int16_t descriptor_ready_locked(int fd, int16_t events, uint64_t snapshot) {
    if (fd < 0) return 0;
    const struct descriptor *d = descriptor_locked(fd);
    if (!d || d->generation > snapshot) return SK_POLLNVAL;
    int ready;
    if (d->local.family) {
        ready = d->head || d->shut_read ? SK_POLLIN | SK_POLLRDNORM : 0;
        if (!d->shut_write) ready |= SK_POLLOUT;
    } else {
        ready = d->value ? SK_POLLIN | SK_POLLRDNORM : 0;
        if (d->value < UINT64_MAX - 1) ready |= SK_POLLOUT;
    }
    return (int16_t)(ready & events);
}

// Called with the lock held. The generation snapshot distinguishes a
// descriptor that closed while blocked from a new one which reused its slot.
static int poll_locked(struct socket_pollfd *fds, size_t count, uint64_t timeout) {
    uint64_t snapshot = generation, session = lifecycle;
    uint64_t started = timeout && timeout != UINT64_MAX ? GetTickCount64() : 0;
    for (;;) {
        int ready = 0;
        for (size_t i = 0; i < count; ++i) {
            fds[i].revents = session == lifecycle ? descriptor_ready_locked(fds[i].fd, fds[i].events, snapshot) :
                             (fds[i].fd < 0 ? 0 : SK_POLLNVAL);
            ready += fds[i].revents != 0;
        }
        if (ready) return ready;
        if (session != lifecycle) return -1;
        if (!timeout) return 0;
        DWORD wait_ms = INFINITE;
        if (timeout != UINT64_MAX) {
            uint64_t elapsed = GetTickCount64() - started;
            if (elapsed >= timeout) return 0;
            uint64_t remaining = timeout - elapsed;
            wait_ms = remaining >= INFINITE ? INFINITE - 1 : (DWORD)remaining;
        }
        if (!SleepConditionVariableSRW(&socket_changed, &socket_lock, wait_ms, 0)) {
            DWORD error = GetLastError();
            if (error != ERROR_TIMEOUT) dm_fail("socket: poll wait failed (%lu)", error);
            if (wait_ms < INFINITE - 1) timeout = 0; // Rescan at the deadline before returning.
        }
    }
}

int socket_poll(struct socket_pollfd *fds, uint64_t count, int timeout_ms) {
    if (count > SK_FD_LIMIT) return socket_error(SK_EINVAL);
    if (count && !dm_guest_range(fds, count * sizeof(*fds), 1)) return socket_error(SK_EFAULT);
    AcquireSRWLockExclusive(&socket_lock);
    int result = initialized ? poll_locked(fds, count, timeout_ms < 0 ? UINT64_MAX : (uint64_t)timeout_ms) : -1;
    ReleaseSRWLockExclusive(&socket_lock);
    return result < 0 ? socket_error(SK_EINVAL) : result;
}

int socket_select(int count, unsigned char *read_fds, unsigned char *write_fds,
                  unsigned char *except_fds, const struct socket_timeval *tv) {
    if (count < 0 || count > SK_FD_LIMIT) return socket_error(SK_EINVAL);
    uint64_t timeout = UINT64_MAX;
    if (tv) {
        struct socket_timeval t;
        if (!dm_guest_range(tv, sizeof(t), 0)) return socket_error(SK_EFAULT);
        memcpy(&t, tv, sizeof(t));
        if (t.seconds < 0 || t.microseconds < 0 || t.microseconds >= 1000000) return socket_error(SK_EINVAL);
        timeout = (uint64_t)t.seconds > (UINT64_MAX - 1000) / 1000 ? UINT64_MAX - 1 :
                  (uint64_t)t.seconds * 1000 + ((uint64_t)t.microseconds + 999) / 1000;
    }
    unsigned char *sets[] = {read_fds, write_fds, except_fds};
    size_t bytes = ((size_t)count + 7) / 8;
    for (size_t i = 0; i < ARRAYSIZE(sets); ++i)
        if (sets[i] && bytes && !dm_guest_range(sets[i], bytes, 1)) return socket_error(SK_EFAULT);
    struct socket_pollfd fds[SK_FD_COUNT];
    size_t used = 0;
    AcquireSRWLockExclusive(&socket_lock);
    int error = initialized ? 0 : SK_EINVAL;
    for (int fd = 0; !error && fd < count; ++fd) {
        unsigned bit = 1u << (fd % 8);
        int r = read_fds && (read_fds[fd / 8] & bit);
        int w = write_fds && (write_fds[fd / 8] & bit);
        int e = except_fds && (except_fds[fd / 8] & bit);
        if (!r && !w && !e) continue;
        if (!descriptor_locked(fd)) error = SK_EBADF;
        else fds[used++] = (struct socket_pollfd){fd, (int16_t)((r ? SK_POLLIN : 0) | (w ? SK_POLLOUT : 0)), 0};
    }
    if (!error && poll_locked(fds, used, timeout) < 0) error = SK_EINVAL;
    int ready = 0;
    for (size_t i = 0; !error && i < used; ++i) if (fds[i].revents & SK_POLLNVAL) error = SK_EBADF;
    if (!error) {
        for (size_t i = 0; i < ARRAYSIZE(sets); ++i) if (sets[i]) memset(sets[i], 0, bytes);
        for (size_t i = 0; i < used; ++i) {
            unsigned char bit = (unsigned char)(1u << (fds[i].fd % 8));
            if (read_fds && (fds[i].revents & SK_POLLIN)) { read_fds[fds[i].fd / 8] |= bit; ++ready; }
            if (write_fds && (fds[i].revents & SK_POLLOUT)) { write_fds[fds[i].fd / 8] |= bit; ++ready; }
        }
    }
    ReleaseSRWLockExclusive(&socket_lock);
    return error ? socket_error(error) : ready;
}

// BSD sockaddr carries a length byte before the family and uses network-order
// port/address bytes. Only loopback (and the wildcard for bind) exists here.
static int wildcard(const struct socket_address *address) {
    static const uint8_t zero[16] = {0};
    return !memcmp(address->ip, zero, sizeof(zero));
}

static void use_loopback(struct socket_address *address) {
    if (address->family == SK_AF_INET) {
        address->ip[0] = 127;
        address->ip[3] = 1;
    } else address->ip[15] = 1;
}

static int same_ip(const struct socket_address *a, const struct socket_address *b) {
    return a->family == b->family && !memcmp(a->ip, b->ip, sizeof(a->ip));
}

static int read_address(struct socket_address *out, const void *ptr, uint32_t length, int allow_unspec) {
    if (length < 2) return SK_EINVAL;
    if (!dm_guest_range(ptr, 2, 0)) return SK_EFAULT;
    const uint8_t *bytes = ptr;
    *out = (struct socket_address){0};
    out->family = bytes[1];
    if (!out->family && allow_unspec) return 0;
    uint32_t need;
    if (out->family == SK_AF_INET) need = 16;
    else if (out->family == SK_AF_INET6) need = 28;
    else return SK_EAFNOSUPPORT;
    if (length < need || (bytes[0] && bytes[0] != need)) return SK_EINVAL;
    if (!dm_guest_range(ptr, need, 0)) return SK_EFAULT;
    out->port = (uint16_t)((unsigned)bytes[2] << 8 | bytes[3]);
    memcpy(out->ip, bytes + (out->family == SK_AF_INET ? 4 : 8), out->family == SK_AF_INET ? 4u : 16u);
    if (wildcard(out)) return 0;
    if (out->family == SK_AF_INET) return out->ip[0] == 127 ? 0 : SK_ENETDOWN;
    struct socket_address loopback = {.family = SK_AF_INET6};
    use_loopback(&loopback);
    return same_ip(out, &loopback) ? 0 : SK_ENETDOWN;
}

static uint32_t write_address(void *ptr, uint32_t capacity, const struct socket_address *addr) {
    uint8_t bytes[28] = {0};
    uint32_t length = addr->family == SK_AF_INET ? 16u : 28u;
    bytes[0] = (uint8_t)length;
    bytes[1] = (uint8_t)addr->family;
    bytes[2] = (uint8_t)(addr->port >> 8);
    bytes[3] = (uint8_t)addr->port;
    memcpy(bytes + (addr->family == SK_AF_INET ? 4 : 8), addr->ip, addr->family == SK_AF_INET ? 4u : 16u);
    if (capacity) memcpy(ptr, bytes, capacity < length ? capacity : length);
    return length;
}

static int udp_error(const struct descriptor *d) {
    return !d ? SK_EBADF : (d->local.family ? 0 : SK_ENOTSOCK);
}

static int port_available_locked(const struct descriptor *d, const struct socket_address *addr) {
    for (size_t i = 0; i < ARRAYSIZE(descriptors); ++i) {
        const struct descriptor *other = &descriptors[i];
        if (!other->generation || other == d || other->local.family != addr->family ||
            other->local.port != addr->port) continue;
        int specific_match = same_ip(addr, &other->local);
        if (!specific_match && !wildcard(addr) && !wildcard(&other->local)) continue;
        int shared = d->reuse & other->reuse;
        if (!(shared & 0x200) && !((shared & 4) && !specific_match)) return 0;
    }
    return 1;
}

static int bind_locked(struct descriptor *d, struct socket_address address) {
    if (d->local.family != address.family) return SK_EAFNOSUPPORT;
    if (d->local.port) return SK_EINVAL;
    if (address.port) {
        if (!port_available_locked(d, &address)) return SK_EADDRINUSE;
    } else {
        do {
            address.port = ephemeral_port;
            ephemeral_port = ephemeral_port == UINT16_MAX ? 49152 : (uint16_t)(ephemeral_port + 1);
        } while (!port_available_locked(d, &address));
    }
    d->local = address;
    return 0;
}

int socket_bind(int fd, const void *ptr, uint32_t length) {
    struct socket_address addr;
    int error = read_address(&addr, ptr, length, 0);
    if (error) return socket_error(error);
    AcquireSRWLockExclusive(&socket_lock);
    struct descriptor *d = descriptor_locked(fd);
    error = udp_error(d);
    if (!error) error = bind_locked(d, addr);
    ReleaseSRWLockExclusive(&socket_lock);
    return error ? socket_error(error) : 0;
}

int socket_connect(int fd, const void *ptr, uint32_t length) {
    struct socket_address addr;
    int error = read_address(&addr, ptr, length, 1);
    if (error) return socket_error(error);
    AcquireSRWLockExclusive(&socket_lock);
    struct descriptor *d = descriptor_locked(fd);
    error = udp_error(d);
    if (!error && addr.family) {
        if (d->local.family != addr.family) error = SK_EAFNOSUPPORT;
        else if (!addr.port) error = SK_EDESTADDRREQ;
        else if (!d->local.port) error = bind_locked(d, d->local);
        if (!error && wildcard(&addr)) use_loopback(&addr);
    }
    if (!error) d->peer = addr;
    ReleaseSRWLockExclusive(&socket_lock);
    return error ? socket_error(error) : 0;
}

static int socket_name(int fd, void *ptr, uint32_t *length, int peer) {
    if (!dm_guest_range(length, sizeof(*length), 1)) return socket_error(SK_EFAULT);
    uint32_t capacity = *length;
    if (capacity && !dm_guest_range(ptr, capacity < 28 ? capacity : 28, 1)) return socket_error(SK_EFAULT);
    AcquireSRWLockShared(&socket_lock);
    struct descriptor *d = descriptor_locked(fd);
    int error = udp_error(d);
    if (!error) {
        struct socket_address addr = peer ? d->peer : d->local;
        if (!addr.family) error = SK_ENOTCONN;
        else {
            if (!peer && d->peer.family && wildcard(&addr)) use_loopback(&addr);
            *length = write_address(ptr, capacity, &addr);
        }
    }
    ReleaseSRWLockShared(&socket_lock);
    return error ? socket_error(error) : 0;
}

int socket_getsockname(int fd, void *ptr, uint32_t *length) { return socket_name(fd, ptr, length, 0); }
int socket_getpeername(int fd, void *ptr, uint32_t *length) { return socket_name(fd, ptr, length, 1); }

int socket_shutdown(int fd, int how) {
    if (how < 0 || how > 2) return socket_error(SK_EINVAL);
    AcquireSRWLockExclusive(&socket_lock);
    struct descriptor *d = descriptor_locked(fd);
    int error = udp_error(d);
    if (!error && !d->peer.family) error = SK_ENOTCONN;
    if (!error) {
        if (how != 1) d->shut_read = 1;
        if (how != 0) d->shut_write = 1;
        WakeAllConditionVariable(&socket_changed);
    }
    ReleaseSRWLockExclusive(&socket_lock);
    return error ? socket_error(error) : 0;
}

static int unsupported_stream_op(int fd) {
    AcquireSRWLockShared(&socket_lock);
    int error = udp_error(descriptor_locked(fd));
    ReleaseSRWLockShared(&socket_lock);
    return socket_error(error ? error : SK_EOPNOTSUPP);
}

int socket_listen(int fd, int backlog) { (void)backlog; return unsupported_stream_op(fd); }
int socket_accept(int fd, void *ptr, uint32_t *length) {
    (void)ptr; (void)length;
    return unsupported_stream_op(fd);
}

int socket_setsockopt(int fd, int level, int option, const void *ptr, uint32_t length) {
    size_t need = option == 0x1006 ? sizeof(struct socket_timeval) : sizeof(int);
    if (length < need) return socket_error(SK_EINVAL);
    if (!dm_guest_range(ptr, need, 0)) return socket_error(SK_EFAULT);
    AcquireSRWLockExclusive(&socket_lock);
    struct descriptor *d = descriptor_locked(fd);
    int error = udp_error(d);
    if (!error && level != 0xffff) error = SK_ENOPROTOOPT;
    if (!error && option == 0x1006) { // SO_RCVTIMEO
        struct socket_timeval t;
        memcpy(&t, ptr, sizeof(t));
        if (t.seconds < 0 || t.seconds > INT32_MAX || t.microseconds < 0 || t.microseconds >= 1000000)
            error = SK_EINVAL;
        else d->receive_timeout = (uint64_t)t.seconds * 1000000 + (uint64_t)t.microseconds;
    } else if (!error) {
        int value;
        memcpy(&value, ptr, sizeof(value));
        switch (option) {
        case 4: case 0x200: d->reuse = (d->reuse & ~option) | (value ? option : 0); break;
        case 0x800: d->no_sigpipe = value != 0; break;
        case 0x1002: // SO_RCVBUF (bounded per process-local socket)
            if (value <= 0) error = SK_EINVAL;
            else d->receive_limit = (size_t)(value > SK_QUEUE_BYTES ? SK_QUEUE_BYTES : value);
            break;
        default: error = SK_ENOPROTOOPT; break;
        }
    }
    ReleaseSRWLockExclusive(&socket_lock);
    return error ? socket_error(error) : 0;
}

int socket_getsockopt(int fd, int level, int option, void *ptr, uint32_t *length) {
    size_t need = option == 0x1006 ? sizeof(struct socket_timeval) : sizeof(int);
    if (!dm_guest_range(length, sizeof(*length), 1)) return socket_error(SK_EFAULT);
    if (*length < need) return socket_error(SK_EINVAL);
    if (!dm_guest_range(ptr, need, 1)) return socket_error(SK_EFAULT);
    AcquireSRWLockShared(&socket_lock);
    struct descriptor *d = descriptor_locked(fd);
    int error = udp_error(d), value = 0;
    if (!error && level != 0xffff) error = SK_ENOPROTOOPT;
    if (!error && option == 0x1006) {
        struct socket_timeval t = {(int64_t)(d->receive_timeout / 1000000), (int64_t)(d->receive_timeout % 1000000)};
        memcpy(ptr, &t, sizeof(t));
    } else if (!error) {
        switch (option) {
        case 4: case 0x200: value = (d->reuse & option) != 0; break;
        case 0x800: value = d->no_sigpipe; break;
        case 0x1002: value = (int)d->receive_limit; break;
        case 0x1007: value = 0; break; // SO_ERROR: no deferred transport errors
        case 0x1008: value = SK_DGRAM; break;
        case 0x1016: value = SK_UDP; break;
        default: error = SK_ENOPROTOOPT; break;
        }
        if (!error) memcpy(ptr, &value, sizeof(value));
    }
    if (!error) *length = (uint32_t)need;
    ReleaseSRWLockShared(&socket_lock);
    return error ? socket_error(error) : 0;
}

struct socket_iov { void *data; size_t size; };
struct socket_message {
    void *name;
    uint32_t name_length;
    struct socket_iov *iov;
    int iov_count;
    void *control;
    uint32_t control_length;
    int flags;
};
_Static_assert(sizeof(struct socket_message) == 48, "Horizon msghdr ABI");

static int validate_iov(const struct socket_iov *iov, int count, int writing, size_t *total) {
    *total = 0;
    for (int i = 0; i < count; ++i) {
        size_t n = iov[i].size;
        if (n > (size_t)INT64_MAX - *total) return SK_EINVAL;
        if (n && !dm_guest_range(iov[i].data, n, writing)) return SK_EFAULT;
        *total += n;
    }
    return 0;
}

static struct descriptor *udp_destination_locked(const struct socket_address *dest,
                                                const struct socket_address *source) {
    struct descriptor *best = NULL;
    int best_score = -1;
    for (size_t i = 0; i < ARRAYSIZE(descriptors); ++i) {
        struct descriptor *d = &descriptors[i];
        if (!d->generation || d->shut_read || d->local.port != dest->port || d->local.family != dest->family)
            continue;
        int ip_match = same_ip(&d->local, dest);
        if (!ip_match && !wildcard(&d->local)) continue;
        if (d->peer.family && (d->peer.port != source->port || !same_ip(&d->peer, source))) continue;
        int score = ip_match + 2 * (d->peer.family != 0);
        if (score > best_score) { best = d; best_score = score; }
    }
    return best;
}

static int64_t udp_send(int fd, const struct socket_iov *iov, int count, int flags,
                        const void *address, uint32_t address_size) {
    if (flags & ~(SK_MSG_DONTWAIT | SK_MSG_DONTROUTE | SK_MSG_NOSIGNAL)) return socket_error(SK_EOPNOTSUPP);
    struct socket_address dest = {0};
    size_t size;
    int error = validate_iov(iov, count, 0, &size);
    if (error) return socket_error(error);
    if (size > SK_MAX_DATAGRAM) return socket_error(SK_EMSGSIZE);
    if (address && (error = read_address(&dest, address, address_size, 0))) return socket_error(error);

    AcquireSRWLockExclusive(&socket_lock);
    struct descriptor *d = descriptor_locked(fd);
    error = udp_error(d);
    if (!error && d->shut_write) error = SK_EPIPE;
    if (!error && !address) {
        dest = d->peer;
        if (!dest.family) error = SK_EDESTADDRREQ;
    } else if (!error && d->peer.family) error = SK_EISCONN;
    if (!error && dest.family != d->local.family) error = SK_EAFNOSUPPORT;
    if (!error && !dest.port) error = SK_EDESTADDRREQ;
    if (!error && !d->local.port) error = bind_locked(d, d->local);
    if (!error) {
        if (wildcard(&dest)) use_loopback(&dest);
        struct socket_address source = d->local;
        if (wildcard(&source)) use_loopback(&source);
        struct descriptor *receiver = udp_destination_locked(&dest, &source);
        // UDP sends are not acknowledged: no local receiver or a full receive
        // queue drops the datagram without changing the send result.
        if (receiver && receiver->packets < SK_QUEUE_PACKETS && size <= receiver->receive_limit &&
            receiver->queued <= receiver->receive_limit - size) {
            struct socket_packet *p = malloc(sizeof(*p) + size);
            if (!p) error = SK_ENOBUFS;
            else {
                *p = (struct socket_packet){NULL, source, size};
                uint8_t *bytes = (uint8_t *)(p + 1);
                for (int i = 0; i < count; ++i) if (iov[i].size) {
                    memcpy(bytes, iov[i].data, iov[i].size);
                    bytes += iov[i].size;
                }
                if (receiver->tail) receiver->tail->next = p;
                else receiver->head = p;
                receiver->tail = p;
                receiver->queued += size;
                ++receiver->packets;
                WakeAllConditionVariable(&socket_changed);
            }
        }
    }
    ReleaseSRWLockExclusive(&socket_lock);
    return error ? socket_error(error) : (int64_t)size;
}

static int64_t udp_recv(int fd, const struct socket_iov *iov, int count, int flags,
                        void *address, uint32_t *address_size, int *message_flags) {
    if (flags & ~(SK_MSG_DONTWAIT | SK_MSG_PEEK | SK_MSG_TRUNC | 0x40)) return socket_error(SK_EOPNOTSUPP);
    size_t capacity;
    int error = validate_iov(iov, count, 1, &capacity);
    if (error) return socket_error(error);
    if (address && *address_size && !dm_guest_range(address, *address_size < 28 ? *address_size : 28, 1))
        return socket_error(SK_EFAULT);
    AcquireSRWLockExclusive(&socket_lock);
    struct descriptor *d = descriptor_locked(fd);
    error = udp_error(d);
    size_t copied = 0;
    if (!error) {
        uint64_t identity = d->generation;
        uint64_t timeout = (d->receive_timeout + 999) / 1000;
        uint64_t started = timeout ? GetTickCount64() : 0;
        while (!d->head && !d->shut_read) {
            if ((flags & SK_MSG_DONTWAIT) || (d->flags & SK_EFD_NONBLOCK)) { error = SK_EAGAIN; break; }
            DWORD wait_ms = INFINITE;
            if (timeout) {
                uint64_t elapsed = GetTickCount64() - started;
                if (elapsed >= timeout) { error = SK_EAGAIN; break; }
                uint64_t remaining = timeout - elapsed;
                wait_ms = remaining >= INFINITE ? INFINITE - 1 : (DWORD)remaining;
            }
            if (!SleepConditionVariableSRW(&socket_changed, &socket_lock, wait_ms, 0)) {
                DWORD wait_error = GetLastError();
                if (wait_error != ERROR_TIMEOUT) dm_fail("socket: receive wait failed (%lu)", wait_error);
            }
            if (d->generation != identity) { error = SK_EBADF; break; }
        }
        if (!error && d->head) {
            struct socket_packet *p = d->head;
            size_t remaining = p->size < capacity ? p->size : capacity;
            const uint8_t *bytes = (const uint8_t *)(p + 1);
            for (int i = 0; i < count && remaining; ++i) {
                size_t n = iov[i].size < remaining ? iov[i].size : remaining;
                if (!n) continue;
                memcpy(iov[i].data, bytes, n);
                bytes += n;
                remaining -= n;
            }
            if (message_flags && p->size > capacity) *message_flags = SK_MSG_TRUNC;
            if (address) *address_size = write_address(address, *address_size, &p->source);
            copied = p->size < capacity || (flags & SK_MSG_TRUNC) ? p->size : capacity;
            if (!(flags & SK_MSG_PEEK)) {
                d->head = p->next;
                if (!d->head) d->tail = NULL;
                d->queued -= p->size;
                --d->packets;
                free(p);
            }
        }
    }
    ReleaseSRWLockExclusive(&socket_lock);
    return error ? socket_error(error) : (int64_t)copied;
}

int64_t socket_send(int fd, const void *data, size_t size, int flags) {
    const struct socket_iov iov = {(void *)data, size};
    return udp_send(fd, &iov, 1, flags, NULL, 0);
}

int64_t socket_recv(int fd, void *data, size_t size, int flags) {
    const struct socket_iov iov = {data, size};
    return udp_recv(fd, &iov, 1, flags, NULL, NULL, NULL);
}

static int descriptor_is_udp(int fd) {
    AcquireSRWLockShared(&socket_lock);
    struct descriptor *d = descriptor_locked(fd);
    int udp = d && d->local.family;
    ReleaseSRWLockShared(&socket_lock);
    return udp;
}

int64_t socket_read(int fd, void *data, size_t size) {
    if (descriptor_is_udp(fd)) return socket_recv(fd, data, size, 0);
    if (size < sizeof(uint64_t)) return socket_error(SK_EINVAL);
    return socket_eventfd_read(fd, data) ? -1 : 8;
}

int64_t socket_write(int fd, const void *data, size_t size) {
    if (descriptor_is_udp(fd)) return socket_send(fd, data, size, 0);
    if (size < sizeof(uint64_t)) return socket_error(SK_EINVAL);
    if (!dm_guest_range(data, sizeof(uint64_t), 0)) return socket_error(SK_EFAULT);
    uint64_t value;
    memcpy(&value, data, sizeof(value));
    return socket_eventfd_write(fd, value) ? -1 : 8;
}

static int read_message(struct socket_message *out, const void *ptr, int writing) {
    if (!dm_guest_range(ptr, sizeof(*out), writing)) return SK_EFAULT;
    memcpy(out, ptr, sizeof(*out));
    if (out->iov_count < 0 || out->iov_count > 1024) return SK_EINVAL;
    if (out->iov_count && !dm_guest_range(out->iov, (size_t)out->iov_count * sizeof(*out->iov), 0)) return SK_EFAULT;
    return 0;
}

int64_t socket_sendmsg(int fd, const void *message, int flags) {
    struct socket_message msg;
    int error = read_message(&msg, message, 0);
    if (error) return socket_error(error);
    if (msg.control_length) return socket_error(SK_EOPNOTSUPP);
    return udp_send(fd, msg.iov, msg.iov_count, flags, msg.name, msg.name_length);
}

int64_t socket_recvmsg(int fd, struct socket_message *message, int flags) {
    struct socket_message msg;
    int error = read_message(&msg, message, 1);
    if (error) return socket_error(error);
    int message_flags = 0;
    uint32_t name_length = msg.name ? msg.name_length : 0;
    int64_t result = udp_recv(fd, msg.iov, msg.iov_count, flags, msg.name, &name_length, &message_flags);
    if (result >= 0) {
        message->name_length = name_length;
        message->control_length = 0; // This transport does not generate control messages.
        message->flags = message_flags;
    }
    return result;
}

int socket_getaddrinfo(const char *host, const char *service, const void *hints, void **result) {
    (void)host; (void)service; (void)hints;
    if (!dm_guest_range(result, sizeof(*result), 1)) {
        socket_error(SK_EFAULT);
        return 11; // EAI_SYSTEM
    }
    *result = NULL;
    socket_error(socket_is_initialized() ? SK_ENETDOWN : SK_EINVAL);
    return 11;
}

int socket_resolver_get(void *option, int key) {
    (void)key;
    return socket_error(socket_is_initialized() && option ? SK_ENETDOWN : SK_EINVAL);
}

int socket_resolver_set(const void *option) {
    return socket_error(socket_is_initialized() && option ? SK_ENETDOWN : SK_EINVAL);
}

// BSD loopback is process-local. NIFM therefore has no external interface,
// proxy profile or network-error applet that could satisfy a submitted request.
// A pending async request is observed once before it settles as rejected.
enum { NIFM_IDLE, NIFM_PENDING, NIFM_REJECTED };
static volatile LONG nifm_request;
uint32_t nifm_initialize(void) { return 0; }
void nifm_cancel(void) { InterlockedExchange(&nifm_request, NIFM_IDLE); }
void nifm_submit(void) { InterlockedExchange(&nifm_request, NIFM_PENDING); }
void nifm_submit_wait(void) { InterlockedExchange(&nifm_request, NIFM_REJECTED); }
int nifm_on_hold(void) {
    return InterlockedCompareExchange(&nifm_request, NIFM_REJECTED, NIFM_PENDING) == NIFM_PENDING;
}
int nifm_unavailable(void) { return 0; }
uint32_t nifm_proxy(void *out) {
    (void)out; // SDK leaves the output untouched when no profile is available.
    return 110u | (1111u << 9); // ResultNetworkCommunicationDisabled
}
