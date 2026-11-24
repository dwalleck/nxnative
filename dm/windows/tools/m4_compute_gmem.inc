// Give each live run one backing, so raw pointers and ordinary buffer
// descriptors see the same device bytes whether imported or staged.
enum {
    GMEM_ALIGN = 16,
    GMEM_MAP_BYTES = DMSH_GMEM_MAP_ENTRIES * DMSH_GMEM_MAP_ENTRY,
    GMEM_READ_ALIAS_DELTA = 1, // real device address deltas are 16-byte aligned
    GMEM_LOG_BYTES = DMSH_GMEM_MAP_ENTRIES / 8,
};

struct gmem_entry {
    uint64_t start, end, delta;
    uint32_t hint, span;
};
static_assert(sizeof(struct gmem_entry) == DMSH_GMEM_MAP_ENTRY, "raw shader map ABI");

static size_t gmem_map_size(int read_alias)
{
    return GMEM_MAP_BYTES + (read_alias ? sizeof(uint32_t) : 0);
}

struct gmem_collect {
    struct dm_m4_draw *draw;
    VkResult result;
    const char **step;
    int unordered;
};

static int gmem_collect_span(void *userdata, struct dm_m4_gmem_span span)
{
    struct gmem_collect *c = userdata;
    struct dm_m4_draw *d = c->draw;
    const uintptr_t host = (uintptr_t)span.host;
    // The shader gates natural alignment; no aligned scalar/vector access
    // whose first byte is in this range can then straddle either edge.
    if (!span.guest || !host || !span.size || (span.guest | host | span.size) % GMEM_ALIGN ||
        span.size > UINT64_MAX - span.guest || span.size > UINTPTR_MAX - host ||
        (d->gmem_count && host - span.guest !=
            (uintptr_t)d->gmem_ranges[0].span.host - d->gmem_ranges[0].span.guest)) {
        c->result = failure(c->step, "invalid or non-affine live NVN global memory span", VK_ERROR_FEATURE_NOT_PRESENT);
        return 0;
    }
    if (d->gmem_count == UINT32_MAX) {
        c->result = failure(c->step, "too many live NVN global memory spans", VK_ERROR_OUT_OF_HOST_MEMORY);
        return 0;
    }
    void *ranges = d->gmem_ranges;
    c->result = grow_array(&ranges, sizeof(*d->gmem_ranges), d->gmem_count + 1, &d->gmem_capacity, 0);
    if (c->result != VK_SUCCESS) {
        (void)failure(c->step, "grow compute global memory spans", c->result);
        return 0;
    }
    d->gmem_ranges = ranges;
    if (d->gmem_count && span.guest < d->gmem_ranges[d->gmem_count - 1].span.guest)
        c->unordered = 1;
    d->gmem_ranges[d->gmem_count++] = (struct gmem_range){.span = span};
    return 1;
}

static int gmem_compare(const void *lhs, const void *rhs)
{
    const struct gmem_range *a = lhs, *b = rhs;
    return a->span.guest > b->span.guest ? 1 : a->span.guest < b->span.guest ? -1 : 0;
}

static struct gmem_range *gmem_host_range(const struct dm_m4_draw *d, const void *ptr, size_t size)
{
    if (!size) return NULL;
    // Search the first range whose end is above ptr; it may only partially
    // overlap. Their guest and host address order is the same.
    const uintptr_t at = (uintptr_t)ptr;
    uint32_t lo = 0, hi = d->gmem_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const struct dm_m4_gmem_span *s = &d->gmem_ranges[mid].span;
        if ((uintptr_t)s->host + s->size <= at) lo = mid + 1;
        else hi = mid;
    }
    if (lo == d->gmem_count) return NULL;
    struct gmem_range *r = &d->gmem_ranges[lo];
    return overlap(ptr, size, r->span.host, r->span.size) ? r : NULL;
}

