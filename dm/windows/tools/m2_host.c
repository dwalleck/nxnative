// Windows bring-up host: native ARM64 guest threads and SDK/SVC dispatch.
#include "m2_host.h"
#include "m4_window.h"
#include "m4_phase.h"
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#include <inttypes.h>
#include <process.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winternl.h>

struct guest_tls {
    uint64_t x18, tpidr, tpidrro;
    void (*gate)(void);
    uintptr_t native_sp;
};
static __declspec(thread) struct guest_tls slots;
extern unsigned long _tls_index; // PE TLS index provided by the CRT/linker.
uint64_t m2_native_sp_offset;
void m2_gate(void);
uint64_t m2_enter(uintptr_t entry, uint64_t arg0, uint64_t arg1, uintptr_t guest_sp, uintptr_t *native_sp);

struct dm_span { uint64_t base, size; };
enum { GUEST_THREADS = 96, WORKER_TLS_PAGES = 3 };
enum thread_phase { THREAD_DORMANT, THREAD_RUNNING, THREAD_EXITED };
struct guest_thread {
    struct dm_span stack;
    uint64_t entry, arg, top, core_mask;
    uint32_t handle, id;
    int priority, core, open;
    unsigned waiters;
    const struct wait_ref *waiting;
    int nwaiting, wait_cancelled, cancel_pending;
    enum thread_phase phase;
};
static SRWLOCK kernel_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE thread_changed = CONDITION_VARIABLE_INIT;
// Waiters live on native SVC stacks and are linked only under kernel_lock.
struct guest_waiter {
    struct guest_waiter *next;
    struct guest_thread *thread;
    uint64_t address, reacquire;
    uint32_t tag;
    CONDITION_VARIABLE ready;
    int signaled;
};
static struct guest_waiter *address_waiters, *mutex_waiters, *key_waiters;
static struct guest_thread workers[GUEST_THREADS - 1];
static struct guest_thread main_thread = {
    .handle = 0x1001, .id = 1, .priority = 44, .core_mask = 1, .open = 1, .phase = THREAD_RUNNING,
};
static __declspec(thread) struct guest_thread *current_thread;
static uint32_t next_handle = 0x1002, next_thread_id = 2;
uint32_t dm_alloc_handle(void) {
    // The top bits of a Horizon thread handle are reserved by mutex users.
    return next_handle < 0x40000000u ? next_handle++ : 0;
}
static struct dm_span arena, heap, stacks, alias, worker_tls;
static uint64_t service_base;
static struct service_block { struct dm_span span; struct service_block *next; } *service_blocks;
static struct dm_span permission_locks[16];
static struct stack_move { uint64_t dst, src, size; } stack_moves[GUEST_THREADS];
static size_t nstack_moves;
static size_t nlocks;
static struct dm_module modules[8];
static size_t nmodules;
static uint64_t heap_used, alias_used, app_pool = 0xcd500000, mem_used = 0x4000000,
    sys_resource = 0x2000000, program_id, exception_addr;
static int alias_counts, trace;
static uint64_t clock_start, clock_freq, tick_rate = 19200000;
static uint64_t entropy[4];
static uint64_t process_core_mask = 7;

static void bind_guest_tls(uint64_t area, struct guest_thread *t) {
    current_thread = t;
    slots.x18 = 0;
    slots.tpidr = area;
    slots.tpidrro = area + 2 * DM_PAGE;
    slots.gate = m2_gate;
    *(uint64_t *)(uintptr_t)(slots.tpidrro + 0x1f8) = slots.tpidrro + 0x200;
    *(uint32_t *)(uintptr_t)(slots.tpidrro + 0x110) = t->id;
}

_Noreturn void dm_fail(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    fputs("[RED] ", stderr);
    vfprintf(stderr, format, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static int contains(struct dm_span s, uint64_t base, uint64_t size) {
    return base >= s.base && size <= s.size && base - s.base <= s.size - size;
}

static int stack_source_busy(uint64_t addr, uint64_t size) {
    for (size_t i = 0; i < nstack_moves; ++i) {
        const struct stack_move *m = &stack_moves[i];
        if (addr < m->src + m->size && m->src < addr + size) return 1;
    }
    return 0;
}

static uint64_t scaled_guest_clock(uint64_t rate) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    uint64_t d = (uint64_t)now.QuadPart - clock_start;
    return (d / clock_freq) * rate + (d % clock_freq) * rate / clock_freq;
}
static uint64_t ticks(void) { return scaled_guest_clock(tick_rate); }
uint64_t dm_guest_time_ns(void) { return scaled_guest_clock(UINT64_C(1000000000)); }

static volatile LONG64 vm_generation;
uint64_t dm_guest_access_generation(void) {
    return (uint64_t)ReadAcquire64(&vm_generation);
}
static void protect(void *address, size_t size, DWORD permission) {
    DWORD old;
    dm_guest_gpu_unmap_begin(address, size);
    if (!VirtualProtect(address, size, permission, &old)) dm_fail("VirtualProtect: %lu", GetLastError());
    InterlockedIncrement64(&vm_generation);
    dm_guest_gpu_unmap_end();
}
static void commit(uint64_t address, uint64_t size) {
    if (size && !VirtualAlloc((void *)(uintptr_t)address, size, MEM_COMMIT, PAGE_READWRITE))
        dm_fail("VirtualAlloc commit %#" PRIx64 " + %#" PRIx64 ": %lu", address, size, GetLastError());
    // Committing PAGE_READWRITE cannot revoke an existing positive access
    // proof. There are no cached failures; newly accessible pages are queried.
}

static void decommit(uint64_t address, uint64_t size) {
    // Keep the single guest VA reservation intact so recommitting gives the SDK the same address.
    if (!size) return;
    dm_guest_gpu_unmap_begin((const void *)(uintptr_t)address, size);
#pragma warning(suppress: 6250)
    if (!VirtualFree((void *)(uintptr_t)address, size, MEM_DECOMMIT))
        dm_fail("VirtualFree decommit %#" PRIx64 " + %#" PRIx64 ": %lu", address, size, GetLastError());
    InterlockedIncrement64(&vm_generation);
    dm_guest_gpu_unmap_end();
}

// Backend-only SDK objects and worker stacks need guest addresses, without
// consuming the game's DSP budget or colliding with Horizon's heap/alias.
void *dm_service_alloc(size_t size) {
    if (!size || (size & (DM_PAGE - 1))) return NULL;
    AcquireSRWLockExclusive(&kernel_lock);
    struct service_block **link = &service_blocks;
    uint64_t start = service_base;
    while (*link && size > (*link)->span.base - start) {
        start = (*link)->span.base + (*link)->span.size;
        link = &(*link)->next;
    }
    struct service_block *block = NULL;
    if (start && start <= heap.base && size <= heap.base - start) block = malloc(sizeof(*block));
    if (block) {
        commit(start, size);
        *block = (struct service_block){{start, size}, *link};
        *link = block;
    }
    ReleaseSRWLockExclusive(&kernel_lock);
    return block ? (void *)(uintptr_t)start : NULL;
}
void dm_service_free(void *ptr, size_t size) {
    uint64_t start = (uintptr_t)ptr;
    AcquireSRWLockExclusive(&kernel_lock);
    struct service_block **link = &service_blocks;
    while (*link && (*link)->span.base != start) link = &(*link)->next;
    if (!*link || (*link)->span.size != size || stack_source_busy(start, size))
        dm_fail("invalid or still-mapped service allocation");
    struct service_block *block = *link;
    *link = block->next;
    decommit(start, size);
    free(block);
    ReleaseSRWLockExclusive(&kernel_lock);
}
static int service_contains(uint64_t base, uint64_t size) {
    for (struct service_block *b = service_blocks; b; b = b->next)
        if (contains(b->span, base, size)) return 1;
    return 0;
}

static void reserve(uint64_t size, uint64_t stack_size, uint64_t alias_size) {
    if (size >= DM_GUEST_LIMIT || size < DM_MODULE_ROOM || stack_size < 0x800000 ||
        stack_size > size - DM_MODULE_ROOM || alias_size > size - DM_MODULE_ROOM - stack_size ||
        ((size | stack_size | alias_size) & (DM_PAGE - 1))) dm_fail("invalid guest region sizes");
    SYSTEM_INFO sys;
    GetNativeSystemInfo(&sys);
    if (sys.dwPageSize != DM_PAGE) dm_fail("prototype requires 4 KiB native pages");
    MEM_ADDRESS_REQUIREMENTS bounds = { (void *)0x8000000, (void *)(uintptr_t)(DM_GUEST_LIMIT - 1), sys.dwAllocationGranularity };
    MEM_EXTENDED_PARAMETER param = {0};
    param.Type = MemExtendedParameterAddressRequirements;
    param.Pointer = &bounds;
    arena = (struct dm_span){ (uintptr_t)VirtualAlloc2(GetCurrentProcess(), NULL, size, MEM_RESERVE, PAGE_NOACCESS, &param, 1), size };
    if (!arena.base) dm_fail("cannot reserve guest AS: %lu", GetLastError());
    heap = (struct dm_span){ arena.base + DM_MODULE_ROOM, size - DM_MODULE_ROOM - stack_size - alias_size };
    stacks = (struct dm_span){ heap.base + heap.size, stack_size };
    alias = (struct dm_span){ stacks.base + stack_size, alias_size };
    // The SDK's stack allocator must never select the worker TLS reserve.
    worker_tls.size = ARRAYSIZE(workers) * WORKER_TLS_PAGES * DM_PAGE;
    stacks.size -= worker_tls.size;
    worker_tls.base = stacks.base + stacks.size;
    // Keep a guard page under the stack and a separate four-page TLS/exception area.
    main_thread.stack = (struct dm_span){ stacks.base + DM_PAGE, 0x400000 };
    commit(main_thread.stack.base, main_thread.stack.size);
    uint64_t thread_area = main_thread.stack.base + main_thread.stack.size + DM_PAGE;
    commit(thread_area, 4 * DM_PAGE);
    exception_addr = thread_area + 3 * DM_PAGE;
    bind_guest_tls(thread_area, &main_thread);
    printf("guest AS %p +%#" PRIx64 "  heap %p alias %p stack %p\n", (void *)(uintptr_t)arena.base,
           arena.size, (void *)(uintptr_t)heap.base, (void *)(uintptr_t)alias.base, (void *)(uintptr_t)stacks.base);
}

static int page_span(uint64_t addr, uint64_t *base, uint64_t *size, DWORD *prot) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)addr, &mi, sizeof(mi))) return 0;
    *base = (uintptr_t)mi.BaseAddress;
    *size = mi.RegionSize;
    *prot = mi.State == MEM_COMMIT ? mi.Protect : 0;
    return 1;
}

