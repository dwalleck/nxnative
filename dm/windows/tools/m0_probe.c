// Standalone M0 transfer probe and optional M4 address/visibility proof; no game files or window.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#define GIB (UINT64_C(1) << 30)
#define MIB (UINT64_C(1) << 20)
#define BYTES (16 * MIB)
#define RUNS 20
#define ADDRESS_RUNS 5u
#define ADDRESS_GROUP_SIZE 128u

__declspec(noreturn) static void fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    exit(1);
}

static void check(VkResult result, const char *call) {
    if (result == VK_SUCCESS) return;
    fprintf(stderr, "%s: VkResult %d\n", call, (int)result);
    exit(1);
}

#define VK(call) check((call), #call)
#define LOAD(name, lookup) PFN_##name name = (PFN_##name)(lookup); if (!name) fail("Missing " #name)
#define INSTANCE(name) LOAD(name, vkGetInstanceProcAddr(instance, #name))
#define DEVICE(name) LOAD(name, vkGetDeviceProcAddr(device, #name))
#define BUFFER_API(X) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkDestroyBuffer) X(vkFreeMemory)
#define SAMPLE_API(X) X(vkResetFences) X(vkQueueSubmit) X(vkWaitForFences) \
    X(vkFlushMappedMemoryRanges) X(vkInvalidateMappedMemoryRanges)
#define FIELD(name) PFN_##name name;
#define LOAD_FIELD(name) DEVICE(name);
#define INIT_FIELD(name) .name = name,

struct buffer_api { BUFFER_API(FIELD) };
struct sample_api { SAMPLE_API(FIELD) };

struct buffer {
    VkBuffer handle;
    VkDeviceMemory memory;
    void *mapped;
    uint32_t type;
    VkMemoryPropertyFlags flags;
};

static void *alloc_guest_va(SIZE_T size, ULONG alloc, ULONG protect, SIZE_T align) {
    // Bound the entire reservation, not just its base.
    MEM_ADDRESS_REQUIREMENTS bounds = { (void *)(uintptr_t)(128 * MIB),
        (void *)(uintptr_t)((UINT64_C(1) << 39) - 1), align };
    MEM_EXTENDED_PARAMETER param = {0};
    param.Type = MemExtendedParameterAddressRequirements;
    param.Pointer = &bounds;
    return VirtualAlloc2(GetCurrentProcess(), NULL, size, alloc, protect, &param, 1);
}

static void release_guest_va(void *p) {
    if (!VirtualFree(p, 0, MEM_RELEASE)) fail("VirtualFree failed");
}

static void probe_va(void) {
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    printf("user_va_max=%p page=%lu allocation_granularity=%lu\n",
           si.lpMaximumApplicationAddress, si.dwPageSize, si.dwAllocationGranularity);
    const SIZE_T sizes[] = { 64 * GIB, 74 * GIB };
    for (unsigned i = 0; i < ARRAYSIZE(sizes); ++i) {
        void *p = alloc_guest_va(sizes[i], MEM_RESERVE, PAGE_NOACCESS, si.dwAllocationGranularity);
        if (p) {
            printf("reserve_below_2^39=%llu GiB base=%p end_inclusive=%p\n",
                   (unsigned long long)(sizes[i] / GIB), p, (void *)((uintptr_t)p + sizes[i] - 1));
            release_guest_va(p);
        } else {
            printf("reserve_below_2^39=%llu GiB failed win32=%lu\n",
                   (unsigned long long)(sizes[i] / GIB), GetLastError());
        }
    }
}

