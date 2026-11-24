// Authoritative host records; shader-address translation never walks pool
// objects or follows a guest pointer to an arbitrary CPU allocation.
struct nvn_live_pool {
    struct nvn_live_pool *next;
    const struct pool *object;
    const struct nvn_device *device;
    struct dm_m4_gmem_span span;
    uint32_t flags;
    volatile LONG64 writable_generation;
};
// Order by host backing so cold range checks can reuse forward VirtualQuery
// extents. Unmapped virtual pools are last and never reach those checks.
static struct nvn_live_pool *live_pools;

// Call with pool_alias_lock held exclusively.
static struct nvn_live_pool **live_pool_link(const struct pool *object)
{
    struct nvn_live_pool **link = &live_pools;
    while (*link && (*link)->object != object) link = &(*link)->next;
    return link;
}

// Under the exclusive lock, find the sorted slot and reject a duplicate
// object in one pass. Pass NULL when reinserting an already unlinked entry.
static struct nvn_live_pool **live_pool_insert_link(const struct pool *object, const void *host)
{
    struct nvn_live_pool **link = &live_pools;
    while (*link && (!host || ((*link)->span.host && (uintptr_t)(*link)->span.host <= (uintptr_t)host))) {
        if ((*link)->object == object) return NULL;
        link = &(*link)->next;
    }
    if (object) {
        for (const struct nvn_live_pool *p = *link; p; p = p->next)
            if (p->object == object) return NULL;
    }
    return link;
}

static struct dm_m4_gmem_span nvn_gmem_span(void *host, size_t size)
{
    // NVN buffer address getters expose this numeric guest VA. Vulkan device
    // addresses are obtained separately and never leave the replay backend.
    // visit_live_pools validates the guest arena. The shared replay lock and
    // guest VM retirement hooks also cover non-pool pages in that reservation.
    return (struct dm_m4_gmem_span){(uintptr_t)host, host, size, 1};
}

// Replay holds the shared pool lock until its fence and readback.
static int visit_live_pools(const struct nvn_device *device, dm_m4_gmem_accept accept, void *userdata)
{
    const LONG64 generation = (LONG64)dm_guest_access_generation();
    for (struct nvn_live_pool *p = live_pools; p && p->span.host; p = p->next) {
        if (p->device != device || (p->flags & NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS)) continue;
        // A large homogeneous Windows extent can take milliseconds to query.
        // The pool lock fixes the span; the display lock excludes revocation
        // until GPU completion. Never cache a negative or a partial proof.
        if (!generation || ReadAcquire64(&p->writable_generation) != generation) {
            if (!dm_guest_range(p->span.host, p->span.size, 1)) return 0;
            WriteRelease64(&p->writable_generation, generation);
        }
        if (!accept(userdata, p->span)) return 0;
    }
    return 1;
}