// VirtualQuery walks to the end of a homogeneous extent. On a multi-GiB
// heap that costs milliseconds even to validate a four-byte NVN value.
// Cache only positive accessibility results, never guest QueryMemory replies.
static int accessible_span(uint64_t p, int write, uint64_t *end) {
    static __declspec(thread) struct cached_access {
        uint64_t begin, end;
        int writable;
    } spans[32];
    static __declspec(thread) unsigned used, hot, replace;
    static __declspec(thread) LONG64 generation;
    const LONG64 gen = ReadAcquire64(&vm_generation);
    if (generation != gen) {
        used = hot = replace = 0;
        generation = gen;
    }
    for (unsigned n = 0, i = hot; n < used; ++n) {
        const struct cached_access *s = &spans[i];
        if (p >= s->begin && p < s->end && (!write || s->writable)) {
            hot = i;
            *end = s->end;
            return 1;
        }
        if (++i == used) i = 0;
    }
    uint64_t base, count;
    DWORD prot;
    if (!page_span(p, &base, &count, &prot) || (prot != PAGE_READWRITE &&
        (write || (prot != PAGE_READONLY && prot != PAGE_EXECUTE_READ)))) return 0;
    *end = base + count;
    // VirtualQuery starts at the requested page, not at the start of its
    // homogeneous region. A descending query extends the same suffix;
    // don't displace unrelated proofs with copies of that suffix.
    const int writable = prot == PAGE_READWRITE;
    for (unsigned i = 0; i < used; ++i) {
        if (spans[i].end == *end && spans[i].writable == writable) {
            if (base < spans[i].begin) spans[i].begin = base;
            hot = i;
            return 1;
        }
    }
    hot = used < ARRAYSIZE(spans) ? used++ : replace++ % ARRAYSIZE(spans);
    spans[hot] = (struct cached_access){base, *end, writable};
    return 1;
}
int dm_guest_range(const void *ptr, size_t size, int write) {
    uint64_t addr = (uintptr_t)ptr;
    if (!contains(arena, addr, size)) return 0;
    for (uint64_t p = addr, end = addr + size; p < end;) {
        if (!accessible_span(p, write, &p)) return 0;
    }
    return 1;
}

int dm_guest_string(const char *input, char *out, size_t capacity) {
    uint64_t p = (uintptr_t)input;
    for (size_t at = 0; at < capacity && contains(arena, p, 1);) {
        uint64_t end;
        if (!accessible_span(p, 0, &end)) return 0;
        uint64_t available = end - p, remaining = arena.base + arena.size - p;
        if (available > remaining) available = remaining;
        size_t take = available < capacity - at ? (size_t)available : capacity - at;
        const char *src = (const char *)(uintptr_t)p;
        const char *nul = memchr(src, 0, take);
        if (nul) { memcpy(out + at, src, (size_t)(nul - src) + 1); return 1; }
        memcpy(out + at, src, take);
        at += take;
        p += take;
    }
    return 0;
}

static int guest_write_ok(uint64_t addr, size_t size) {
    return dm_guest_range((const void *)(uintptr_t)addr, size, 1);
}

struct memory_info {
    uint64_t base, size;
    uint32_t state, attr, perm, ipc_refs, device_refs, padding;
};

static void query_memory(uint64_t *r) {
    struct memory_info m = {0};
    if (!guest_write_ok(r[0], sizeof(m))) { r[0] = 0xd401; return; }
    if (r[2] >= DM_GUEST_LIMIT) {
        m.base = DM_GUEST_LIMIT;
        m.size = 0 - m.base;
        m.state = 0x10;
    } else {
        int found = 0;
        for (size_t j = 0; !found && j < nmodules; ++j)
            found = dm_module_region(&modules[j], r[2], &m.base, &m.size, &m.state, &m.perm);
        if (found && m.state == 4 && m.perm == 3) {
            uint64_t base, size;
            DWORD prot;
            if (!page_span(r[2], &base, &size, &prot)) dm_fail("VirtualQuery module data failed");
            uint64_t end = m.base + m.size;
            if (base > m.base) m.base = base;
            m.size = (end < base + size ? end : base + size) - m.base;
            if (prot == PAGE_READONLY) m.perm = 1;
        }
        if (!found && contains(main_thread.stack, r[2], 1)) {
            m.base = main_thread.stack.base; m.size = main_thread.stack.size; m.state = 0xb; m.perm = 3; found = 1;
        }
        if (!found && contains(arena, r[2], 1)) {
            DWORD prot;
            if (!page_span(r[2], &m.base, &m.size, &prot)) dm_fail("VirtualQuery failed");
            if (prot == PAGE_READWRITE) { m.state = 5; m.perm = 3; }
            if (prot == PAGE_READONLY) { m.state = 5; m.perm = 1; }
            found = 1;
        }
        if (!found) {
            m.base = r[2] < arena.base ? 0 : arena.base + arena.size;
            m.size = (r[2] < arena.base ? arena.base : DM_GUEST_LIMIT) - m.base;
        }
    }
    for (size_t i = 0; i < nstack_moves; ++i) {
        const struct stack_move *move = &stack_moves[i];
        if (contains((struct dm_span){move->dst, move->size}, r[2], 1)) {
            m.base = move->dst; m.size = move->size; m.state = 0xb; m.perm = 3;
            break;
        }
        if (contains((struct dm_span){move->src, move->size}, r[2], 1)) {
            m.base = move->src; m.size = move->size; m.state = 5; m.attr = 1; m.perm = 0;
            break;
        }
    }
    uint64_t end = m.base + m.size;
    for (size_t i = 0; i < nlocks; ++i) {
        struct dm_span lock = permission_locks[i];
        uint64_t hi = lock.base + lock.size;
        if (contains(lock, r[2], 1)) {
            if (lock.base > m.base) m.base = lock.base;
            if (hi < end) end = hi;
            m.attr |= 0x10;
            break;
        }
        if (hi <= r[2] && hi > m.base) m.base = hi;
        if (lock.base > r[2] && lock.base < end) end = lock.base;
    }
    m.size = end - m.base;
    memcpy((void *)(uintptr_t)r[0], &m, sizeof(m));
    r[0] = r[1] = 0;
}

// Horizon removes access to the source while its bytes live in the stack
// region. A copy with exclusive access and copy-back has the same guest-visible
// behavior and doesn't require remapping inside the Windows VA reservation.
static void move_stack(uint64_t *r, int map) {
    uint64_t dst = r[0], src = r[1], size = r[2];
    if ((dst | src) & (DM_PAGE - 1)) { r[0] = 0xcc01; return; }
    if (!size || (size & (DM_PAGE - 1))) { r[0] = 0xca01; return; }
    if (!contains(stacks, dst, size)) { r[0] = 0xdc01; return; }
    if (!contains((struct dm_span){heap.base, heap_used}, src, size) && !contains(alias, src, size) &&
        !service_contains(src, size)) {
        r[0] = 0xd401; return;
    }
    void *from = (void *)(uintptr_t)src, *to = (void *)(uintptr_t)dst;
    if (map) {
        if (stack_source_busy(src, size) || !dm_guest_range(from, size, 1)) { r[0] = 0xd401; return; }
        // Keep the permanently reserved guard, initial thread stack and TLS out of the destination.
        if (dst < exception_addr + DM_PAGE) { r[0] = 0xd401; return; }
        for (uint64_t p = dst; p < dst + size;) {
            uint64_t base, count;
            DWORD prot;
            if (!page_span(p, &base, &count, &prot)) dm_fail("MapMemory: cannot query destination");
            if (prot) { r[0] = 0xd401; return; }
            p = base + count;
        }
        if (nstack_moves >= ARRAYSIZE(stack_moves)) { r[0] = 0xce01; return; }
        commit(dst, size);
        memcpy(to, from, (size_t)size);
        protect(from, size, PAGE_NOACCESS);
        stack_moves[nstack_moves++] = (struct stack_move){dst, src, size};
    } else {
        size_t i = 0;
        while (i < nstack_moves && (stack_moves[i].dst != dst || stack_moves[i].src != src || stack_moves[i].size != size)) ++i;
        if (i == nstack_moves) { r[0] = 0xd401; return; }
        for (size_t j = 0; j < ARRAYSIZE(workers); ++j) {
            const struct guest_thread *t = &workers[j];
            if (t->id && t->phase != THREAD_EXITED && t->stack.base == dst) { r[0] = 0xd401; return; }
        }
        protect(from, size, PAGE_READWRITE);
        memcpy(from, to, (size_t)size);
        decommit(dst, size);
        stack_moves[i] = stack_moves[--nstack_moves];
    }
    r[0] = 0;
}