static VkResult gmem_prepare(struct dm_m4_draw *d, const struct dm_m4_gmem_source *source,
                            uint32_t flags, int read_alias, VkDeviceSize *used, const char **step)
{
    d->gmem_count = 0;
    d->gmem_writes = (flags & 2u) != 0;
    d->gmem_read_alias = read_alias;
    d->gmem_read_protected = 0;
    if (!flags && !read_alias) return VK_SUCCESS;
    if ((flags != 1u && flags != 3u) || (read_alias && flags != 1u))
        return failure(step, "invalid global memory map/log flags", VK_ERROR_INVALID_SHADER_NV);
    if (!d->GetBufferDeviceAddress)
        return failure(step, "raw global memory requires enabled bufferDeviceAddress", VK_ERROR_FEATURE_NOT_PRESENT);
    if (!source->visit || (source->view_count && !source->views))
        return failure(step, "raw global memory has no live pool resolver", VK_ERROR_INITIALIZATION_FAILED);
    for (uint32_t i = 0; i < source->view_count; ++i) {
        const struct dm_m4_gmem_view *view = &source->views[i];
        if (!view->data || !view->size || view->size > UINTPTR_MAX - (uintptr_t)view->data)
            return failure(step, "invalid separately staged global memory view", VK_ERROR_INITIALIZATION_FAILED);
    }
    const size_t map_size = gmem_map_size(read_alias);
    if (map_size > d->limits.maxStorageBufferRange)
        return failure(step, "global memory map exceeds storage buffer range", VK_ERROR_FEATURE_NOT_PRESENT);
    struct gmem_collect collect = {.draw = d, .result = VK_SUCCESS, .step = step};
    if (!source->visit(source->context, gmem_collect_span, &collect)) {
        if (collect.result != VK_SUCCESS) return collect.result;
        return failure(step, "raw global memory includes an unmapped live NVN pool", VK_ERROR_INITIALIZATION_FAILED);
    }
    if (collect.unordered) qsort(d->gmem_ranges, d->gmem_count, sizeof *d->gmem_ranges, gmem_compare);
    // Native NVN virtual mappings alias their physical host storage at the
    // same guest-to-host translation. Fold overlaps and adjacency, not holes.
    uint32_t runs = 0;
    for (uint32_t i = 0; i < d->gmem_count; ++i) {
        const struct gmem_range *r = &d->gmem_ranges[i];
        if (runs) {
            struct dm_m4_gmem_span *prior = &d->gmem_ranges[runs - 1].span;
            if (r->span.guest <= prior->guest + prior->size) {
                const uint64_t end = r->span.guest + r->span.size;
                if (end > prior->guest + prior->size) prior->size = (size_t)(end - prior->guest);
                prior->stable_private_backing &= r->span.stable_private_backing;
                continue;
            }
        }
        d->gmem_ranges[runs++] = *r;
    }
    d->gmem_count = runs;
    if (runs >= DMSH_GMEM_MAP_ENTRIES)
        return failure(step, "compute global memory map exhausted", VK_ERROR_FEATURE_NOT_PRESENT);

    const VkDeviceSize storage_align = d->limits.minStorageBufferOffsetAlignment;
    const VkDeviceSize map_align = storage_align < _Alignof(struct gmem_entry) ? _Alignof(struct gmem_entry) : storage_align;
    if (!reserve(used, map_align, map_size, &d->gmem_map_offset) ||
        (d->gmem_writes && !reserve(used, map_align, GMEM_LOG_BYTES, &d->gmem_log_offset)))
        return failure(step, "compute global memory descriptor size overflow", VK_ERROR_OUT_OF_HOST_MEMORY);
    return VK_SUCCESS;
}

static VkDeviceSize gmem_binding_alignment(const struct dm_m4_draw *d, const struct draw_binding *binding)
{
    if (binding->texel_format != VK_FORMAT_UNDEFINED) return d->limits.minTexelBufferOffsetAlignment;
    return binding->storage ? d->limits.minStorageBufferOffsetAlignment : d->limits.minUniformBufferOffsetAlignment;
}