static struct buffer make_buffer(const struct buffer_api *api, VkDevice device,
        const VkPhysicalDeviceMemoryProperties *props, VkBufferUsageFlags usage,
        VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred, int map, int address,
        VkDeviceSize bytes) {
    struct buffer buf = {0};
    VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bytes, .usage = usage };
    VK(api->vkCreateBuffer(device, &ci, NULL, &buf.handle));
    VkMemoryRequirements req;
    api->vkGetBufferMemoryRequirements(device, buf.handle, &req);
    buf.type = UINT32_MAX;
    for (uint32_t i = 0; i < props->memoryTypeCount; ++i) {
        VkMemoryPropertyFlags flags = props->memoryTypes[i].propertyFlags;
        if (!(req.memoryTypeBits & (UINT32_C(1) << i)) || (flags & required) != required) continue;
        if (buf.type == UINT32_MAX || (flags & preferred) == preferred) {
            buf.type = i;
            buf.flags = flags;
        }
        if ((flags & preferred) == preferred) break;
    }
    if (buf.type == UINT32_MAX) {
        api->vkDestroyBuffer(device, buf.handle, NULL);
        buf.handle = VK_NULL_HANDLE;
        return buf;
    }
    VkMemoryAllocateFlagsInfo flags = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = address ? &flags : NULL, .allocationSize = req.size, .memoryTypeIndex = buf.type };
    VK(api->vkAllocateMemory(device, &alloc, NULL, &buf.memory));
    VK(api->vkBindBufferMemory(device, buf.handle, buf.memory, 0));
    if (map) VK(api->vkMapMemory(device, buf.memory, 0, VK_WHOLE_SIZE, 0, &buf.mapped));
    return buf;
}

static void free_buffer(const struct buffer_api *api, VkDevice device, struct buffer buf) {
    if (buf.mapped) api->vkUnmapMemory(device, buf.memory);
    api->vkDestroyBuffer(device, buf.handle, NULL);
    api->vkFreeMemory(device, buf.memory, NULL);
}

static struct buffer import_buffer(const struct buffer_api *api, VkDevice device,
        PFN_vkGetMemoryHostPointerPropertiesEXT pointer_props, const VkPhysicalDeviceMemoryProperties *memory,
        VkBufferUsageFlags usage, void *guest, int address, VkDeviceSize bytes) {
    struct buffer out = {0};
    const VkExternalMemoryHandleTypeFlagBits type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkExternalMemoryBufferCreateInfo ext = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = type };
    VkBufferCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = &ext, .size = bytes, .usage = usage };
    VK(api->vkCreateBuffer(device, &ci, NULL, &out.handle));
    VkMemoryRequirements req;
    api->vkGetBufferMemoryRequirements(device, out.handle, &req);
    VkMemoryHostPointerPropertiesEXT props = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    VkResult result = pointer_props(device, type, guest, &props);
    if (result != VK_SUCCESS || req.size > bytes) {
        printf("host_pointer_import=unavailable pointer_query_result=%d required_size=%llu\n",
               (int)result, (unsigned long long)req.size);
        goto unsupported;
    }
    const VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t bits = props.memoryTypeBits & req.memoryTypeBits;
    for (out.type = 0; out.type < memory->memoryTypeCount; ++out.type) {
        out.flags = memory->memoryTypes[out.type].propertyFlags;
        if ((bits & (UINT32_C(1) << out.type)) && (out.flags & coherent) == coherent) break;
    }
    if (out.type == memory->memoryTypeCount) {
        printf("host_pointer_import=no coherent memory type (compatible_bits=0x%x)\n", bits);
        goto unsupported;
    }
    VkMemoryAllocateFlagsInfo flags = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
        .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkImportMemoryHostPointerInfoEXT import = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .pNext = address ? &flags : NULL, .handleType = type, .pHostPointer = guest };
    VkMemoryAllocateInfo alloc = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import, .allocationSize = bytes, .memoryTypeIndex = out.type };
    result = api->vkAllocateMemory(device, &alloc, NULL, &out.memory);
    if (result != VK_SUCCESS) {
        printf("host_pointer_import=allocation rejected VkResult=%d\n", (int)result);
        goto unsupported;
    }
    VK(api->vkBindBufferMemory(device, out.handle, out.memory, 0));
    // Write through the original coherent host pointer, not vkMapMemory.
    return out;
unsupported:
    api->vkDestroyBuffer(device, out.handle, NULL);
    return (struct buffer){0};
}