static uint64_t used_non_heap(void) { return mem_used + (alias_counts ? alias_used : 0); }

static void set_heap(uint64_t *r) {
    uint64_t want = r[1], used = used_non_heap();
    if (want & (DM_PAGE - 1)) { r[0] = 0xca01; return; }
    if (want > heap.size) { r[0] = 0xd001; return; }
    if (used > app_pool || want > app_pool - used) { r[0] = 0x10801; return; }
    if (want > heap_used) commit(heap.base + heap_used, want - heap_used);
    if (want < heap_used) {
        if (stack_source_busy(heap.base + want, heap_used - want)) { r[0] = 0xd401; return; }
        decommit(heap.base + want, heap_used - want);
    }
    heap_used = want;
    printf("SetHeapSize %#" PRIx64 " -> base %p\n", want, (void *)(uintptr_t)heap.base);
    fflush(stdout);
    r[0] = 0; r[1] = heap.base;
}

static void alias_memory(uint64_t *r, int map) {
    uint64_t addr = r[0], size = r[1];
    if (addr & (DM_PAGE - 1)) { r[0] = 0xcc01; return; }
    if (!size || (size & (DM_PAGE - 1))) { r[0] = 0xca01; return; }
    if (!contains(alias, addr, size) || stack_source_busy(addr, size)) { r[0] = 0xd401; return; }
    uint64_t committed = 0;
    for (uint64_t p = addr; p < addr + size;) {
        uint64_t base, count;
        DWORD prot;
        if (!page_span(p, &base, &count, &prot)) dm_fail("VirtualQuery alias failed");
        uint64_t end = base + count;
        if (end > addr + size) end = addr + size;
        if (prot) committed += end - p;
        p = end;
    }
    if (map) {
        uint64_t used = used_non_heap() + heap_used;
        if (alias_counts && (used > app_pool || size - committed > app_pool - used)) { r[0] = 0x10801; return; }
        commit(addr, size);
        alias_used += size - committed;
    } else {
        decommit(addr, size);
        alias_used -= committed;
    }
    r[0] = 0;
}

static int info(uint64_t *r) {
    uint64_t v;
    switch (r[1]) {
    case 0: v = process_core_mask; break;
    case 1: case 8: case 9: case 10: case 25: case 26: case 27: case 28:
    case 29: case 30: case 31: case 32: v = 0; break;
    case 2: v = alias.base; break; case 3: v = alias.size; break;
    case 4: v = heap.base; break; case 5: v = heap.size; break;
    case 6: case 21: v = app_pool; break;
    case 7: case 22: v = used_non_heap() + heap_used; break;
    case 11:
        if (r[3] >= ARRAYSIZE(entropy)) return 0;
        v = entropy[r[3]];
        break;
    case 12: v = arena.base; break; case 13: v = arena.size; break;
    case 14: v = stacks.base; break; case 15: v = stacks.size; break;
    case 16: v = sys_resource; break; case 17: v = sys_resource ? 0x400000 : 0; break;
    case 18: v = program_id; break;
    case 20: v = exception_addr; break;
    case 23: v = 1; break;
    case 24:
        v = 0;
        for (size_t i = 0; i < ARRAYSIZE(workers); ++i) if (!workers[i].id) ++v;
        break;
    case 33: v = DM_PAGE; break;
    default: return 0;
    }
    r[0] = 0; r[1] = v;
    return 1;
}

// Same service startup takeovers as hle_sdkinit.c, hle_sdk16.c and hle_time.c.
// They do not make later service calls succeed: those still stop at the first missing operation.
static uint32_t boot_no_service(void) { return 0; }

// Headless host stays in the foreground. Keep the applet state on the host;
// the SDK proxy is not initialized because this host does not open AM sessions.
static volatile LONG oe_focus_mode = -1;
static ULONGLONG active_time_origin;
uint64_t oe_program_active_time(void) {
    ULONGLONG now;
    QueryUnbiasedInterruptTimePrecise(&now);
    return (now - active_time_origin) * 100; // SDK TimeSpan ns; exclude Windows sleep/hibernate.
}
static SRWLOCK oe_focus_lock = SRWLOCK_INIT;
static uint32_t oe_last_focus = 1;
static void oe_set_focus_mode(int mode) {
    AcquireSRWLockExclusive(&oe_focus_lock);
    oe_last_focus = dm_m4_focus_serial();
    oe_focus_mode = mode;
    ReleaseSRWLockExclusive(&oe_focus_lock);
}
static int oe_get_focus(void) { return (dm_m4_focus_serial() & 1u) ? 1 : 2; }
static unsigned oe_take_focus(uint32_t *observed) {
    AcquireSRWLockExclusive(&oe_focus_lock);
    *observed = dm_m4_focus_serial();
    const unsigned available = (oe_focus_mode == 1 || oe_focus_mode == 2) && oe_last_focus != *observed;
    if (available) oe_last_focus = *observed;
    ReleaseSRWLockExclusive(&oe_focus_lock);
    return available;
}
static uint32_t oe_pop(void) {
    for (;;) {
        uint32_t generation;
        if (oe_take_focus(&generation)) return 15; // FocusStateChanged
        dm_m4_wait_focus_change(generation);
    }
}
static unsigned oe_try_pop(uint32_t *message) {
    uint32_t generation;
    if (!oe_take_focus(&generation)) return 0;
    if (!dm_guest_range(message, sizeof(*message), 1)) dm_fail("oe::TryPopNotificationMessage: invalid output");
    *message = 15; // FocusStateChanged: the consumer queries the latest state.
    return 1;
}
// The Windows guest has coherent CPU memory; the GPU backend stages from it
// at submit. Order the writes without executing EL0 DC CIVAC, which traps.
static void os_flush_data_cache(const void *ptr, size_t bytes) {
    if (!bytes) return;
    if (!dm_guest_range(ptr, bytes, 0)) dm_fail("os::FlushDataCache: invalid guest range");
    MemoryBarrier();
}
static void oe_set_resume_notification(unsigned on) { (void)on; } // No suspend/resume source in the headless host.
static int oe_operation_mode = 1, oe_perf_boost;
static char oe_language[8] = "en-US";
static char oe_version[16] = "1.0.0";
static void oe_get_version(char *out) {
    if (!dm_guest_range(out, sizeof(oe_version), 1)) dm_fail("oe::GetDisplayVersion: invalid output");
    memcpy(out, oe_version, sizeof(oe_version));
}
static uint64_t oe_get_language(void) { uint64_t code; memcpy(&code, oe_language, sizeof(code)); return code; }
static int oe_get_opmode(void) { return oe_operation_mode; }
static int oe_get_perfmode(void) { return oe_perf_boost ? 1 : oe_operation_mode; }
static int oe_get_scale(void) { return 0; }
static volatile LONG oe_inactivity, oe_notify_opmode, oe_notify_perfmode, oe_notify_scale;
static void oe_set_inactivity(unsigned on) { InterlockedExchange(&oe_inactivity, on & 1u); }
static void oe_set_opmode_notification(unsigned on) { InterlockedExchange(&oe_notify_opmode, on & 0xffu); }
static void oe_set_perfmode_notification(unsigned on) { InterlockedExchange(&oe_notify_perfmode, on & 0xffu); }
static void oe_set_scale_notification(unsigned on) { InterlockedExchange(&oe_notify_scale, on & 0xffu); }

static int fixture_mode;
extern void m3_fixture_sampler_snapshot(void);
extern void m3_fixture_guest_call(void);
extern void m3_prepo_offline_send(void);