static void gmem_use_import(struct gmem_range *r, const struct gmem_import *entry)
{
    r->offset = (uintptr_t)r->span.host - entry->begin;
    r->imported = entry->buffer;
    r->imported_address = entry->address + r->offset;
}

static void gmem_reuse_imports(struct dm_m4_draw *d, int extra)
{
    // Validate each cached window against every live run it intersects
    // before assigning it. Subsequent windows are disjoint and cannot evict it.
    uint32_t run = 0;
    for (uint32_t i = 0; i < d->gmem_import_count;) {
        const struct gmem_import *entry = &d->gmem_imports[i];
        while (run < d->gmem_count &&
               (uintptr_t)d->gmem_ranges[run].span.host + d->gmem_ranges[run].span.size <= entry->begin)
            ++run;
        uint32_t j = run;
        for (; j < d->gmem_count; ++j) {
            const struct dm_m4_gmem_span *s = &d->gmem_ranges[j].span;
            const uintptr_t begin = (uintptr_t)s->host, end = begin + s->size;
            if (begin >= entry->end) break;
            if (begin < entry->begin || end > entry->end ||
                ((!entry->widened || !s->stable_private_backing) &&
                 (begin != entry->begin || end != entry->end)))
                break;
        }
        if (j == run || (j < d->gmem_count && (uintptr_t)d->gmem_ranges[j].span.host < entry->end)) {
            gmem_drop_imports(d, i, entry->end);
            continue;
        }
        for (; run < j; ++run) {
            struct gmem_range *r = &d->gmem_ranges[run];
            if (r->stage_only) continue;
            const uintptr_t begin = (uintptr_t)r->span.host;
            const int exact = entry->begin == begin && entry->end == begin + r->span.size;
            if ((!extra || !r->span.stable_private_backing) && (!exact || r->span.size < GMEM_IMPORT_MIN))
                continue;
            if (!r->descriptor_alignment || (begin - entry->begin) % r->descriptor_alignment == 0)
                gmem_use_import(r, entry);
        }
        ++i;
    }
}

static int gmem_exact_candidate(const struct dm_m4_draw *d, const struct gmem_range *r, uintptr_t align)
{
    return r->span.size <= d->gmem_import_max_size &&
        (uintptr_t)r->span.host % align == 0 && r->span.size % align == 0;
}

static int gmem_group_member(const struct gmem_range *r, uintptr_t import_begin)
{
    return !r->stage_only && !r->imported && r->span.stable_private_backing &&
        (!r->descriptor_alignment || ((uintptr_t)r->span.host - import_begin) % r->descriptor_alignment == 0);
}

static struct gmem_candidate gmem_window(struct dm_m4_draw *d, uint32_t first, uintptr_t align)
{
    const struct gmem_range *r = &d->gmem_ranges[first];
    const uintptr_t host = (uintptr_t)r->span.host, begin = host - host % align;
    struct gmem_candidate window = {.first = first, .last = first + 1};
    if (first) {
        const struct dm_m4_gmem_span *previous = &d->gmem_ranges[first - 1].span;
        if ((uintptr_t)previous->host + previous->size > begin) return window;
    }

