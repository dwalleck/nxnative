// Kernel events have independent readable and writable handles. Waits retain
// the object, not a handle-table slot; closing a handle cannot recycle it
// underneath a waiter. Internal operations require kernel_lock; the dm_*
// service wrappers acquire it.
struct guest_event { uint64_t signals; uint32_t refs; int set; };
struct event_end { struct guest_event *event; uint32_t handle; int writable; };
static struct event_end *event_ends;
static size_t nends, end_capacity;

static struct event_end *event_end(uint32_t h) {
    for (size_t i = 0; i < nends; ++i) if (event_ends[i].handle == h) return &event_ends[i];
    return NULL;
}
static uint32_t add_event_end(struct guest_event *e, int writable) {
    if (nends == end_capacity) {
        size_t n = end_capacity ? end_capacity * 2 : 16;
        if (n > SIZE_MAX / sizeof(*event_ends)) return 0;
        struct event_end *new_ends = realloc(event_ends, n * sizeof(*event_ends));
        if (!new_ends) return 0;
        event_ends = new_ends;
        end_capacity = n;
    }
    uint32_t h = dm_alloc_handle();
    if (h) { event_ends[nends++] = (struct event_end){e, h, writable}; ++e->refs; }
    return h;
}
static void release_event(struct guest_event *e) { if (!--e->refs) free(e); }
static int close_event(uint32_t h) {
    struct event_end *end = event_end(h);
    if (!end) return 0;
    release_event(end->event);
    *end = event_ends[--nends];
    return 1;
}
static void create_event(uint64_t *r) {
    struct guest_event *e = calloc(1, sizeof(*e));
    if (!e) { r[0] = 0xce01; return; }
    uint32_t wr = add_event_end(e, 1), rd = wr ? add_event_end(e, 0) : 0;
    if (!rd) {
        if (wr) close_event(wr); else free(e);
        r[0] = 0xd201;
        return;
    }
    r[0] = 0; r[1] = wr; r[2] = rd;
}
uint32_t dm_dup_readable_event(uint32_t handle) {
    AcquireSRWLockExclusive(&kernel_lock);
    struct event_end *end = event_end(handle);
    uint32_t dup = end && !end->writable ? add_event_end(end->event, 0) : 0;
    ReleaseSRWLockExclusive(&kernel_lock);
    return dup;
}
static uint32_t change_event(unsigned svc_number, uint32_t handle) {
    struct event_end *end = event_end(handle);
    if (!end || (svc_number == 0x11 && !end->writable) || (svc_number == 0x17 && end->writable)) return 0xe401;
    struct guest_event *e = end->event;
    if (svc_number == 0x11) {
        e->set = 1;
        ++e->signals; // Already-enrolled waiters complete even if another thread resets before they resume.
        WakeAllConditionVariable(&thread_changed);
    } else {
        if (svc_number == 0x17 && !e->set) return 0xfa01;
        e->set = 0;
    }
    return 0;
}

static uint32_t service_event(unsigned svc_number, uint32_t handle) {
    AcquireSRWLockExclusive(&kernel_lock);
    uint32_t result = change_event(svc_number, handle);
    ReleaseSRWLockExclusive(&kernel_lock);
    return result;
}
uint32_t dm_signal_event(uint32_t writable) { return service_event(0x11, writable); }
uint32_t dm_clear_event(uint32_t writable) { return service_event(0x12, writable); }