uintptr_t dm_boot_import(const char *name) {
    if (fixture_mode && !strcmp(name, "dmFixtureSamplerSnapshot")) return (uintptr_t)m3_fixture_sampler_snapshot;
    if (fixture_mode && !strcmp(name, "dmFixtureGuestCall")) return (uintptr_t)m3_fixture_guest_call;
    uintptr_t service = dm_m3_import(name);
    if (service) return service;
    // Do not retain or transmit player telemetry. Leave report construction,
    // completion events, result inspection and destruction to the guest SDK.
    if (!strcmp(name, "_ZN2nn5prepo10PlayReport4SendERKNS_7account3UidE")) return (uintptr_t)m3_prepo_offline_send;
    if (!strcmp(name, "_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE") ||
        !strcmp(name, "_ZN2nn5prepo10PlayReport4SaveEv") ||
        !strcmp(name, "_ZN2nn5prepo28RequestImmediateTransmissionEv")) return (uintptr_t)boot_no_service;
    if (!strcmp(name, "_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE")) return (uintptr_t)oe_set_focus_mode;
    if (!strcmp(name, "_ZN2nn2oe20GetCurrentFocusStateEv")) return (uintptr_t)oe_get_focus;
    if (!strcmp(name, "_ZN2nn2oe18GetDesiredLanguageEv")) return (uintptr_t)oe_get_language;
    if (!strcmp(name, "_ZN2nn2oe25TryPopNotificationMessageEPj")) return (uintptr_t)oe_try_pop;
    if (!strcmp(name, "_ZN2nn2oe22PopNotificationMessageEv")) return (uintptr_t)oe_pop;
    if (!strcmp(name, "_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE")) return (uintptr_t)oe_get_version;
    if (!strcmp(name, "_ZN2nn2os14FlushDataCacheEPKvm")) return (uintptr_t)os_flush_data_cache;
    if (!strcmp(name, "_ZN2nn2oe16GetOperationModeEv")) return (uintptr_t)oe_get_opmode;
    if (!strcmp(name, "_ZN2nn2oe18GetPerformanceModeEv")) return (uintptr_t)oe_get_perfmode;
    if (!strcmp(name, "_ZN2nn2oe23GetCurrentGraphicsScaleEv")) return (uintptr_t)oe_get_scale;
    if (!strcmp(name, "_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb")) return (uintptr_t)oe_set_inactivity;
    if (!strcmp(name, "_ZN2nn2oe28SetResumeNotificationEnabledEb")) return (uintptr_t)oe_set_resume_notification;
    if (!strcmp(name, "_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb")) return (uintptr_t)oe_set_opmode_notification;
    if (!strcmp(name, "_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb")) return (uintptr_t)oe_set_perfmode_notification;
    if (!strcmp(name, "_ZN2nn2oe42SetGraphicsScaleChangedNotificationEnabledEb")) return (uintptr_t)oe_set_scale_notification;
    static const char *const names[] = {
        "nndiagStartup", "_ZN2nn2oe10InitializeEv", "_ZN2nn3aoc10InitializeEv",
        "_ZN2nn2oe17FinishStartupLogoEv", // No system overlay covers the Windows swapchain.
        "_ZN2nn3err26InitializeContextWriterApiEv", "_ZN2nn4pctl10InitializeEv",
        "_ZN2nn4diag34InitializeApplicationAbortObserverEv", "_ZN2nn2fs24InitializeForApplicationEv",
        "_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE", "_ZN2nn4time10InitializeEv"
    };
    for (unsigned i = 0; i < ARRAYSIZE(names); ++i)
        if (!strcmp(name, names[i])) return (uintptr_t)boot_no_service;
    return 0;
}

static int guest_entry(uint64_t pc) {
    if (pc & 3) return 0;
    for (size_t i = 0; i < nmodules; ++i)
        if (contains((struct dm_span){(uintptr_t)modules[i].base, modules[i].text}, pc, 4)) return 1;
    return 0;
}

extern struct dm_guest_result m2_guest_call_asm(
    uintptr_t entry, const uint64_t *args, uintptr_t *native_sp, uintptr_t guest_sp);

uintptr_t dm_guest_sdk_symbol(const char *name) {
    for (size_t i = 0; i < nmodules; ++i) {
        if (strcmp(modules[i].name, "sdk")) continue;
        // The loader only returns definitions here, not relocated imports.
        uintptr_t entry = dm_find_symbol(&modules[i], 1, name);
        return guest_entry(entry) ? entry : 0;
    }
    return 0;
}

int dm_guest_call(uintptr_t entry, const uint64_t args[8], struct dm_guest_result *result) {
    static const uint64_t no_args[8] = {0};
    if (!current_thread || !slots.native_sp || !guest_entry(entry)) return 0;
    uintptr_t sp = ((const uintptr_t *)slots.native_sp)[1];
    // The guest may have switched to a user-mode fiber. Validate the actual
    // writable gate frame, not the kernel thread's original stack allocation.
    if (sp < 32 || (sp & 15) || !guest_write_ok(sp - 32, 32)) return 0;

    uint64_t x18 = slots.x18, tpidr = slots.tpidr;
    struct dm_guest_result r = m2_guest_call_asm(entry, args ? args : no_args, &slots.native_sp, sp);
    slots.x18 = x18;
    slots.tpidr = tpidr;
    if (result) *result = r;
    return 1;
}

uint64_t fixture_guest_call(uintptr_t entry, const uint64_t *args) {
    if (!entry) entry = dm_guest_sdk_symbol("dmFixtureGuestCall");
    struct dm_guest_result r;
    if (!dm_guest_range(args, 8 * sizeof(*args), 0) || !dm_guest_call(entry, args, &r))
        return UINT64_MAX;
    if (r.x0 != 0x1357) return r.x0 ? r.x0 : 6;
    return r.x1 == 0x2468 ? 0 : 6;
}