    uintptr_t bound = 0, live_end = host;
    size_t live = 0;
    for (uint32_t i = first; i < d->gmem_count; ++i) {
        const struct gmem_range *next = &d->gmem_ranges[i];
        const uintptr_t next_host = (uintptr_t)next->span.host, end = next_host + next->span.size;
        // A later faultable raw store may use a large exact import, but not
        // a widened one. Preserve that backing across alternating dispatches.
        if (next->span.size >= GMEM_IMPORT_MIN && gmem_exact_candidate(d, next, align))
            break;
        if (next_host - live_end > GMEM_IMPORT_MAX_GAP || !gmem_group_member(next, begin))
            break;
        const uintptr_t rem = end % align, tail = rem ? align - rem : 0;
        if (tail > UINTPTR_MAX - end || end + tail - begin > d->gmem_import_max_size)
            break;
        const size_t bytes = live + next->span.size;
        const size_t non_live = end + tail - begin - bytes;
        const int dense = non_live <= bytes / 32 ||
            (non_live <= GMEM_IMPORT_MAX_GAP && non_live <= bytes);
        // The small-window exception counts all gaps and edge padding.
        // It costs at most one extra max-gap per cache slot, and never
        // more non-live backing than live bytes saved.
        if (!dense && i != first) break;
        if (dense && (i != first || begin != host || tail)) {
            if (!bound) {
                bound = gmem_private_extent(d, begin);
                const uint32_t overlap = gmem_first_overlap(d, begin);
                if (overlap < d->gmem_import_count && d->gmem_imports[overlap].begin < bound)
                    bound = d->gmem_imports[overlap].begin;
            }
            if (end + tail > bound) break;
            if (i + 1 == d->gmem_count || (uintptr_t)d->gmem_ranges[i + 1].span.host >= end + tail)
                window = (struct gmem_candidate){begin, end + tail, bytes, first, i + 1};
        }
        live_end = end;
        live = bytes;
    }
    return window;
}

static int gmem_compare_candidates(const void *lhs, const void *rhs)
{
    const struct gmem_candidate *a = lhs, *b = rhs;
    if (a->live_bytes != b->live_bytes) return a->live_bytes > b->live_bytes ? -1 : 1;
    return a->begin > b->begin ? 1 : a->begin < b->begin ? -1 : 0;
}

static VkResult gmem_add_candidate(struct dm_m4_draw *d, struct gmem_candidate c, uint32_t *count)
{
    void *candidates = d->gmem_candidates;
    VkResult result = grow_array(&candidates, sizeof c, *count + 1, &d->gmem_candidate_capacity, 0);
    if (result != VK_SUCCESS) return result;
    d->gmem_candidates = candidates;
    d->gmem_candidates[(*count)++] = c;
    return VK_SUCCESS;
}

static VkResult gmem_select_imports(struct dm_m4_draw *d, int extra, uint32_t *planned,
                                   uint32_t *rejected, const char **step)
{
    const uint32_t budget = gmem_import_budget(d);
    if (!budget) return VK_SUCCESS;
    gmem_reuse_imports(d, extra);
    if (d->gmem_import_count >= budget) return VK_SUCCESS;

    uintptr_t align = (uintptr_t)d->device->host_import_alignment;
    if (align < GMEM_ALIGN) align = GMEM_ALIGN;
    d->gmem_private_begin = d->gmem_private_end = 0;
    uint32_t group_end = 0;
    for (uint32_t i = 0; i < d->gmem_count; ++i) {
        const struct gmem_range *r = &d->gmem_ranges[i];
        if (r->stage_only || r->imported) continue;
        const int broaden = extra && r->span.stable_private_backing;
        if ((broaden || r->span.size >= GMEM_IMPORT_MIN) && gmem_exact_candidate(d, r, align)) {
            const uintptr_t begin = (uintptr_t)r->span.host;
            const struct gmem_candidate exact = {begin, begin + r->span.size, r->span.size, i, i + 1};
            const VkResult result = gmem_add_candidate(d, exact, planned);
            if (result != VK_SUCCESS) return failure(step, "plan exact global memory import", result);
        }
        if (broaden && i >= group_end) {
            const struct gmem_candidate window = gmem_window(d, i, align);
            group_end = window.last;
            if (window.end) {
                const VkResult result = gmem_add_candidate(d, window, planned);
                if (result != VK_SUCCESS) return failure(step, "plan grouped global memory import", result);
            }
        }
    }
    if (*planned > 1) qsort(d->gmem_candidates, *planned, sizeof(*d->gmem_candidates), gmem_compare_candidates);
    // Rank by live bytes saved. A rejected widened attempt must not use
    // up the exact fallback's attempt budget. Only successful imports
    // occupy cache slots, and widened rejections are separately bounded.
    uint32_t attempts = 0, widened_rejections = 0;
    for (uint32_t i = 0; i < *planned && d->gmem_import_count < budget && attempts < budget; ++i) {
        const struct gmem_candidate *candidate = &d->gmem_candidates[i];
        if (d->gmem_ranges[candidate->first].imported) continue;
        const int widened = candidate->end - candidate->begin != candidate->live_bytes;
        if (widened && widened_rejections == budget) continue;
        const struct gmem_import *entry;
        const VkResult result = gmem_import(d, candidate, &entry, step);
        if (result != VK_SUCCESS) return result;
        if (!entry) {
            if (widened) ++widened_rejections;
            else ++attempts;
            ++*rejected;
            continue;
        }
        ++attempts;
        for (uint32_t j = candidate->first; j < candidate->last; ++j)
            gmem_use_import(&d->gmem_ranges[j], entry);
    }
    return VK_SUCCESS;
}