static void sample(const struct sample_api *api, VkDevice device, VkQueue queue, VkCommandBuffer cmd,
        VkFence fence, struct buffer upload, struct buffer readback, const char *label) {
    VkMappedMemoryRange src = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = upload.memory, .size = VK_WHOLE_SIZE };
    VkMappedMemoryRange dst = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = readback.memory, .size = VK_WHOLE_SIZE };
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cmd };
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    double total = 0.0;
    for (unsigned i = 0; i <= RUNS; ++i) {
        unsigned char pattern = (unsigned char)(0x70 + i);
        LARGE_INTEGER start, end;
        QueryPerformanceCounter(&start);
        memset(upload.mapped, pattern, BYTES);
        if (!(upload.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            VK(api->vkFlushMappedMemoryRanges(device, 1, &src));
        VK(api->vkResetFences(device, 1, &fence));
        VK(api->vkQueueSubmit(queue, 1, &submit, fence));
        VK(api->vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000)));
        if (!(readback.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            VK(api->vkInvalidateMappedMemoryRanges(device, 1, &dst));
        QueryPerformanceCounter(&end);
        if (i) total += (double)(end.QuadPart - start.QuadPart) / (double)freq.QuadPart;
        // The warm-up and last iteration check every byte; validation is not timed.
        if (i == 0 || i == RUNS) {
            const unsigned char *bytes = readback.mapped;
            for (size_t j = 0; j < BYTES; ++j)
                if (bytes[j] != pattern) fail("GPU readback mismatch");
        }
    }
    printf("%s upload_type=%u readback_type=%u size=%llu MiB mean=%.3f ms (n=%u, verified)\n",
           label, upload.type, readback.type, (unsigned long long)(BYTES / MIB),
           1000.0 * total / RUNS, RUNS);
}

struct address_range {
    uintptr_t guest;
    VkDeviceAddress device;
    VkDeviceSize size;
};

static VkDeviceAddress translate_address(struct address_range range, uintptr_t guest, VkDeviceSize bytes) {
    if (guest < range.guest || guest % sizeof(uint32_t)) fail("Unaligned or unmapped guest address");
    VkDeviceSize offset = guest - range.guest;
    if (offset >= range.size || bytes > range.size - offset || range.device > UINT64_MAX - offset)
        fail("Guest address extends beyond its buffer");
    return range.device + offset;
}

struct address_push {
    VkDeviceAddress src, dst;
    uint32_t count;
};
enum { ADDRESS_PUSH_BYTES = 20 };
_Static_assert(offsetof(struct address_push, dst) == 8 &&
               offsetof(struct address_push, count) + sizeof(uint32_t) == ADDRESS_PUSH_BYTES,
               "m4_address_probe.comp push constant offsets");

static uint32_t *read_spirv(const char *path, size_t *size) {
    FILE *file;
    if (fopen_s(&file, path, "rb") || !file) fail("Cannot open address shader SPIR-V");
    if (fseek(file, 0, SEEK_END)) fail("Cannot seek address shader SPIR-V");
    long length = ftell(file);
    if (length < 20 || length % 4 || length > 64 * 1024 || fseek(file, 0, SEEK_SET))
        fail("Invalid address shader SPIR-V length");
    *size = (size_t)length;
    uint32_t *code = malloc(*size);
    if (!code || fread(code, 1, *size, file) != *size || code[0] != UINT32_C(0x07230203))
        fail("Invalid address shader SPIR-V");
    fclose(file);
    return code;
}

static void sample_address(const struct sample_api *api, VkDevice device, VkQueue queue, VkCommandBuffer cmd,
        VkFence fence, const struct buffer *stage, uint32_t *guest, SIZE_T bytes, const char *label) {
    const uint32_t guard = UINT32_C(0xe1b62cf3);
    size_t half = bytes / (2 * sizeof(*guest));
    uint32_t *src = guest + 1, *dst = guest + half + 1;
    VkMappedMemoryRange mapped = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = stage ? stage->memory : VK_NULL_HANDLE, .size = VK_WHOLE_SIZE };
    VkSubmitInfo submit = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cmd };
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    double total = 0.0;
    for (unsigned run = 0; run <= ADDRESS_RUNS; ++run) {
        uint32_t seed = UINT32_C(0x170e4500) + run;
        guest[0] = guest[half - 1] = guest[half] = guest[2 * half - 1] = guard;
        for (size_t i = 0; i < half - 2; ++i)
            src[i] = dst[i] = seed ^ ((uint32_t)i * UINT32_C(0x9e3779b9));

        // Exclude input generation/checks. Staged includes both CPU and GPU copies.
        LARGE_INTEGER start, end;
        QueryPerformanceCounter(&start);
        if (stage) {
            memcpy(stage->mapped, guest, bytes);
            if (!(stage->flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                VK(api->vkFlushMappedMemoryRanges(device, 1, &mapped));
        } else {
            // Order access through the original Win32 VA. This is a CPU barrier,
            // not cache maintenance or a guarantee of platform I/O coherency.
            MemoryBarrier();
        }
        VK(api->vkResetFences(device, 1, &fence));
        VK(api->vkQueueSubmit(queue, 1, &submit, fence));
        VK(api->vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_C(10000000000)));
        if (stage) {
            if (!(stage->flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                VK(api->vkInvalidateMappedMemoryRanges(device, 1, &mapped));
            memcpy(guest, stage->mapped, bytes);
        } else {
            MemoryBarrier();
        }
        QueryPerformanceCounter(&end);
        if (run) total += (double)(end.QuadPart - start.QuadPart) / (double)freq.QuadPart;

        if (guest[0] != guard || guest[half - 1] != guard || guest[half] != guard || guest[2 * half - 1] != guard)
            fail("Address shader overwrote a guard");
        for (size_t i = 0; i < half - 2; ++i) {
            uint32_t expected = seed ^ ((uint32_t)i * UINT32_C(0x9e3779b9));
            if (src[i] != expected || dst[i] != ~expected) {
                fprintf(stderr, "%s shader mismatch: run=%u word=%zu src=0x%08x dst=0x%08x expected_dst=0x%08x\n",
                        label, run, i, src[i], dst[i], ~expected);
                exit(1);
            }
        }
    }
    printf("%s pool=%llu MiB mean=%.3f ms (n=%u, all bytes verified)\n",
           label, (unsigned long long)(bytes / MIB), 1000.0 * total / ADDRESS_RUNS, ADDRESS_RUNS);
}

static void probe_address(PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr, VkDevice device, uint32_t family,
        const VkPhysicalDeviceMemoryProperties *memory, VkBufferUsageFlags usage, SIZE_T bytes,
        int host_import, VkDeviceSize import_alignment, const char *shader_path) {
    DEVICE(vkGetDeviceQueue);
    DEVICE(vkGetBufferDeviceAddress);
    DEVICE(vkCreateShaderModule);
    DEVICE(vkDestroyShaderModule);
    DEVICE(vkCreatePipelineLayout);
    DEVICE(vkDestroyPipelineLayout);
    DEVICE(vkCreateComputePipelines);
    DEVICE(vkDestroyPipeline);
    DEVICE(vkCmdBindPipeline);
    DEVICE(vkCmdPushConstants);
    DEVICE(vkCmdDispatch);
    DEVICE(vkCmdPipelineBarrier);
    DEVICE(vkCmdCopyBuffer);
    DEVICE(vkCreateCommandPool);
    DEVICE(vkDestroyCommandPool);
    DEVICE(vkAllocateCommandBuffers);
    DEVICE(vkResetCommandBuffer);
    DEVICE(vkBeginCommandBuffer);
    DEVICE(vkEndCommandBuffer);
    DEVICE(vkCreateFence);
    DEVICE(vkDestroyFence);
    BUFFER_API(LOAD_FIELD)
    SAMPLE_API(LOAD_FIELD)
    const struct buffer_api buffers = { BUFFER_API(INIT_FIELD) };
    const struct sample_api samples = { SAMPLE_API(INIT_FIELD) };

    size_t code_size;
    uint32_t *code = read_spirv(shader_path, &code_size);
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code_size, .pCode = code };
    VkShaderModule shader;
    VK(vkCreateShaderModule(device, &smi, NULL, &shader));
    free(code);
    VkPushConstantRange push_range = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = ADDRESS_PUSH_BYTES };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push_range };
    VkPipelineLayout layout;
    VK(vkCreatePipelineLayout(device, &pli, NULL, &layout));
    VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main" }, .layout = layout };
    VkPipeline pipeline;
    VK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, NULL, &pipeline));
    vkDestroyShaderModule(device, shader, NULL);

    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family };
    VkCommandPool commands;
    VK(vkCreateCommandPool(device, &pci, NULL, &commands));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commands, .commandBufferCount = 1 };
    VkCommandBuffer cmd;
    VK(vkAllocateCommandBuffers(device, &cai, &cmd));
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    VK(vkCreateFence(device, &fci, NULL, &fence));

    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    SIZE_T align = host_import && import_alignment > si.dwAllocationGranularity ?
        import_alignment : si.dwAllocationGranularity;
    uint32_t *guest = alloc_guest_va(bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, align);
    if (!guest) {
        fprintf(stderr, "M4 guest VA allocation failed: win32=%lu\n", GetLastError());
        exit(1);
    }
    uint32_t words = (uint32_t)(bytes / (2 * sizeof(*guest))) - 2;
    // Vulkan guarantees 65535 groups in X; the shader strides across larger pools.
    uint32_t groups = (words + ADDRESS_GROUP_SIZE - 1) / ADDRESS_GROUP_SIZE;
    if (groups > 65535) groups = 65535;

    // Run and free the control before importing the same guest range.
    for (int staged = 1; staged >= 0; --staged) {
        const char *label = staged ? "staged_address" : "imported_address";
        struct buffer pool, stage = {0};
        if (staged) {
            pool = make_buffer(&buffers, device, memory, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                               0, 0, 1, bytes);
            stage = make_buffer(&buffers, device, memory,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 1, 0, bytes);
            if (!stage.handle) fail("No host-visible memory for M4 staging");
            printf("staging_type=%u flags=0x%x\n", stage.type, stage.flags);
        } else {
            if (!host_import) fail("M4 requires host-pointer import for the pool usage and alignment");
            DEVICE(vkGetMemoryHostPointerPropertiesEXT);
            pool = import_buffer(&buffers, device, vkGetMemoryHostPointerPropertiesEXT, memory, usage, guest, 1, bytes);
        }
        if (!pool.handle) fail(staged ? "No M4 device-local pool" : "No M4 coherent imported pool");

        VkBufferDeviceAddressInfo addr = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = pool.handle };
        struct address_range range = { (uintptr_t)guest, vkGetBufferDeviceAddress(device, &addr), bytes };
        if (!range.device || range.device % sizeof(*guest)) fail("Invalid M4 buffer device address");
        struct address_push push = {
            .src = translate_address(range, (uintptr_t)(guest + 1), words * sizeof(*guest)),
            .dst = translate_address(range, (uintptr_t)(guest + 1) + bytes / 2, words * sizeof(*guest)),
            .count = words };
        printf("%s type=%u flags=0x%x guest=%p gpu=0x%llx src=0x%llx dst=0x%llx\n",
               label, pool.type, pool.flags, (void *)guest, (unsigned long long)range.device,
               (unsigned long long)push.src, (unsigned long long)push.dst);

        VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VK(vkBeginCommandBuffer(cmd, &begin));
        VkBufferCopy copy = { .size = bytes };
        VkMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        if (staged) {
            vkCmdCopyBuffer(cmd, stage.handle, pool.handle, 1, &copy);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &barrier, 0, NULL, 0, NULL);
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, ADDRESS_PUSH_BYTES, &push);
        vkCmdDispatch(cmd, groups, 1, 1);
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        if (staged) {
            // The readback includes the source and guards from the first copy.
            barrier.srcAccessMask |= VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
            vkCmdCopyBuffer(cmd, pool.handle, stage.handle, 1, &copy);
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        }
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, staged ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
        VK(vkEndCommandBuffer(cmd));
        sample_address(&samples, device, queue, cmd, fence, staged ? &stage : NULL, guest, bytes, label);
        VK(vkResetCommandBuffer(cmd, 0));
        if (staged) free_buffer(&buffers, device, stage);
        free_buffer(&buffers, device, pool);
    }
    release_guest_va(guest);
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, commands, NULL);
    vkDestroyPipeline(device, pipeline, NULL);
    vkDestroyPipelineLayout(device, layout, NULL);
    puts("M4 address and bidirectional visibility: PASS");
}