// The following thread operations and the memory SVCs share kernel_lock. In
// particular UnmapMemory must not reclaim a stack between Create and Exit.
static struct guest_thread *thread_for_handle(uint32_t handle) {
    if (handle == 0xffff8000u) return current_thread;
    if (handle == main_thread.handle && main_thread.open) return &main_thread;
    for (size_t i = 0; i < ARRAYSIZE(workers); ++i)
        if (workers[i].open && workers[i].handle == handle) return &workers[i];
    return NULL;
}
static void thread_collect(struct guest_thread *t) {
    if (t != &main_thread && !t->open && !t->waiters && t->phase == THREAD_EXITED) *t = (struct guest_thread){0};
}
static unsigned WINAPI run_worker(void *arg) {
    struct guest_thread *t = arg;
    uint64_t area = worker_tls.base + (size_t)(t - workers) * WORKER_TLS_PAGES * DM_PAGE;
    // Publish StartThread's state before the worker reads it or enters guest code.
    AcquireSRWLockExclusive(&kernel_lock);
    commit(area, WORKER_TLS_PAGES * DM_PAGE);
    bind_guest_tls(area, t);
    ReleaseSRWLockExclusive(&kernel_lock);
    m2_enter(t->entry, t->arg, 0, t->top, &slots.native_sp);
    dm_fail("guest thread %#x returned instead of calling ExitThread", t->handle);
}
static void thread_create(uint64_t *r) {
    uint64_t top = r[3];
    int32_t priority = (int32_t)r[4], core = (int32_t)r[5];
    if (core == -2) core = 0;
    if (core < 0 || core >= 64 || !(process_core_mask & (UINT64_C(1) << core)) ||
        priority < 0 || priority > 63) { r[0] = 0xee01; return; }
    if ((top & 15) || !guest_entry(r[1])) { r[0] = 0xcc01; return; }
    struct dm_span span = {0};
    for (size_t i = 0; i < nstack_moves; ++i) {
        const struct stack_move *m = &stack_moves[i];
        if (top > m->dst && top - m->dst <= m->size) { span = (struct dm_span){m->dst, m->size}; break; }
    }
    if (!span.size || top - span.base < 16) { r[0] = 0xd401; return; }
    struct guest_thread *free_slot = NULL;
    for (size_t i = 0; i < ARRAYSIZE(workers); ++i) {
        struct guest_thread *t = &workers[i];
        if (!t->id) { if (!free_slot) free_slot = t; }
        else if (t->phase != THREAD_EXITED && t->stack.base == span.base) { r[0] = 0xd401; return; }
    }
    uint32_t handle = free_slot ? dm_alloc_handle() : 0;
    if (!handle) { r[0] = 0xce01; return; }
    *free_slot = (struct guest_thread){ .stack = span, .entry = r[1], .arg = r[2], .top = top,
        .core_mask = UINT64_C(1) << core, .handle = handle, .id = next_thread_id++,
        .priority = priority, .core = core, .open = 1, .phase = THREAD_DORMANT };
    r[0] = 0;
    r[1] = free_slot->handle;
    if (trace) printf("[thread] created %#x id=%u stack=%#" PRIx64 " +%#" PRIx64 "\n",
                      free_slot->handle, free_slot->id, span.base, span.size);
}
static void thread_start(uint64_t *r) {
    struct guest_thread *t = thread_for_handle((uint32_t)r[0]);
    if (!t) { r[0] = 0xe401; return; }
    if (t->phase != THREAD_DORMANT) { r[0] = 0xfa01; return; }
    uintptr_t native = _beginthreadex(NULL, 0, run_worker, t, 0, NULL);
    if (!native) dm_fail("cannot start native thread for guest handle %#x", t->handle);
    t->phase = THREAD_RUNNING;
    CloseHandle((HANDLE)native); // Guest references and waits use the guest table, not native handles.
    r[0] = 0;
    if (trace) { printf("[thread] started %#x\n", t->handle); fflush(stdout); }
}
static void thread_exit(void) {
    struct guest_thread *t = current_thread;
    if (t == &main_thread) dm_fail("main guest ExitThread is not implemented");
    uint64_t area = worker_tls.base + (size_t)(t - workers) * WORKER_TLS_PAGES * DM_PAGE;
    decommit(area, WORKER_TLS_PAGES * DM_PAGE); // Use the original address, not the guest's writable TLS register.
    current_thread = NULL;
    slots.native_sp = 0;
    t->phase = THREAD_EXITED;
    if (trace) { printf("[thread] exited %#x\n", t->handle); fflush(stdout); }
    thread_collect(t);
    WakeAllConditionVariable(&thread_changed);
    ReleaseSRWLockExclusive(&kernel_lock);
    _endthreadex(0); // Already on the native stack; never return to the guest.
}
static void thread_close(uint64_t *r) {
    uint32_t handle = (uint32_t)r[0];
    struct guest_thread *t = thread_for_handle(handle);
    if (!t || handle != t->handle) { r[0] = 0xe401; return; }
    t->open = 0;
    if (t->phase == THREAD_DORMANT) { t->phase = THREAD_EXITED; WakeAllConditionVariable(&thread_changed); }
    thread_collect(t);
    r[0] = 0;
}
static uint64_t timeout_ms(uint64_t ns) { return ns / 1000000 + (ns % 1000000 != 0); }
static DWORD wait_chunk(uint64_t ms) { return ms < INFINITE ? (DWORD)ms : INFINITE - 1; }
#include "m2_events.inc"
struct wait_ref { struct guest_thread *thread; struct guest_event *event; uint64_t signals; };
static int first_ready(const struct wait_ref *refs, int n) {
    for (int i = 0; i < n; ++i)
        if (refs[i].thread ? refs[i].thread->phase == THREAD_EXITED :
            refs[i].event->set || refs[i].event->signals != refs[i].signals) return i;
    return n;
}
static void cancel_sync(uint64_t *r) {
    struct guest_thread *t = thread_for_handle((uint32_t)r[0]);
    if (!t) { r[0] = 0xe401; return; }
    // Both completion sources run under kernel_lock. A cancel after a signal
    // cannot take that wait back; leave it pending for the next blocking wait.
    if (t->waiting && !t->wait_cancelled && first_ready(t->waiting, t->nwaiting) == t->nwaiting) {
        t->wait_cancelled = 1;
        WakeAllConditionVariable(&thread_changed);
    } else t->cancel_pending = 1;
    r[0] = 0;
}
static void thread_wait(uint64_t *r) {
    enum { MAX_WAIT_HANDLES = 64 };
    int32_t n = (int32_t)r[2];
    int64_t ns = (int64_t)r[3];
    const uint32_t *handles = (const uint32_t *)(uintptr_t)r[1];
    struct wait_ref waiting[MAX_WAIT_HANDLES];
    uint32_t result = 0xe401;
    r[1] = 0;
    if (n < 0 || n > MAX_WAIT_HANDLES) { r[0] = 0xee01; return; }
    if (n && !dm_guest_range(handles, (size_t)n * sizeof(*handles), 0)) { r[0] = 0xd401; return; }
    int held = 0;
    for (; held < n; ++held) {
        struct guest_thread *t = thread_for_handle(handles[held]);
        if (t) {
            ++t->waiters;
            waiting[held] = (struct wait_ref){.thread = t};
        } else {
            struct event_end *end = event_end(handles[held]);
            if (!end || end->writable) break;
            struct guest_event *e = end->event;
            ++e->refs;
            waiting[held] = (struct wait_ref){.event = e, .signals = e->signals};
        }
    }
    if (held == n) {
        uint64_t deadline = ns >= 0 ? GetTickCount64() + timeout_ms((uint64_t)ns) : 0;
        for (;;) {
            if (current_thread->wait_cancelled) { result = 0xec01; break; }
            int i = first_ready(waiting, n);
            if (i < n) { result = 0; r[1] = (uint32_t)i; break; }
            uint64_t now = GetTickCount64();
            if (ns >= 0 && now >= deadline) { result = 0xea01; break; }
            if (current_thread->cancel_pending) { current_thread->cancel_pending = 0; result = 0xec01; break; }
            current_thread->waiting = waiting;
            current_thread->nwaiting = n;
            DWORD remaining = ns < 0 ? INFINITE : wait_chunk(deadline - now);
            if (!SleepConditionVariableSRW(&thread_changed, &kernel_lock, remaining, 0) && GetLastError() != ERROR_TIMEOUT)
                dm_fail("WaitSynchronization: cannot wait for a guest thread: %lu", GetLastError());
        }
        current_thread->waiting = NULL;
        current_thread->wait_cancelled = 0;
    }
    for (int i = 0; i < held; ++i) {
        struct guest_thread *t = waiting[i].thread;
        if (t) { --t->waiters; thread_collect(t); }
        else release_event(waiting[i].event);
    }
    r[0] = result;
}
enum {
    ADDR_INVALID_ADDRESS = 0xcc01, ADDR_INVALID_MEMORY = 0xd401, ADDR_INVALID_TYPE = 0xf001,
    ADDR_TIMED_OUT = 0xea01, ADDR_INVALID_STATE = 0xfa01,
};

static void append_waiter(struct guest_waiter **queue, struct guest_waiter *w) {
    while (*queue) queue = &(*queue)->next;
    *queue = w;
    w->next = NULL;
}
static uint32_t wait_on_queue(struct guest_waiter **queue, uint64_t address, uint32_t tag, int64_t ns,
                              uint64_t reacquire) {
    if (!ns) return ADDR_TIMED_OUT;
    // Enqueue before releasing kernel_lock in SleepConditionVariableSRW, so a
    // concurrent signal cannot slip between the predicate and the wait.
    struct guest_waiter waiter = { .thread = current_thread, .address = address, .tag = tag, .reacquire = reacquire };
    InitializeConditionVariable(&waiter.ready);
    append_waiter(queue, &waiter);
    uint64_t deadline = 0;
    if (ns > 0) {
        QueryInterruptTimePrecise(&deadline); // 100 ns units, monotonic.
        deadline += (uint64_t)ns / 100 + (ns % 100 != 0);
    }
    while (!waiter.signaled) {
        DWORD delay = INFINITE;
        // Once signaled a condition waiter is a mutex waiter. Its condition
        // timeout must not remove it from the new queue.
        if (ns > 0 && (!reacquire || waiter.reacquire)) {
            uint64_t now;
            QueryInterruptTimePrecise(&now);
            if (now >= deadline) break;
            delay = wait_chunk(timeout_ms((deadline - now) * 100));
        }
        if (!SleepConditionVariableSRW(&waiter.ready, &kernel_lock, delay, 0) && GetLastError() != ERROR_TIMEOUT)
            dm_fail("guest arbitration: native wait failed: %lu", GetLastError());
    }
    if (waiter.signaled) return 0; // The signaler already unlinked it.
    struct guest_waiter **link = queue;
    while (*link != &waiter) link = &(*link)->next;
    *link = waiter.next;
    return ADDR_TIMED_OUT;
}
static struct guest_waiter **pick_waiter(struct guest_waiter **queue, uint64_t address, int *matches) {
    struct guest_waiter **best = NULL;
    int n = 0;
    for (struct guest_waiter **link = queue; *link; link = &(*link)->next) {
        if ((*link)->address != address) continue;
        ++n;
        if (!best || (*link)->thread->priority < (*best)->thread->priority) best = link;
    }
    if (matches) *matches = n;
    return best;
}
static void wake_waiter(struct guest_waiter **link) {
    struct guest_waiter *w = *link;
    *link = w->next;
    w->signaled = 1;
    WakeConditionVariable(&w->ready);
}

#define MUTEX_WAITERS UINT32_C(0x40000000)
static uint32_t mutex_lock(uint32_t owner, uint64_t address, uint32_t tag) {
    if (address & (sizeof(LONG) - 1)) return ADDR_INVALID_ADDRESS;
    if (!guest_write_ok(address, sizeof(LONG))) return ADDR_INVALID_MEMORY;
    // These are guest tags (SDK 22 uses thread IDs), not necessarily handles.
    // If unlock won before this SVC, the SDK retries its user-mode acquisition.
    if ((uint32_t)ReadAcquire((volatile LONG *)(uintptr_t)address) != (owner | MUTEX_WAITERS)) return 0;
    return wait_on_queue(&mutex_waiters, address, tag, -1, 0);
}
static uint32_t mutex_unlock(uint64_t address) {
    if (address & (sizeof(LONG) - 1)) return ADDR_INVALID_ADDRESS;
    if (!guest_write_ok(address, sizeof(LONG))) return ADDR_INVALID_MEMORY;
    int n;
    struct guest_waiter **next = pick_waiter(&mutex_waiters, address, &n);
    uint32_t new_tag = next ? (*next)->tag | (n > 1 ? MUTEX_WAITERS : 0) : 0;
    // Publish the new owner and the caller's writes before waking the waiter.
    WriteRelease((volatile LONG *)(uintptr_t)address, (LONG)new_tag);
    if (next) wake_waiter(next);
    return 0;
}