static VkResult gmem_backing(struct dm_m4_draw *d, const struct dm_m4_gmem_source *source,
                            uint32_t bindings, VkDeviceSize *used, VkBufferUsageFlags *usage, const char **step)
{
    const uint32_t runs = d->gmem_count;
    if (!runs) return VK_SUCCESS;
    int binding_fault = 0, raw_write_fault = 0;
    for (uint32_t i = 0; i < bindings; ++i)
        binding_fault |= d->bindings[i].bindless_map_binding != 0;
    if (d->gmem_writes) {
        // A direct GPU write cannot be rolled back. Keep every run touching a
        // separately staged view in staging until its precise log is checked.
        for (uint32_t i = 0; i < source->view_count; ++i) {
            const struct dm_m4_gmem_view *view = &source->views[i];
            const uintptr_t end = (uintptr_t)view->data + view->size;
            struct gmem_range *r = gmem_host_range(d, view->data, view->size);
            if (!r) continue;
            raw_write_fault = 1;
            for (; r != d->gmem_ranges + runs && (uintptr_t)r->span.host < end; ++r)
                r->stage_only = 1;
        }
    }
    for (uint32_t i = 0; i < bindings; ++i) {
        struct draw_binding *binding = &d->bindings[i];
        if (!binding->data || !binding->size) continue;
        struct gmem_range *r = gmem_host_range(d, binding->data, binding->size);
        if (!r) continue;
        const uintptr_t base = (uintptr_t)r->span.host, at = (uintptr_t)binding->data;
        if (at < base || binding->buffer.range != binding->size || binding->size > r->span.size - (at - base))
            return failure(step, "compute buffer only partly or incompatibly aliases raw global memory", VK_ERROR_FEATURE_NOT_PRESENT);
        binding->gmem_alias = (int)(r - d->gmem_ranges) + 1;
        // An ordinary descriptor can still write in a raw-read-only shader.
        // If the dispatch could fault, these writes must not bypass readback.
        if ((d->gmem_read_protected || binding_fault) && binding->writeback) r->stage_only = 1;
        const VkDeviceSize descriptor_align = gmem_binding_alignment(d, binding);
        if (r->descriptor_alignment < descriptor_align) r->descriptor_alignment = descriptor_align;
        if (binding->bindless_map_binding || (at - base) % descriptor_align ||
            (binding->texel_format != VK_FORMAT_UNDEFINED && !binding->import_texel))
            r->stage_only = 1;
    }

    uint32_t planned = 0, rejected = 0;
    // Do not extend direct raw stores to new ranges when this dispatch can
    // reject them after execution. Read-only raw accesses may still share.
    const int extra = !(d->gmem_writes && (raw_write_fault || binding_fault));
    const VkResult result = gmem_select_imports(d, extra, &planned, &rejected, step);
    if (result != VK_SUCCESS) return result;

    VkDeviceSize imported_bytes = 0, backing_bytes = 0, staged_bytes = 0;
    uint32_t imported = 0, backings = 0, backing_index = 0;
    VkBuffer prior = VK_NULL_HANDLE;
    const VkDeviceSize storage_align = d->limits.minStorageBufferOffsetAlignment;
    VkDeviceSize align = d->limits.minUniformBufferOffsetAlignment;
    if (align < storage_align) align = storage_align;
    if (align < d->limits.minTexelBufferOffsetAlignment) align = d->limits.minTexelBufferOffsetAlignment;
    if (align < GMEM_ALIGN) align = GMEM_ALIGN;
    for (uint32_t i = 0; i < runs; ++i) {
        struct gmem_range *r = &d->gmem_ranges[i];
        if (r->imported) {
            ++imported;
            imported_bytes += r->span.size;
            if (prior != r->imported) {
                const uintptr_t host = (uintptr_t)r->span.host;
                while (d->gmem_imports[backing_index].end <= host) ++backing_index;
                const struct gmem_import *entry = &d->gmem_imports[backing_index];
                prior = r->imported;
                ++backings;
                backing_bytes += entry->end - entry->begin;
            }
            continue;
        }
        const VkDeviceSize prefix = (uintptr_t)r->span.host % align;
        // Preserve descriptor offset alignment for subranges of this pool.
        if (r->span.size > SIZE_MAX - prefix || !reserve(used, align, prefix + r->span.size, &r->offset))
            return failure(step, "compute global memory staging size overflow", VK_ERROR_OUT_OF_HOST_MEMORY);
        r->offset += prefix;
        staged_bytes += r->span.size;
    }
    if (staged_bytes) *usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const unsigned report = imported > backings ? 4u : imported ? 2u : 1u;
    if (d->gmem_trace || !(d->gmem_reported & report) ||
        (staged_bytes > d->gmem_reported_staged && staged_bytes / 2 >= d->gmem_reported_staged)) {
        d->gmem_reported |= report;
        if (d->gmem_reported_staged < staged_bytes) d->gmem_reported_staged = staged_bytes;
        printf("[nvn] checked global memory: imported %u ranges, %llu bytes; staged %u ranges, %llu bytes; "
               "%u backing buffers, %llu bytes; %u new candidates, %u rejected; host alignment %llu\n",
               (unsigned)imported, (unsigned long long)imported_bytes,
               (unsigned)(runs - imported), (unsigned long long)staged_bytes,
               (unsigned)backings, (unsigned long long)backing_bytes, (unsigned)planned, (unsigned)rejected,
               (unsigned long long)d->device->host_import_alignment);
        fflush(stdout);
    }
    return VK_SUCCESS;
}

