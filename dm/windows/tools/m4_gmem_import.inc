// Keep most device allocations available to the rest of replay. A window may
// fill small committed gaps so several live runs can share an import slot.
enum { GMEM_IMPORT_MIN = 4 << 20, GMEM_IMPORT_MAX = 256, GMEM_IMPORT_MAX_GAP = 64 << 10 };

static void gmem_destroy_import(struct dm_m4_draw *d, const struct gmem_import *entry)
{
    const VkDevice vk = d->device->device;
    d->DestroyBuffer(vk, entry->buffer, NULL);
    if (entry->memory) d->FreeMemory(vk, entry->memory, NULL);
}

static void gmem_wait_before_releasing(struct dm_m4_draw *d)
{
    if (!d->in_flight) return;
    // Normal submissions are synchronous. A failed fence wait may leave work
    // pending even after replay returned and released its host-VM lock.
    const VkResult r = d->DeviceWaitIdle(d->device->device);
    if (r == VK_ERROR_DEVICE_LOST) {
        fprintf(stderr, "[nvn] Vulkan device lost while retiring global memory imports\n");
        d->queue_error = r;
    } else if (r != VK_SUCCESS) {
        fprintf(stderr, "[nvn] Cannot release imported global memory while Vulkan work may be pending: VkResult %d\n",
                (int)r);
        abort();
    }
    d->in_flight = 0;
}

// The cache stays sorted and disjoint. Its ends are therefore sorted too.
static uint32_t gmem_first_overlap(const struct dm_m4_draw *d, uintptr_t begin)
{
    uint32_t lo = 0, hi = d->gmem_import_count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (d->gmem_imports[mid].end <= begin) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static void gmem_drop_imports(struct dm_m4_draw *d, uint32_t first, uintptr_t end)
{
    uint32_t last = first;
    while (last < d->gmem_import_count && d->gmem_imports[last].begin < end) ++last;
    if (first == last) return;
    gmem_wait_before_releasing(d);
    const VkDevice vk = d->device->device;
    for (uint32_t i = first; i < last; ++i) {
        const struct gmem_import *entry = &d->gmem_imports[i];
        for (uint32_t j = 0; j < d->texel_view_capacity; ++j) {
            struct draw_texel_view *view = &d->texel_views[j];
            if (view->handle && view->buffer == entry->buffer) {
                d->DestroyBufferView(vk, view->handle, NULL);
                *view = (struct draw_texel_view){0};
            }
        }
        gmem_destroy_import(d, entry);
    }
    if (last < d->gmem_import_count)
        memmove(d->gmem_imports + first, d->gmem_imports + last,
                (d->gmem_import_count - last) * sizeof(*d->gmem_imports));
    d->gmem_import_count -= last - first;
}

void dm_m4_draw_forget_imports(struct dm_m4_draw *d, const void *host, size_t size)
{
    if (!d || !size) return;
    const uintptr_t begin = (uintptr_t)host;
    const uintptr_t end = size > UINTPTR_MAX - begin ? UINTPTR_MAX : begin + size;
    gmem_drop_imports(d, gmem_first_overlap(d, begin), end);
}

static VkResult gmem_import_result(struct dm_m4_draw *d, VkResult r, const char *where, const char **step)
{
    switch (r) {
    case VK_ERROR_INVALID_EXTERNAL_HANDLE:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_TOO_MANY_OBJECTS:
        // Import may be rejected by the platform or run out of its allocation
        // budget even when a shared staging allocation remains available.
        return VK_SUCCESS;
    case VK_ERROR_DEVICE_LOST:
        fprintf(stderr, "[nvn] Vulkan device lost: %s\n", where);
        d->queue_error = r;
        break;
    default:
        break;
    }
    return failure(step, where, r);
}

static uint32_t gmem_import_budget(const struct dm_m4_draw *d)
{
    if (!d->GetMemoryHostPointerPropertiesEXT) return 0;
    const uint32_t limit = d->limits.maxMemoryAllocationCount / 16;
    return limit < GMEM_IMPORT_MAX ? limit : GMEM_IMPORT_MAX;
}

static uintptr_t gmem_private_extent(struct dm_m4_draw *d, uintptr_t begin)
{
    if (begin >= d->gmem_private_begin && begin < d->gmem_private_end)
        return d->gmem_private_end;
    // One submission holds the replay lock throughout. Cache its positive
    // proof only within that submission: a later commit can join two
    // previously separate VirtualQuery regions without revoking any import.
    // Never stitch regions across allocation, state or protection changes.
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery((const void *)begin, &mi, sizeof mi) != sizeof mi ||
        mi.State != MEM_COMMIT || mi.Type != MEM_PRIVATE || mi.Protect != PAGE_READWRITE ||
        !mi.AllocationBase)
        return 0;
    const uintptr_t base = (uintptr_t)mi.BaseAddress;
    if (base > begin || mi.RegionSize > UINTPTR_MAX - base || begin - base >= mi.RegionSize)
        return 0;
    d->gmem_private_begin = base;
    return d->gmem_private_end = base + mi.RegionSize;
}