static uint32_t process_key_wait(uint64_t mutex, uint64_t key, uint32_t tag, int64_t ns) {
    if ((key | mutex) & 3) return ADDR_INVALID_ADDRESS;
    if (mutex == key || !guest_write_ok(key, 4) || !guest_write_ok(mutex, 4)) return ADDR_INVALID_MEMORY;
    // SDK signalers check the key in user mode while holding the mutex.
    // Publish interest before releasing ownership, even for a timed poll.
    WriteRelease((volatile LONG *)(uintptr_t)key, 1);
    mutex_unlock(mutex);
    return wait_on_queue(&key_waiters, key, tag, ns, mutex);
}
static uint32_t process_key_signal(uint64_t key, int32_t count) {
    if (key & 3) return ADDR_INVALID_ADDRESS;
    if (!guest_write_ok(key, 4)) return ADDR_INVALID_MEMORY;
    for (int n = 0; count <= 0 || n < count; ++n) {
        struct guest_waiter **link = pick_waiter(&key_waiters, key, NULL);
        if (!link) break;
        struct guest_waiter *w = *link;
        volatile LONG *word = (volatile LONG *)(uintptr_t)w->reacquire;
        if (!guest_write_ok(w->reacquire, 4)) dm_fail("process key: backing mutex disappeared");
        LONG old = ReadAcquire(word);
        for (;;) {
            LONG value = old ? old | (LONG)MUTEX_WAITERS : (LONG)w->tag;
            LONG seen = InterlockedCompareExchange(word, value, old);
            if (seen == old) break;
            old = seen;
        }
        if (!old) wake_waiter(link);
        else {
            *link = w->next;
            w->address = w->reacquire;
            w->reacquire = 0;
            append_waiter(&mutex_waiters, w);
        }
    }
    if (!pick_waiter(&key_waiters, key, NULL)) WriteRelease((volatile LONG *)(uintptr_t)key, 0);
    return 0;
}

static uint32_t address_wait(uint64_t address, uint32_t type, uint64_t value, int64_t ns) {
    size_t width = type == 3 ? sizeof(LONG64) : sizeof(LONG);
    if (address & (width - 1)) return ADDR_INVALID_ADDRESS;
    if (type > 3) return ADDR_INVALID_TYPE;
    volatile LONG *word = (volatile LONG *)(uintptr_t)address;
    if (!dm_guest_range((const void *)(uintptr_t)address, width, type == 1)) return ADDR_INVALID_MEMORY;
    if (type == 3) {
        if ((uint64_t)ReadAcquire64((volatile LONG64 *)(uintptr_t)address) != value) return ADDR_INVALID_STATE;
    } else {
        LONG compare = (LONG)value;
        LONG old = ReadAcquire(word);
        for (;;) {
            if (type == 2 ? old != compare : old >= compare) return ADDR_INVALID_STATE;
            if (type != 1) break;
            // A guest CPU can change the word outside the SVC lock.
            LONG observed = InterlockedCompareExchange(word, (LONG)((uint32_t)old - 1), old);
            if (observed == old) break;
            old = observed;
        }
    }
    return wait_on_queue(&address_waiters, address, 0, ns, 0);
}
static uint32_t address_signal(uint64_t address, uint32_t type, uint32_t value, int32_t count) {
    if (address & (sizeof(LONG) - 1)) return ADDR_INVALID_ADDRESS;
    if (type > 2) return ADDR_INVALID_TYPE;
    if (!contains(arena, address, sizeof(LONG))) return ADDR_INVALID_MEMORY;
    if (type) {
        if (!guest_write_ok(address, sizeof(LONG))) return ADDR_INVALID_MEMORY;
        uint32_t next = value + 1;
        if (type == 2) {
            int n = 0;
            for (const struct guest_waiter *w = address_waiters; w; w = w->next) n += w->address == address;
            if (n) next = count <= 0 ? value - 2 : n <= count ? value - 1 : value;
        }
        if ((uint32_t)InterlockedCompareExchange((volatile LONG *)(uintptr_t)address, (LONG)next, (LONG)value) != value)
            return ADDR_INVALID_STATE;
    }
    const int wake_all = count <= 0;
    while (wake_all || count > 0) {
        struct guest_waiter **best = pick_waiter(&address_waiters, address, NULL);
        if (!best) break;
        wake_waiter(best);
        if (!wake_all) --count;
    }
    return 0;
}

static uint32_t thread_core(const struct guest_thread *t) {
    if (t->core >= 0) return (uint32_t)t->core;
    uint32_t first = 0;
    for (uint64_t mask = t->core_mask; mask && !(mask & 1); mask >>= 1) ++first;
    return first;
}
static void thread_properties(unsigned num, uint64_t *r) {
    struct guest_thread *t = thread_for_handle((uint32_t)r[num == 0x0d || num == 0x0f ? 0 : num == 0x0e ? 2 : 1]);
    if (!t) { r[0] = 0xe401; return; }
    if (num == 0x0c) r[1] = (uint32_t)t->priority;
    if (num == 0x0d) {
        int32_t p = (int32_t)r[1];
        if (p < 0 || p > 63) { r[0] = 0xee01; return; }
        t->priority = p;
    }
    if (num == 0x0e) { r[1] = (uint32_t)t->core; r[2] = t->core_mask; }
    if (num == 0x0f) {
        int core = (int32_t)r[1];
        uint64_t mask = r[2];
        if (core == -2) { core = 0; mask = 1; }
        if (!mask || (mask & ~process_core_mask) || core < -3 || core >= 64 ||
            (core >= 0 && !(mask & (UINT64_C(1) << core)))) { r[0] = 0xd201; return; }
        if (core == -3) core = t->core;
        t->core_mask = mask;
        t->core = core;
        if (core >= 0 && !(mask & (UINT64_C(1) << core))) { t->core = -1; t->core = (int)thread_core(t); }
    }
    if (num == 0x25) r[1] = t->id;
    r[0] = 0;
}

static int svc(unsigned num, uint64_t *r) {
    switch (num) {
    case 1: set_heap(r); return 1;
    case 2: case 3: {  // SDK RELRO. Never make code or metadata writable.
        if (r[0] & (DM_PAGE - 1)) { r[0] = 0xcc01; return 1; }
        if (!r[1] || (r[1] & (DM_PAGE - 1))) { r[0] = 0xca01; return 1; }
        for (size_t i = 0; i < nmodules; ++i) {
            const struct dm_module *m = &modules[i];
            if (!contains((struct dm_span){(uintptr_t)m->base + m->data, m->key - m->data}, r[0], r[1])) continue;
            if (num == 3) {
                if (r[2] != 0x10 || r[3] != 0x10) return 0;  // Other attributes are not implemented.
                if (nlocks == ARRAYSIZE(permission_locks)) dm_fail("too many RELRO ranges");
                permission_locks[nlocks++] = (struct dm_span){r[0], r[1]};
                r[0] = 0;
                return 1;
            }
            for (size_t j = 0; j < nlocks; ++j) {
                struct dm_span lock = permission_locks[j];
                if (r[0] < lock.base + lock.size && lock.base < r[0] + r[1]) { r[0] = 0xd401; return 1; }
            }
            if (r[2] != 1 && r[2] != 3) { r[0] = 0xd801; return 1; }
            protect((void *)(uintptr_t)r[0], r[1], r[2] == 1 ? PAGE_READONLY : PAGE_READWRITE);
            r[0] = 0;
            return 1;
        }
        r[0] = 0xd401;
        return 1;
    }
    case 4: case 5: move_stack(r, num == 4); return 1;
    case 6: query_memory(r); return 1;
    case 8: thread_create(r); return 1;
    case 9: thread_start(r); return 1;
    case 0x0a: thread_exit(); return 1;
    case 0x0b: {
        int64_t ns = (int64_t)r[0];
        ReleaseSRWLockExclusive(&kernel_lock);
        if (ns > 0) {
            uint64_t ms = timeout_ms((uint64_t)ns);
            while (ms) { DWORD chunk = wait_chunk(ms); Sleep(chunk); ms -= chunk; }
        } else if (!SwitchToThread()) Sleep(0); // Host scheduler, not Horizon priority handoff.
        AcquireSRWLockExclusive(&kernel_lock);
        return 1;
    }
    case 0x0c: case 0x0d: case 0x0e: case 0x0f: case 0x25: thread_properties(num, r); return 1;
    case 0x10: r[0] = thread_core(current_thread); return 1;
    case 0x11: case 0x12: case 0x17: r[0] = change_event(num, (uint32_t)r[0]); return 1;
    case 0x16:
        if (close_event((uint32_t)r[0]) || dm_ipc_close((uint32_t)r[0])) r[0] = 0;
        else thread_close(r);
        return 1;
    case 0x18: thread_wait(r); return 1;
    case 0x19: cancel_sync(r); return 1;
    case 0x1a: r[0] = mutex_lock((uint32_t)r[0], r[1], (uint32_t)r[2]); return 1;
    case 0x1b: r[0] = mutex_unlock(r[0]); return 1;
    case 0x1c: r[0] = process_key_wait(r[0], r[1], (uint32_t)r[2], (int64_t)r[3]); return 1;
    case 0x1d: r[0] = process_key_signal(r[0], (int32_t)r[1]); return 1;
    case 0x1e: r[0] = 0; r[1] = ticks(); return 1;
    case 0x1f: {
        uint32_t handle;
        r[0] = dm_ipc_connect((const char *)(uintptr_t)r[1], &handle, trace);
        r[1] = handle;
        return 1;
    }
    case 0x21: r[0] = dm_ipc_send((uint32_t)r[0], (void *)(uintptr_t)slots.tpidrro, trace); return 1;
    case 0x29: return info(r);
    case 0x2c: case 0x2d: alias_memory(r, num == 0x2c); return 1;
    case 0x34: r[0] = address_wait(r[0], (uint32_t)r[1], r[2], (int64_t)r[3]); return 1;
    case 0x35: r[0] = address_signal(r[0], (uint32_t)r[1], (uint32_t)r[2], (int32_t)r[3]); return 1;
    case 0x45: create_event(r); return 1;
    default: return 0;
    }
}