static VkResult gmem_plan_buffer(struct dm_m4_draw *d, struct draw_binding *binding, const char **step)
{
    const struct gmem_range *r = &d->gmem_ranges[binding->gmem_alias - 1];
    const VkDeviceSize offset = r->offset + ((uintptr_t)binding->data - (uintptr_t)r->span.host);
    if (offset % gmem_binding_alignment(d, binding))
        return failure(step, "compute buffer only partly or incompatibly aliases raw global memory", VK_ERROR_FEATURE_NOT_PRESENT);
    binding->buffer.buffer = r->imported;
    binding->buffer.offset = offset;
    return VK_SUCCESS;
}

static int gmem_split_view(uintptr_t start, size_t size, uintptr_t pos, uintptr_t *next)
{
    if (start >= *next || !size || (start <= pos && size <= pos - start)) return 0;
    const uintptr_t mask = GMEM_ALIGN - 1;
    // Natural alignment (including a subword's containing word) keeps every
    // checked access within one 16-byte block. Mark the blocks a view touches.
    const uintptr_t first = start & ~mask;
    if (first > pos) {
        *next = first;
        return 0;
    }
    // next is aligned, so rounding an end below it cannot overflow.
    if (size < *next - start) *next = (start + size + mask) & ~mask;
    return 1;
}