static VkResult gmem_import(struct dm_m4_draw *d, const struct gmem_candidate *candidate,
                            const struct gmem_import **out, const char **step)
{
    *out = NULL;
    const uintptr_t begin = candidate->begin, end = candidate->end;
    const size_t size = end - begin;
    void *const host = (void *)begin;
    const uint32_t first = gmem_first_overlap(d, begin);
    // All conflicting old handles were retired before selecting this draw's
    // backing. Never replace a handle now: a previously selected run may
    // already refer to it, or this exact candidate may be part of a window.
    if (first < d->gmem_import_count && d->gmem_imports[first].begin < end)
        return VK_SUCCESS;

    const VkDevice vk = d->device->device;
    const VkExternalMemoryHandleTypeFlagBits type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkMemoryHostPointerPropertiesEXT pointer_props = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT,
    };
    VkResult r = d->GetMemoryHostPointerPropertiesEXT(vk, type, host, &pointer_props);
    if (r != VK_SUCCESS) return gmem_import_result(d, r, "query global memory host pointer", step);
    if (!pointer_props.memoryTypeBits) return VK_SUCCESS;

    const VkExternalMemoryBufferCreateInfo external = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = type,
    };
    const VkBufferCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &external,
        .size = size, .usage = DM_M4_HOST_IMPORT_USAGE,
    };
    VkBuffer handle;
    r = d->CreateBuffer(vk, &ci, NULL, &handle);
    if (r != VK_SUCCESS) return gmem_import_result(d, r, "create imported global memory buffer", step);
    struct gmem_import entry = {.begin = begin, .end = end, .buffer = handle,
        .widened = size != candidate->live_bytes};

    const VkBufferMemoryRequirementsInfo2 info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, .buffer = handle,
    };
    VkMemoryDedicatedRequirements dedicated = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 requirements = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dedicated};
    d->GetBufferMemoryRequirements2(vk, &info, &requirements);
    const VkMemoryRequirements *mem = &requirements.memoryRequirements;
    // Host-pointer imports cannot be dedicated. Don't round past the caller's
    // exact validated extent just to accommodate a buffer's padding.
    if (dedicated.requiresDedicatedAllocation || mem->size > size) goto discard;

    const uint32_t bits = pointer_props.memoryTypeBits & mem->memoryTypeBits;
    const VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t index;
    for (index = 0; index < d->memory.memoryTypeCount; ++index) {
        const VkMemoryType *memory = &d->memory.memoryTypes[index];
        if ((bits & (UINT32_C(1) << index)) && (memory->propertyFlags & coherent) == coherent &&
            size <= d->memory.memoryHeaps[memory->heapIndex].size)
            break;
    }
    if (index == d->memory.memoryTypeCount) goto discard;

    const VkMemoryAllocateFlagsInfo flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    const VkImportMemoryHostPointerInfoEXT import = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, .pNext = &flags,
        .handleType = type, .pHostPointer = host,
    };
    const VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import,
        .allocationSize = size, .memoryTypeIndex = index,
    };
    VkDeviceMemory memory;
    r = d->AllocateMemory(vk, &alloc, NULL, &memory);
    if (r != VK_SUCCESS) {
        r = gmem_import_result(d, r, "import coherent global memory host allocation", step);
        goto discard;
    }
    entry.memory = memory;
    r = d->BindBufferMemory(vk, handle, memory, 0);
    if (r != VK_SUCCESS) {
        r = gmem_import_result(d, r, "bind imported global memory buffer", step);
        goto discard;
    }
    const VkBufferDeviceAddressInfo address_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = handle,
    };
    // The guest uses its original coherent CPU pointer; only the checked
    // address map and descriptors see the separate Vulkan device address.
    entry.address = d->GetBufferDeviceAddress(vk, &address_info);
    if (!entry.address || entry.address % 16 || size > UINT64_MAX - entry.address) goto discard;

    void *imports = d->gmem_imports;
    r = grow_array(&imports, sizeof entry, d->gmem_import_count + 1, &d->gmem_import_capacity, 0);
    if (r != VK_SUCCESS) {
        r = failure(step, "grow global memory import cache", r);
        goto discard;
    }
    d->gmem_imports = imports;
    if (first < d->gmem_import_count)
        memmove(d->gmem_imports + first + 1, d->gmem_imports + first,
                (d->gmem_import_count - first) * sizeof entry);
    d->gmem_imports[first] = entry;
    ++d->gmem_import_count;
    *out = &d->gmem_imports[first];
    return VK_SUCCESS;

discard:
    gmem_destroy_import(d, &entry);
    return r;
}