static void log_guest_pc(uint64_t pc) {
    // Guest addresses are below 2^39; signed LR may contain pointer-auth bits above it.
    pc &= DM_GUEST_LIMIT - 1;
    for (size_t j = 0; j < nmodules; ++j) {
        const struct dm_module *m = &modules[j];
        if (pc >= (uintptr_t)m->base && pc - (uintptr_t)m->base < m->size) {
            fprintf(stderr, " %s+%#" PRIx64, m->name, pc - (uintptr_t)m->base);
            return;
        }
    }
    fprintf(stderr, " %#" PRIx64, pc);
}

static void log_guest_callers(uint64_t sp) {
    if (sp < 16 || !guest_write_ok(sp - 16, 16)) return;
    const uint64_t *saved = (const uint64_t *)(uintptr_t)(sp - 16);
    fputs("guest callers:", stderr);
    log_guest_pc(saved[1]);
    for (uint64_t fp = saved[0], i = 0; i < 16 && !(fp & 7) && guest_write_ok(fp, 16); ++i) {
        const uint64_t *frame = (const uint64_t *)(uintptr_t)fp;
        log_guest_pc(frame[1]);
        if (frame[0] <= fp) break;
        fp = frame[0];
    }
    fputc('\n', stderr);
}

static void guest_break(const uint64_t *r, uint64_t sp) {
    uint32_t result = 0;
    if (r[2] >= sizeof(result) && guest_write_ok(r[1], sizeof(result))) memcpy(&result, (void *)(uintptr_t)r[1], sizeof(result));
    fprintf(stderr, "[RED] Break reason=%#" PRIx64 " Result=%#x\n", r[0], result);
    log_guest_callers(sp);
    if (!(r[0] & 0x80000000u)) exit(2);
}

void m2_dispatch(uint64_t ret, uint64_t *r, uint64_t guest_sp) {
    for (size_t i = 0; i < nmodules; ++i) {
        const struct dm_module *m = &modules[i];
        if (ret < (uintptr_t)m->base || ret - (uintptr_t)m->base >= m->size) continue;
        const struct dm_gate_site *g = dm_find_gate(m, ret - (uintptr_t)m->base);
        if (!g) break;
        if (!current_thread || (guest_sp & 15) || guest_sp < 32 ||
            (!contains(current_thread->stack, guest_sp - 32, 32) && !guest_write_ok(guest_sp - 32, 32))) {
            log_guest_callers(guest_sp);
            dm_fail("gate %s %#x at %s+%#" PRIx64 " called on an invalid guest stack at %#" PRIx64
                    " (thread %#x stack %#" PRIx64 "+%#" PRIx64 ")", g->kind, g->arg, m->name, g->orig,
                    guest_sp, current_thread ? current_thread->handle : 0,
                    current_thread ? current_thread->stack.base : 0, current_thread ? current_thread->stack.size : 0);
        }
        if (!strcmp(g->kind, "svc")) {
            if (trace) {
                printf("svc %#x %s+%#" PRIx64 " x0=%#" PRIx64 " x1=%#" PRIx64 " x2=%#" PRIx64 " thread=%#x\n",
                       g->arg, m->name, g->orig, r[0], r[1], r[2], current_thread->handle);
                fflush(stdout);
            }
            if (g->arg == 0x26) { guest_break(r, guest_sp); return; }
            AcquireSRWLockExclusive(&kernel_lock);
            int handled = svc(g->arg, r);
            ReleaseSRWLockExclusive(&kernel_lock);
            if (handled) return;
        } else if (!strcmp(g->kind, "cntpct") || !strcmp(g->kind, "cntvct")) {
            if (g->arg < 29) {
                if (g->arg == 18) slots.x18 = ticks(); else r[g->arg] = ticks();
                return;
            }
            if (g->arg == 31) return;
        } else if (!strcmp(g->kind, "spin")) { YieldProcessor(); SwitchToThread(); return; }
        // These three gates only observe the SDK's VammManager on the Apple host.
        else if (!strcmp(g->kind, "obs")) {
            unsigned obs = g->arg & 0xff;
            if (obs >= 22 && obs <= 24) return;
            // StandardAllocator uses the supplied memory rather than reserving an alias region.
            if (obs == 25 && r[1]) return;
        }
        log_guest_callers(guest_sp);
        dm_fail("unsupported %s %#x at %s+%#" PRIx64 " x0=%#" PRIx64 " x1=%#" PRIx64 " x2=%#" PRIx64,
                g->kind, g->arg, m->name, g->orig, r[0], r[1], r[2]);
    }
    dm_fail("no gate for PC %#" PRIx64, ret);
}

static uint64_t enter(uintptr_t entry, uint64_t a, uint64_t b) {
    if (!guest_entry(entry)) dm_fail("bootstrap entry outside guest text");
    return m2_enter(entry, a, b, main_thread.stack.base + main_thread.stack.size, &slots.native_sp);
}

static void stage(const char *name, uint64_t a, uint64_t b) {
    uintptr_t entry = dm_find_symbol(modules, nmodules, name);
    if (!entry) dm_fail("missing bootstrap symbol %s", name);
    printf(">>> %s\n", name);
    fflush(stdout);
    enter(entry, a, b);
}