static size_t gmem_plane_size(const struct draw_transfer *t)
{
    return t->row + (t->rows - 1) * t->pitch;
}

static int gmem_split_plane(const struct draw_storage_image *image, uintptr_t pos, uintptr_t *next)
{
    const uintptr_t start = (uintptr_t)image->writeback;
    if (start >= *next) return 0;
    const struct draw_transfer *t = &image->transfer;
    if (t->pitch - t->row < GMEM_ALIGN)
        return gmem_split_view(start, gmem_plane_size(t), pos, next);
    // Large row gaps can contain unprotected blocks. Only the row containing
    // pos in its pitch and the following row can give the next boundary.
    const size_t y = pos <= start ? 0 : (pos - start) / t->pitch;
    if (y >= t->rows) return 0;
    const uintptr_t row = start + y * t->pitch;
    int blocked = gmem_split_view(row, t->row, pos, next);
    if (y + 1 < t->rows) blocked |= gmem_split_view(row + t->pitch, t->row, pos, next);
    return blocked;
}

static VkResult gmem_read_alias_fault(const struct dm_m4_draw *d, const char **step)
{
    if (d->gmem_read_alias) {
        uint32_t fault;
        memcpy(&fault, d->mapped + d->gmem_map_offset + GMEM_MAP_BYTES, sizeof fault);
        if (fault)
            return failure(step, "compute raw global read aliases a separately staged writable view",
                           VK_ERROR_FEATURE_NOT_PRESENT);
    }
    return VK_SUCCESS;
}

static VkResult gmem_upload(struct dm_m4_draw *d, const struct dm_m4_gmem_source *source,
                           uint32_t flags, uint32_t *entry_count, const char **step)
{
    *entry_count = 0;
    if (!flags) return VK_SUCCESS;
    struct gmem_entry *table = (struct gmem_entry *)(d->mapped + d->gmem_map_offset);
    memset(table, 0, gmem_map_size(d->gmem_read_alias));
    uint32_t count = 1; // permanent empty entry
    for (uint32_t i = 0; i < d->gmem_count; ++i) {
        const struct gmem_range *r = &d->gmem_ranges[i];
        const uint64_t delta = (r->imported ? r->imported_address : d->buffer_address + r->offset) - r->span.guest;
        const uintptr_t host = (uintptr_t)r->span.host, host_end = host + r->span.size;
        for (uintptr_t pos = host; pos < host_end;) {
            uintptr_t next = host_end;
            int blocked = 0;
            if (d->gmem_read_protected || (d->gmem_writes && !r->imported)) {
                // Raw writes log the staged entry they touch; guarded reads
                // fault instead of dereferencing a protected entry.
                for (uint32_t j = 0; j < source->view_count; ++j) {
                    const struct dm_m4_gmem_view *view = &source->views[j];
                    if (d->gmem_writes || view->writable)
                        blocked |= gmem_split_view((uintptr_t)view->data, view->size, pos, &next);
                }
            }
            if (d->gmem_read_protected) {
                for (uint32_t j = 0; j < d->storage_image_count; ++j) {
                    const struct draw_storage_image *image = &d->storage_images[j];
                    if (image->writeback) blocked |= gmem_split_plane(image, pos, &next);
                }
            }
            if (count == DMSH_GMEM_MAP_ENTRIES)
                return failure(step, "compute global memory map exhausted by staged views", VK_ERROR_FEATURE_NOT_PRESENT);
            const uint64_t start = r->span.guest + (pos - host), end = r->span.guest + (next - host);
            struct gmem_entry *e = &table[count++];
            *e = (struct gmem_entry){.start = start, .end = end,
                .delta = d->gmem_read_protected && blocked ? GMEM_READ_ALIAS_DELTA : delta};
            if (end - start <= UINT32_MAX && start >> 32 == (end - 1) >> 32)
                e->span = (uint32_t)(end - start);
            pos = next;
        }
    }
    for (uint32_t i = count; i < DMSH_GMEM_MAP_ENTRIES; ++i) table[i].start = UINT64_MAX;
    for (uint32_t i = 1; i < count; ++i) {
        const uint64_t first = (table[i].start >> DMSH_GMEM_HINT_SHIFT) +
            ((table[i].start & ((UINT64_C(1) << DMSH_GMEM_HINT_SHIFT) - 1)) != 0);
        const uint64_t last = table[i].end >> DMSH_GMEM_HINT_SHIFT;
        for (uint64_t c = first; c < last && c <= UINT32_MAX / DMSH_GMEM_MAP_ENTRIES; ++c)
            table[c % DMSH_GMEM_MAP_ENTRIES].hint = (uint32_t)(c * DMSH_GMEM_MAP_ENTRIES + i);
    }
    for (uint32_t i = 0; i < d->gmem_count; ++i) {
        const struct gmem_range *r = &d->gmem_ranges[i];
        if (!r->imported) memcpy(d->mapped + r->offset, r->span.host, r->span.size);
    }
    if (d->gmem_writes) memset(d->mapped + d->gmem_log_offset, 0, GMEM_LOG_BYTES);
    *entry_count = count;
    return VK_SUCCESS;
}