int main(int argc, char **argv) {
    const char *shader_path = NULL;
    SIZE_T bytes = BYTES;
    if (argc > 1) {
        if (argc < 3 || argc > 4 || strcmp(argv[1], "--address-shader"))
            fail("usage: m0_probe [--address-shader m4_address_probe.spv [pool_MiB]]");
        shader_path = argv[2];
        if (argc == 4) {
            char *end;
            unsigned long mib = strtoul(argv[3], &end, 10);
            if (!mib || mib > 512 || *end || argv[3][0] < '0' || argv[3][0] > '9')
                fail("pool_MiB must be an integer from 1 to 512");
            bytes = mib * MIB;
        }
    }
    USHORT process, native;
    if (!IsWow64Process2(GetCurrentProcess(), &process, &native) ||
        process != IMAGE_FILE_MACHINE_UNKNOWN || native != IMAGE_FILE_MACHINE_ARM64)
        fail("Not running as a native ARM64 process");
    puts("machine=native ARM64");
    if (!shader_path) probe_va();

    HMODULE loader = LoadLibraryExW(L"vulkan-1.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!loader) fail("No Vulkan loader in System32");
    LOAD(vkGetInstanceProcAddr, GetProcAddress(loader, "vkGetInstanceProcAddr"));
    LOAD(vkCreateInstance, vkGetInstanceProcAddr(NULL, "vkCreateInstance"));
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance instance;
    VK(vkCreateInstance(&ici, NULL, &instance));
    INSTANCE(vkDestroyInstance);
    INSTANCE(vkEnumeratePhysicalDevices);
    INSTANCE(vkGetPhysicalDeviceProperties2);
    INSTANCE(vkGetPhysicalDeviceFeatures2);
    INSTANCE(vkGetPhysicalDeviceQueueFamilyProperties);
    INSTANCE(vkGetPhysicalDeviceMemoryProperties);
    INSTANCE(vkCreateDevice);
    INSTANCE(vkGetDeviceProcAddr);
    INSTANCE(vkEnumerateDeviceExtensionProperties);
    INSTANCE(vkGetPhysicalDeviceExternalBufferProperties);
    uint32_t count = 0;
    VK(vkEnumeratePhysicalDevices(instance, &count, NULL));
    if (!count) fail("No Vulkan devices found");
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices) fail("Device list allocation failed");
    VK(vkEnumeratePhysicalDevices(instance, &count, devices));
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceDriverProperties driver = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceProperties2 props = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver };
        vkGetPhysicalDeviceProperties2(devices[i], &props);
        uint32_t ver = props.properties.apiVersion;
        printf("gpu=%s api=%u.%u.%u driver_id=%d driver=%s %s\n", props.properties.deviceName,
               VK_API_VERSION_MAJOR(ver), VK_API_VERSION_MINOR(ver), VK_API_VERSION_PATCH(ver),
               (int)driver.driverID, driver.driverName, driver.driverInfo);
        if (!gpu && ver >= VK_API_VERSION_1_2 && driver.driverID == VK_DRIVER_ID_NVIDIA_PROPRIETARY)
            gpu = devices[i];
    }
    free(devices);
    if (!gpu) fail("No proprietary NVIDIA Vulkan 1.2+ device (no fallback to Dozen or software)");

    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(gpu, &memory);
    puts("memory type flags: device_local=0x1 host_visible=0x2 host_coherent=0x4 host_cached=0x8");
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        uint32_t heap = memory.memoryTypes[i].heapIndex;
        printf("memory_type=%u flags=0x%x heap=%u heap_size=%.2f GiB\n", i,
               memory.memoryTypes[i].propertyFlags, heap, (double)memory.memoryHeaps[heap].size / GIB);
    }
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, NULL);
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    if (!families) fail("Queue family allocation failed");
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families);
    uint32_t family = 0;
    VkQueueFlags queue_flags = VK_QUEUE_GRAPHICS_BIT | (shader_path ? VK_QUEUE_COMPUTE_BIT : 0);
    while (family < count && (families[family].queueFlags & queue_flags) != queue_flags) ++family;
    free(families);
    if (family == count) fail("No suitable graphics/compute queue");
    VkPhysicalDeviceBufferDeviceAddressFeatures bda = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
    VkPhysicalDeviceFeatures2 features = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &bda };
    vkGetPhysicalDeviceFeatures2(gpu, &features);
    bda.bufferDeviceAddressCaptureReplay = bda.bufferDeviceAddressMultiDevice = VK_FALSE;
    if (shader_path && !bda.bufferDeviceAddress) fail("M4 requires bufferDeviceAddress");
    VkBufferUsageFlags both = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBufferUsageFlags pool_usage = both | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        (bda.bufferDeviceAddress ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
    if (shader_path)
        pool_usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    VK(vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, NULL));
    VkExtensionProperties *exts = calloc(count ? count : 1, sizeof(*exts));
    if (!exts) fail("Extension list allocation failed");
    VK(vkEnumerateDeviceExtensionProperties(gpu, NULL, &count, exts));
    const char *host_ext = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    int host_import = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(exts[i].extensionName, host_ext)) host_import = 1;
    free(exts);
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT import_props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
    if (host_import) {
        VkPhysicalDeviceProperties2 props = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &import_props };
        vkGetPhysicalDeviceProperties2(gpu, &props);
        VkPhysicalDeviceExternalBufferInfo query = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
            .usage = pool_usage, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
        VkExternalBufferProperties external = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
        vkGetPhysicalDeviceExternalBufferProperties(gpu, &query, &external);
        VkExternalMemoryFeatureFlags flags = external.externalMemoryProperties.externalMemoryFeatures;
        printf("host_pointer_import alignment=%llu external_features=0x%x\n",
               (unsigned long long)import_props.minImportedHostPointerAlignment, flags);
        host_import = (flags & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) &&
            !(flags & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) &&
            import_props.minImportedHostPointerAlignment && bytes % import_props.minImportedHostPointerAlignment == 0;
    }
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &priority };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &bda,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = host_import ? 1 : 0, .ppEnabledExtensionNames = &host_ext };
    VkDevice device;
    VK(vkCreateDevice(gpu, &dci, NULL, &device));
    DEVICE(vkDestroyDevice);
    if (shader_path) {
        probe_address(vkGetDeviceProcAddr, device, family, &memory, pool_usage, bytes,
                      host_import, import_props.minImportedHostPointerAlignment, shader_path);
        vkDestroyDevice(device, NULL);
        vkDestroyInstance(instance, NULL);
        FreeLibrary(loader);
        return 0;
    }
    DEVICE(vkGetDeviceQueue);
    DEVICE(vkGetBufferDeviceAddress);
    DEVICE(vkCreateCommandPool);
    DEVICE(vkDestroyCommandPool);
    DEVICE(vkAllocateCommandBuffers);
    DEVICE(vkBeginCommandBuffer);
    DEVICE(vkEndCommandBuffer);
    DEVICE(vkCmdCopyBuffer);
    DEVICE(vkCmdPipelineBarrier);
    DEVICE(vkCreateFence);
    DEVICE(vkDestroyFence);
    BUFFER_API(LOAD_FIELD)
    SAMPLE_API(LOAD_FIELD)
    const struct buffer_api buffers = { BUFFER_API(INIT_FIELD) };
    const struct sample_api samples = { SAMPLE_API(INIT_FIELD) };

    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);
    VkMemoryPropertyFlags local = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    VkMemoryPropertyFlags cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    struct buffer pool = make_buffer(&buffers, device, &memory, pool_usage,
        local | host, cached, 1, bda.bufferDeviceAddress, bytes);
    if (!pool.handle) {
        puts("host_visible_device_local_pool=unavailable; measuring a non-local mapped buffer instead");
        pool = make_buffer(&buffers, device, &memory, pool_usage, host, cached, 1, bda.bufferDeviceAddress, bytes);
    }
    if (!pool.handle) fail("No host-visible memory type for the pool buffer");
    printf("direct_pool_type=%u cpu_va=%p", pool.type, pool.mapped);
    if (bda.bufferDeviceAddress) {
        VkBufferDeviceAddressInfo addr = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = pool.handle };
        VkDeviceAddress gpu_va = vkGetBufferDeviceAddress(device, &addr);
        printf(" gpu_va=0x%llx equal=%s below_2^40=%s\n", (unsigned long long)gpu_va,
               gpu_va == (uintptr_t)pool.mapped ? "yes" : "no", gpu_va < (UINT64_C(1) << 40) ? "yes" : "no");
    } else puts(" buffer_device_address=unavailable");
    struct buffer local_pool = make_buffer(&buffers, device, &memory, both | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        local, 0, 0, 0, bytes);
    struct buffer stage = make_buffer(&buffers, device, &memory, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host, cached, 1, 0, bytes);
    struct buffer readback = make_buffer(&buffers, device, &memory, VK_BUFFER_USAGE_TRANSFER_DST_BIT, host, cached, 1, 0, bytes);
    if (!local_pool.handle || !stage.handle || !readback.handle) fail("No compatible local or staging/readback memory type");
    printf("staged_destination_type=%u\n", local_pool.type);
    struct buffer imported = {0};
    void *guest = NULL;
    if (host_import) {
        DEVICE(vkGetMemoryHostPointerPropertiesEXT);
        SYSTEM_INFO si;
        GetNativeSystemInfo(&si);
        SIZE_T align = (SIZE_T)import_props.minImportedHostPointerAlignment;
        if (align < si.dwAllocationGranularity) align = si.dwAllocationGranularity;
        guest = alloc_guest_va(BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, align);
        if (!guest) printf("host_pointer_import=guest allocation failed win32=%lu\n", GetLastError());
        else {
            imported = import_buffer(&buffers, device, vkGetMemoryHostPointerPropertiesEXT,
                &memory, pool_usage, guest, bda.bufferDeviceAddress, bytes);
            if (imported.handle && bda.bufferDeviceAddress) {
                VkBufferDeviceAddressInfo addr = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                    .buffer = imported.handle };
                VkDeviceAddress gpu_va = vkGetBufferDeviceAddress(device, &addr);
                printf("imported_guest_va=%p gpu_va=0x%llx equal=%s\n", guest,
                       (unsigned long long)gpu_va, gpu_va == (uintptr_t)guest ? "yes" : "no");
            }
        }
    } else puts("host_pointer_import=not supported for this buffer configuration");

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family };
    VkCommandPool commands;
    VK(vkCreateCommandPool(device, &pci, NULL, &commands));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = commands, .commandBufferCount = imported.handle ? 3 : 2 };
    VkCommandBuffer cmds[3];
    VK(vkAllocateCommandBuffers(device, &cai, cmds));
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    VK(vkCreateFence(device, &fci, NULL, &fence));
    for (uint32_t i = 0; i < cai.commandBufferCount; ++i) {
        VkCommandBuffer cmd = cmds[i];
        int staged = i == 1;
        VkBuffer source = staged ? local_pool.handle : i == 2 ? imported.handle : pool.handle;
        VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VK(vkBeginCommandBuffer(cmd, &begin));
        VkBufferCopy copy = { .size = BYTES };
        VkMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
        if (staged) {
            vkCmdCopyBuffer(cmd, stage.handle, local_pool.handle, 1, &copy);
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 1, &barrier, 0, NULL, 0, NULL);
        }
        vkCmdCopyBuffer(cmd, source, readback.handle, 1, &copy);
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &barrier, 0, NULL, 0, NULL);
        VK(vkEndCommandBuffer(cmd));
    }
    sample(&samples, device, queue, cmds[0], fence, pool, readback, "direct");
    sample(&samples, device, queue, cmds[1], fence, stage, readback, "staged");
    if (imported.handle) {
        struct buffer upload = imported;
        upload.mapped = guest;
        sample(&samples, device, queue, cmds[2], fence, upload, readback, "imported");
    }
    vkDestroyFence(device, fence, NULL);
    vkDestroyCommandPool(device, commands, NULL);
    if (imported.handle) free_buffer(&buffers, device, imported);
    if (guest) release_guest_va(guest);
    free_buffer(&buffers, device, readback);
    free_buffer(&buffers, device, stage);
    free_buffer(&buffers, device, local_pool);
    free_buffer(&buffers, device, pool);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    FreeLibrary(loader);
    return 0;
}