static LONG CALLBACK guest_fault(EXCEPTION_POINTERS *e) {
    // Guest stacks have no Windows unwind chain. Report the precise instruction and stop.
    const CONTEXT *c = e->ContextRecord;
    if (!slots.native_sp || !current_thread || (!contains(current_thread->stack, c->Sp, 0) && !guest_entry(c->Pc)))
        return EXCEPTION_CONTINUE_SEARCH;
    uint64_t addr = e->ExceptionRecord->NumberParameters >= 2 ? e->ExceptionRecord->ExceptionInformation[1] : 0;
    char msg[256];
    const char *name = "outside a module";
    uint64_t off = c->Pc;
    for (size_t i = 0; i < nmodules; ++i) {
        uint64_t base = (uintptr_t)modules[i].base;
        if (c->Pc >= base && c->Pc - base < modules[i].size) { name = modules[i].name; off = c->Pc - base; break; }
    }
    int len = snprintf(msg, sizeof(msg), "[RED] guest exception %#lx at %s+%#" PRIx64 " addr=%#" PRIx64 " lr=%#" PRIx64 "\n",
                       e->ExceptionRecord->ExceptionCode, name, off, addr, (uint64_t)c->Lr);
    DWORD written;
    if (len > 0 && (size_t)len < sizeof(msg)) WriteFile(GetStdHandle(STD_ERROR_HANDLE), msg, len, &written, NULL);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    len = snprintf(msg, sizeof(msg), "thread=%#x sp=%#" PRIx64 " fp=%#" PRIx64 "\n",
                   current_thread->handle, (uint64_t)c->Sp, (uint64_t)c->Fp);
    if (len > 0 && (size_t)len < sizeof(msg)) WriteFile(err, msg, len, &written, NULL);
    for (unsigned i = 0; i < 29; ++i) {
        len = snprintf(msg, sizeof(msg), "x%u=%#" PRIx64 "%c", i, (uint64_t)c->X[i], i % 4 == 3 || i == 28 ? '\n' : ' ');
        if (len > 0 && (size_t)len < sizeof(msg)) WriteFile(err, msg, len, &written, NULL);
    }
    // Generated guest code has no PE unwind records. Walk only ordered
    // frame pairs on this guest's stack, without faulting in the handler.
    uint64_t fp = c->Fp;
    for (unsigned depth = 0; depth < 32 && !(fp & 15) && contains(current_thread->stack, fp, 16); ++depth) {
        uint64_t pair[2];
        SIZE_T copied;
        if (!ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)fp, pair, sizeof(pair), &copied) ||
            copied != sizeof(pair)) break;
        for (size_t i = 0; i < nmodules; ++i) {
            uint64_t base = (uintptr_t)modules[i].base;
            if (pair[1] < base || pair[1] - base >= modules[i].size) continue;
            len = snprintf(msg, sizeof(msg), "  at %s+%#" PRIx64 "\n", modules[i].name, pair[1] - base);
            if (len > 0 && (size_t)len < sizeof(msg)) WriteFile(err, msg, len, &written, NULL);
            break;
        }
        if (pair[0] <= fp) break;
        fp = pair[0];
    }
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Installed control data wins. NUP/dump inputs may lack it: use only the
// display version whose profile row names the loaded main's complete ID.
static void load_version(const char *profile, const char *build) {
    wchar_t *path = NULL;
    _wdupenv_s(&path, NULL, L"DM_CONTROL_NACP");
    FILE *f = NULL;
    if (path) {
        _wfopen_s(&f, path, L"rb");
    } else {
        wchar_t *rom = NULL;
        _wdupenv_s(&rom, NULL, L"DM_ROMFS");
        if (rom) {
            size_t prefix = wcslen(rom);
            while (prefix && rom[prefix - 1] != L'\\' && rom[prefix - 1] != L'/') --prefix;
            static const wchar_t control[] = L"control.nacp";
            path = malloc(prefix * sizeof(*rom) + sizeof(control));
            if (!path) dm_fail("cannot allocate control path");
            memcpy(path, rom, prefix * sizeof(*rom));
            memcpy(path + prefix, control, sizeof(control));
            _wfopen_s(&f, path, L"rb");
            free(rom);
        }
    }
    free(path);
    if (f) {
        char field[16];
        int found = !fseek(f, 0x3060, SEEK_SET) && fread(field, 1, sizeof(field), f) == sizeof(field) && field[0];
        fclose(f);
        if (found) {
            field[15] = 0;
            strcpy_s(oe_version, sizeof(oe_version), field);
            return;
        }
    }
    if (!build[0] || fopen_s(&f, profile, "r") || !f) return;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "build\t", 6)) continue;
        char *ctx = NULL, *ver = strtok_s(line + 6, "\t\r\n", &ctx);
        if (!ver || !(ver = strtok_s(NULL, "\t\r\n", &ctx))) continue;
        for (char *mod; (mod = strtok_s(NULL, "\t\r\n", &ctx));) {
            if (!strncmp(mod, "main=", 5) && !strcmp(mod + 5, build)) {
                strncpy_s(oe_version, sizeof(oe_version), ver, _TRUNCATE);
                fclose(f);
                return;
            }
        }
    }
    fclose(f);
}
static void read_profile(const char *path, uint64_t *size, uint64_t *stack, uint64_t *alias_size) {
    FILE *f;
    if (fopen_s(&f, path, "r")) dm_fail("cannot open profile %s", path);
    char line[4096], key[80], value[80];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf_s(line, "%79s %79s", key, (unsigned)sizeof(key), value, (unsigned)sizeof(value)) != 2) continue;
        key[sizeof(key) - 1] = value[sizeof(value) - 1] = '\0';
        uint64_t n = strtoull(value, NULL, 0);
        if (!strcmp(key, "mem_reserve")) *size = n;
        if (!strcmp(key, "mem_stack")) *stack = n;
        if (!strcmp(key, "mem_alias")) *alias_size = n;
        if (!strcmp(key, "app_pool")) app_pool = n;
        if (!strcmp(key, "mem_used")) mem_used = n;
        if (!strcmp(key, "sys_resource")) sys_resource = n;
        if (!strcmp(key, "program_id")) program_id = strtoull(value, NULL, 16);
        if (!strcmp(key, "mem_map_limit")) alias_counts = !strcmp(value, "on");
        if (!strcmp(key, "op_mode")) oe_operation_mode = !strcmp(value, "console") || n == 1;
        if (!strcmp(key, "perf_mode")) oe_perf_boost = !strcmp(value, "boost");
    }
    fclose(f);
    if ((program_id >> 56) == 4) {
        tick_rate = 31250000;
        process_core_mask = 0x3f; // Six application cores on Ounce, independent of the Windows CPU count.
    }
}

int main(int argc, char **argv) {
    USHORT proc, native;
    if (!IsWow64Process2(GetCurrentProcess(), &proc, &native) || proc != IMAGE_FILE_MACHINE_UNKNOWN || native != IMAGE_FILE_MACHINE_ARM64)
        dm_fail("requires a native Windows ARM64 executable");
    if (argc < 4 || argc - 3 > ARRAYSIZE(modules))
        dm_fail("usage: m2_host <game.tsv|--fixture> <rewrite dir> <module>... (in load order)");
    m4_phase_init();
    fixture_mode = !strcmp(argv[1], "--fixture");
    char *setting = NULL;
    if (!_dupenv_s(&setting, NULL, "DM_SVC")) trace = setting && *setting;
    free(setting);
    LARGE_INTEGER q;
    if (!QueryPerformanceCounter(&q)) dm_fail("cannot read guest QPC clock");
    clock_start = (uint64_t)q.QuadPart;
    QueryUnbiasedInterruptTimePrecise(&active_time_origin);
    // The fractional-second product in scaled_guest_clock must fit at ns resolution.
    if (!QueryPerformanceFrequency(&q) || q.QuadPart <= 0 ||
        (uint64_t)q.QuadPart > UINT64_MAX / UINT64_C(1000000000))
        dm_fail("invalid guest QPC clock frequency");
    clock_freq = (uint64_t)q.QuadPart;
    // Per-process entropy returned by GetInfo's four documented sub-IDs.
    if (BCryptGenRandom(NULL, (unsigned char *)entropy, sizeof(entropy), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        dm_fail("cannot initialize guest entropy");
    uint64_t size = 0x140000000, alias_size = 0x118000000, stack = 0x10000000;
    if (!fixture_mode) read_profile(argv[1], &size, &stack, &alias_size);
    char env_lang[sizeof(oe_language)];
    DWORD lang_len = GetEnvironmentVariableA("DM_LANG", env_lang, ARRAYSIZE(env_lang));
    if (lang_len && lang_len < ARRAYSIZE(env_lang)) {
        memset(oe_language, 0, sizeof(oe_language));
        memcpy(oe_language, env_lang, lang_len);
    }
    char mode[2];
    if (GetEnvironmentVariableA("DM_OPMODE", mode, ARRAYSIZE(mode)) == 1 && (mode[0] == '0' || mode[0] == '1'))
        oe_operation_mode = mode[0] - '0';
    reserve(size, stack, alias_size);
    unsigned char **tls;
    memcpy(&tls, (const unsigned char *)NtCurrentTeb() + 0x58, sizeof(tls));
    uintptr_t base = (uintptr_t)tls[_tls_index];
    const uint64_t keys[5] = { (uintptr_t)&slots.x18 - base, (uintptr_t)&slots.tpidr - base,
        (uintptr_t)&slots.tpidrro - base, (uintptr_t)&slots.gate - base, (uint64_t)_tls_index * 8 };
    m2_native_sp_offset = (uintptr_t)&slots.native_sp - base;
    uint64_t next = arena.base;
    nmodules = (size_t)(argc - 3);
    for (size_t i = 0; i < nmodules; ++i) {
        modules[i].name = argv[3 + i];
        dm_module_load(&modules[i], argv[2], (void *)(uintptr_t)next, (size_t)(heap.base - next), keys);
        if (!fixture_mode && !strcmp(modules[i].name, "main")) load_version(argv[1], modules[i].build_id);
        if (!strcmp(modules[i].name, "sdk")) dm_m3_nvn_load(&modules[i]);
        next += (modules[i].size + 0xffff) & ~UINT64_C(0xffff);
    }
    service_base = next;
    for (size_t i = 0; i < nmodules; ++i) dm_module_relocate(&modules[i], modules, nmodules);
    for (size_t i = 0; i < nmodules; ++i) dm_module_seal(&modules[i]);
    InterlockedIncrement64(&vm_generation);
    if (!AddVectoredExceptionHandler(1, guest_fault)) dm_fail("cannot install guest fault handler");
    if (fixture_mode) {
        uint64_t r = enter((uintptr_t)modules[0].base + 0x100, 0, 0);
        if (r) dm_fail("fixture returned %#" PRIx64, r);
        puts("Windows ARM64 fixture returned 0");
        return 0;
    }
    stage("_init_libc0", 0, 0);
    stage("nnosInitialize", main_thread.handle, 0);
    stage("_init_libc1", 0, 0);
    stage("nninitInitializeSdkModule", 0, 0);
    uint64_t args = exception_addr + 0x300;
    memcpy((void *)(uintptr_t)(args + 16), "game", 5);
    *(uint64_t *)(uintptr_t)args = args + 16;
    stage("_ZN2nn2os11SetHostArgcEi", 1, 0);
    stage("_ZN2nn2os11SetHostArgvEPPc", args, 0);
    stage("nninitStartup", 0, 0);
    stage("_init_libc2", 0, 0);
    for (size_t i = nmodules; i-- > 0;)
        if (modules[i].init) enter((uintptr_t)modules[i].base + modules[i].init, 0, 0);
    stage("nnMain", 0, 0);
    puts("nnMain returned");
    return 0;
}