static uint32_t gmem_log_next(const uint32_t *log, uint32_t i)
{
    while (i < DMSH_GMEM_MAP_ENTRIES) {
        uint32_t bits = log[i / 32] >> (i % 32);
        if (bits) {
            while (!(bits & 1u)) { bits >>= 1; ++i; }
            return i;
        }
        i = (i / 32 + 1) * 32;
    }
    return i;
}

static VkResult gmem_download(struct dm_m4_draw *d, const struct dm_m4_gmem_source *source,
                              uint32_t count, const char **step)
{
    if (!d->gmem_writes) return VK_SUCCESS;
    const struct gmem_entry *table = (const struct gmem_entry *)(d->mapped + d->gmem_map_offset);
    const uint32_t *log = (const uint32_t *)(d->mapped + d->gmem_log_offset);
    // Validate the entire log before publishing staged writes; each bit is
    // relative to this exact table, not an address provided by the shader.
    for (int readback = 0; readback < 2; ++readback) {
        uint32_t pool = 0;
        for (uint32_t i = gmem_log_next(log, 0); i < DMSH_GMEM_MAP_ENTRIES; i = gmem_log_next(log, i + 1)) {
            if (!i || i >= count)
                return failure(step, "compute global memory log named an invalid map entry", VK_ERROR_INVALID_SHADER_NV);
            const struct gmem_entry *e = &table[i];
            while (pool < d->gmem_count && e->start >= d->gmem_ranges[pool].span.guest + d->gmem_ranges[pool].span.size)
                ++pool;
            if (pool == d->gmem_count || e->start < d->gmem_ranges[pool].span.guest ||
                e->end <= e->start || e->end > d->gmem_ranges[pool].span.guest + d->gmem_ranges[pool].span.size)
                return failure(step, "compute global memory log named an invalid live span", VK_ERROR_INVALID_SHADER_NV);
            struct gmem_range *r = &d->gmem_ranges[pool];
            if (r->imported) continue; // Already coherent and cannot touch a separately staged view.
            const size_t offset = (size_t)(e->start - r->span.guest), size = (size_t)(e->end - e->start);
            uint8_t *host = (uint8_t *)r->span.host + offset;
            if (!readback) {
                for (uint32_t j = 0; j < source->view_count; ++j) {
                    const struct dm_m4_gmem_view *view = &source->views[j];
                    if (overlap(host, size, view->data, view->size))
                        return failure(step, "compute raw global write aliases a separately staged view", VK_ERROR_FEATURE_NOT_PRESENT);
                }
                continue;
            }
            memcpy(host, d->mapped + r->offset + offset, size);
            r->dirty = size == r->span.size;
        }
    }
    return VK_SUCCESS;
}
