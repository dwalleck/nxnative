#include "../../gfx/nvn/dmsh_abi.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>

#define SMOKE_GUEST_BASE UINT64_C(0x1ffffffe0)
enum { GMEM_SMOKE_WORDS = 64, GMEM_SMOKE_SPAN = 64, GMEM_SMOKE_DST = 96 };

struct gmem_smoke_pools {
    uint32_t *words;
    int malformed;
    uint32_t *large;
};

static int smoke_visit_gmem(const void *context, dm_m4_gmem_accept accept, void *userdata)
{
    const struct gmem_smoke_pools *p = context;
    // Deliberately enumerate out of order and include a live overlapping
    // alias. The guest range crosses a 4 GiB boundary and is neither a host nor Vulkan VA.
    const uint64_t base = p->large ? (uintptr_t)p->words : SMOKE_GUEST_BASE;
    if (p->large && !accept(userdata, (struct dm_m4_gmem_span){
            (uintptr_t)p->large, p->large, 16u << 20, 0})) return 0;
    return accept(userdata, (struct dm_m4_gmem_span){base + GMEM_SMOKE_DST,
                      p->words + GMEM_SMOKE_DST / 4, GMEM_SMOKE_SPAN, 0}) &&
           accept(userdata, (struct dm_m4_gmem_span){base + 16, p->words + 4, 32, 0}) &&
           accept(userdata, (struct dm_m4_gmem_span){base, p->words,
                      p->malformed ? GMEM_SMOKE_SPAN - 1 : GMEM_SMOKE_SPAN, 0});
}

static int smoke_visit_shared(const void *context, dm_m4_gmem_accept accept, void *userdata)
{
    const struct dm_m4_gmem_span *spans = context;
    return accept(userdata, spans[1]) && accept(userdata, spans[0]);
}

static VkResult smoke_shared_gmem(struct dm_m4_draw *draw, const struct dm_m4_device *device,
                     struct dm_m4_compute_info *info, struct dm_m4_draw_binding *packet, const char **step)
{
    SYSTEM_INFO sys;
    GetNativeSystemInfo(&sys);
    const size_t page = sys.dwPageSize;
    size_t alignment = (size_t)device->host_import_alignment;
    if (alignment < page) alignment = page;
    PFN_vkGetPhysicalDeviceProperties properties = (PFN_vkGetPhysicalDeviceProperties)
        device->get_instance_proc(device->instance, "vkGetPhysicalDeviceProperties");
    if (!properties) {
        *step = "query shared GMEM fixture descriptor alignment";
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkPhysicalDeviceProperties props;
    properties(device->physical, &props);
    if (alignment < props.limits.minStorageBufferOffsetAlignment)
        alignment = (size_t)props.limits.minStorageBufferOffsetAlignment;
    const size_t half = alignment > (2u << 20) ? alignment : (2u << 20);
    *step = "reserve two live GMEM ranges with one private committed hole";
    if (half > SIZE_MAX / 3) return VK_ERROR_OUT_OF_HOST_MEMORY;
    const size_t allocation_size = 2 * half + alignment;
    uint8_t *allocation = VirtualAlloc(NULL, allocation_size, MEM_RESERVE, PAGE_NOACCESS);
    if (!allocation) return VK_ERROR_OUT_OF_HOST_MEMORY;
    const uintptr_t offset = (uintptr_t)allocation % alignment;
    uint8_t *base = allocation + (offset ? alignment - offset : 0);
    VkResult result = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!VirtualAlloc(base, 2 * half, MEM_COMMIT, PAGE_READWRITE)) goto done;

    uint32_t *left = (uint32_t *)(base + 16), *right = (uint32_t *)(base + half);
    uint32_t *hole = (uint32_t *)(base + half - page);
    const struct dm_m4_gmem_span spans[] = {
        {(uintptr_t)left, left, half - page - 16, 1},
        {(uintptr_t)right, right, half - 16, 1},
    };
    packet->buffer = (struct dm_m4_draw_span){right, 4 * sizeof(uint64_t)};
    info->gmem = (struct dm_m4_gmem_source){.visit = smoke_visit_shared, .context = spans};
    uint32_t *tail = (uint32_t *)((uint8_t *)left + spans[0].size - 16);
    uint32_t *left_result = left + 16, *right_result = right + 16;
    const uint32_t canary = UINT32_C(0x8297fea1);
    hole[0] = hole[4] = canary;
    static const char *const phases[] = {
        "shared GMEM committed: ordinary SSBO in second span, raw copy from first to second",
        "shared GMEM reuse: ordinary SSBO in second span, raw copy back to first",
        "shared GMEM guards: discard raw reads and stores in committed hole",
        "shared GMEM protected hole: retire import and reject a wider backing",
        "shared GMEM decommitted hole: submit both still-live spans without a wider backing",
        "shared GMEM recommitted hole: coherent access with fresh backing",
    };
    for (unsigned pass = 0; pass < _countof(phases); ++pass) {
        if (pass >= 3) {
            // Revoke just the non-live page; that must revoke the entire
            // backing even though neither of its visible runs overlaps it.
            dm_m4_draw_forget_imports(draw, hole, page);
            *step = phases[pass];
            DWORD old;
            if ((pass == 3 && !VirtualProtect(hole, page, PAGE_READONLY, &old)) ||
                (pass == 4 && !VirtualFree(hole, page, MEM_DECOMMIT)) ||
                (pass == 5 && !VirtualAlloc(hole, page, MEM_COMMIT, PAGE_READWRITE))) {
                result = VK_ERROR_UNKNOWN;
                goto done;
            }
        }
        uint64_t addresses[4] = {(uintptr_t)tail, (uintptr_t)right_result, 0, 0};
        uint32_t expected[4];
        if (pass == 1) {
            addresses[0] = (uintptr_t)right_result;
            addresses[1] = (uintptr_t)left_result;
        } else if (pass == 2) {
            addresses[0] = (uintptr_t)hole;
            addresses[2] = (uintptr_t)tail;
            addresses[3] = (uintptr_t)hole;
        }
        uint32_t *dst = (uint32_t *)(uintptr_t)addresses[1];
        for (unsigned i = 0; i < _countof(expected); ++i) {
            if (pass != 1) tail[i] = UINT32_C(0xa8890031) + 16u * pass + i;
            expected[i] = pass == 2 ? 0 : tail[i];
            dst[i] = canary;
        }
        memcpy(right, addresses, sizeof addresses);
        puts(phases[pass]);
        result = dm_m4_compute_submit(draw, info, step);
        if (result != VK_SUCCESS) goto done;
        if (memcmp(dst, expected, sizeof expected) || (pass <= 3 && (hole[0] != canary || hole[4] != canary))) {
            *step = phases[pass];
            result = VK_ERROR_UNKNOWN;
            goto done;
        }
    }

done:
    dm_m4_draw_forget_imports(draw, allocation, allocation_size);
    VirtualFree(allocation, 0, MEM_RELEASE);
    return result;
}

static VkResult smoke_gmem_dispatch(const struct dm_m4_device *device,
                     const struct dm_m4_draw_requirements *requirements, const wchar_t *directory,
                     const char **step)
{
    static const uint8_t key[8] = {'g', 'm', 'e', 'm', 's', 'm', 'o', 'k'};
    struct dm_m4_shader cs = {0};
    struct dm_m4_draw *draw = NULL;
    uint32_t *large = NULL;
    *step = "load production-translated checked GMEM smoke shader";
    VkResult r = dm_m4_shader_load(device, directory, key, VK_SHADER_STAGE_COMPUTE_BIT, &cs);
    if (r != VK_SUCCESS) return r;

    struct dm_m4_draw_binding bindings[4] = {0};
    uint32_t packet = UINT32_MAX;
    *step = "check smoke shader GMEM and address-packet reflection";
    if (cs.gmem != 3 || cs.binding_count > sizeof bindings / sizeof *bindings) {
        r = VK_ERROR_INVALID_SHADER_NV;
        goto done;
    }
    for (uint32_t i = 0; i < cs.binding_count; ++i) {
        const struct dm_m4_shader_binding *b = &cs.bindings[i];
        if (b->source == DM_M4_SHADER_SBUF && !b->sbuf.flags && !b->sbuf.cb_slot &&
            b->sbuf.cb_offset == DMSH_C0_SSBO_CS / 4) packet = i;
    }
    if (packet == UINT32_MAX) {
        r = VK_ERROR_INVALID_SHADER_NV;
        goto done;
    }
    r = dm_m4_draw_create(&draw, device, requirements, step);
    if (r != VK_SUCCESS) goto done;

    static const struct {
        const char *name;
        int src, dst;
        int writes, reads, protect_at;
    } vector_cases[] = {
        {"read the last aligned vector and write the first destination vector", 48, GMEM_SMOKE_DST, 1, 1, 0},
        {"reload changed live bytes and write the last destination vector", 48, GMEM_SMOKE_DST + 48, 1, 1, 0},
        {"reject a misaligned raw vector even when entirely inside the span", 20, GMEM_SMOKE_DST, 1, 0, 0},
        {"reject a vector that would cross the logical pool end", 52, GMEM_SMOKE_DST, 1, 0, 0},
        {"reject a read at the exclusive pool end", 64, GMEM_SMOKE_DST, 1, 0, 0},
        {"reject a read before the first live pool", -16, GMEM_SMOKE_DST, 1, 0, 0},
        {"discard misaligned raw stores before logging", 48, GMEM_SMOKE_DST + 1, 0, 0, GMEM_SMOKE_DST},
        {"discard raw stores in the hole before the destination", 48, GMEM_SMOKE_DST - 16, 0, 0, 0},
        {"discard raw stores at the exclusive destination end", 48, GMEM_SMOKE_DST + GMEM_SMOKE_SPAN, 0, 0, 0},
    };
    static const struct {
        const char *name;
        int src, dst, expect_u8, expect_u16, protect_at;
    } subword_cases[] = {
        {"read the last aligned halfword and store only the selected bytes", 62, 98, 0x45, 0x6745, 0},
        {"accept an odd byte read but reject an odd halfword read", 61, 98, 0x23, 0, 0},
        {"read the last byte but reject a halfword crossing the pool end", 63, 98, 0x67, 0, 0},
        {"write an odd byte but reject an odd halfword before logging", 62, 97, 0x45, -1, 112},
        {"write the last byte without touching the unmapped next halfword", 63, 159, 0x67, -1, 0},
        {"reject byte and halfword reads at the exclusive pool end", 64, 98, 0, 0, 0},
    };
    enum { VECTOR_CASES = sizeof vector_cases / sizeof *vector_cases,
           SUBWORD_CASES = sizeof subword_cases / sizeof *subword_cases };
    _Alignas(16) uint32_t words[GMEM_SMOKE_WORDS];
    struct gmem_smoke_pools pools = {.words = words};
    bindings[packet].buffer = (struct dm_m4_draw_span){words, 4 * sizeof(uint64_t)};
    struct dm_m4_compute_info info = {
        .cs = {&cs, bindings}, .groups = {1, 1, 1}, .gmem = {.visit = smoke_visit_gmem, .context = &pools},
    };
    dm_m4_draw_support_init(&info.support);
    for (uint32_t i = 0; i < VECTOR_CASES + SUBWORD_CASES; ++i) {
        for (uint32_t j = 0; j < GMEM_SMOKE_WORDS; ++j)
            words[j] = UINT32_C(0x9abc0000) | (i << 8) | j;
        const int subword = i >= VECTOR_CASES;
        const uint32_t j = subword ? i - VECTOR_CASES : i;
        uint64_t addresses[4] = {0};
        const char *name;
        int protect_at;
        if (subword) {
            addresses[2] = SMOKE_GUEST_BASE + (uint64_t)subword_cases[j].src;
            addresses[3] = SMOKE_GUEST_BASE + (uint64_t)subword_cases[j].dst;
            words[15] = UINT32_C(0x67452311); // independent byte/halfword test values
            name = subword_cases[j].name;
            protect_at = subword_cases[j].protect_at;
        } else {
            addresses[0] = SMOKE_GUEST_BASE + (uint64_t)vector_cases[j].src;
            addresses[1] = SMOKE_GUEST_BASE + (uint64_t)vector_cases[j].dst;
            name = vector_cases[j].name;
            protect_at = vector_cases[j].protect_at;
        }
        memcpy(words, addresses, sizeof addresses);
        uint32_t expected[GMEM_SMOKE_WORDS];
        memcpy(expected, words, sizeof expected);
        if (subword) {
            uint8_t *dst = (uint8_t *)expected + subword_cases[j].dst;
            *dst = (uint8_t)subword_cases[j].expect_u8;
            if (subword_cases[j].expect_u16 >= 0) {
                dst[16] = (uint8_t)subword_cases[j].expect_u16;
                dst[17] = (uint8_t)((unsigned)subword_cases[j].expect_u16 >> 8);
            }
        } else if (vector_cases[j].writes) {
            uint32_t *dst = expected + vector_cases[j].dst / 4;
            for (uint32_t k = 0; k < 4; ++k)
                dst[k] = vector_cases[j].reads ? UINT32_C(0x9abc000c) | (i << 8) | k : 0;
        }
        // A rejected store must not log its entry: logging the protected
        // interval would cause replay to refuse the submission.
        const struct dm_m4_gmem_view protected_view = {(uint8_t *)words + protect_at, 16, 0};
        info.gmem.views = protect_at ? &protected_view : NULL;
        info.gmem.view_count = (uint32_t)(protect_at != 0);
        r = dm_m4_compute_submit(draw, &info, step);
        if (r != VK_SUCCESS) goto done;
        *step = name;
        if (memcmp(words, expected, sizeof words)) {
            for (uint32_t word = 0; word < GMEM_SMOKE_WORDS; ++word) {
                if (words[word] == expected[word]) continue;
                fprintf(stderr, "GMEM case %u guest word %u: got %08lx, expected %08lx\n", (unsigned)i,
                        (unsigned)word, (unsigned long)words[word], (unsigned long)expected[word]);
                break;
            }
            r = VK_ERROR_UNKNOWN;
            goto done;
        }
    }
    info.gmem.views = NULL;
    info.gmem.view_count = 0;
    // Exercise a large, importable host allocation beside small staged
    // aliases. Reuse its VA with fresh backing between GPU submissions.
    const size_t large_size = 16u << 20;
    const uint32_t last = (uint32_t)(large_size / sizeof(*large)) - 4;
    large = VirtualAlloc(NULL, large_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!large) { *step = "allocate large GMEM smoke pool"; r = VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
    pools.large = large;
    for (uint32_t pass = 1; pass <= 3; ++pass) {
        if (pass == 3) {
            dm_m4_draw_forget_imports(draw, large, large_size);
            if (!VirtualFree(large, large_size, MEM_DECOMMIT) ||
                !VirtualAlloc(large, large_size, MEM_COMMIT, PAGE_READWRITE)) {
                *step = "replace backing at the same GMEM host address"; r = VK_ERROR_UNKNOWN; goto done;
            }
        }
        const uint64_t base = (uintptr_t)large;
        const uint64_t addresses[4] = {base + last * sizeof(*large), base + 16, 0, 0};
        memcpy(words, addresses, sizeof addresses);
        for (uint32_t k = 0; k < 4; ++k) large[last + k] = (pass << 16) + k;
        large[3] = large[8] = UINT32_C(0xaabbccdd);
        r = dm_m4_compute_submit(draw, &info, step);
        if (r != VK_SUCCESS) goto done;
        if (memcmp(large + 4, large + last, 4 * sizeof(*large)) ||
            large[3] != UINT32_C(0xaabbccdd) || large[8] != UINT32_C(0xaabbccdd)) {
            *step = "observe coherent GMEM read/write across reuse and remapping"; r = VK_ERROR_UNKNOWN; goto done;
        }
    }
    pools.large = NULL;
    pools.malformed = 1;
    r = dm_m4_compute_submit(draw, &info, step);
    if (r == VK_ERROR_FEATURE_NOT_PRESENT) r = VK_SUCCESS;
    else if (r == VK_SUCCESS) {
        *step = "reject a live range whose endpoint violates the checked shader alignment contract";
        r = VK_ERROR_UNKNOWN;
    }
    if (r == VK_SUCCESS) r = smoke_shared_gmem(draw, device, &info, &bindings[packet], step);

done:
    dm_m4_draw_destroy(draw);
    if (large) VirtualFree(large, 0, MEM_RELEASE);
    dm_m4_shader_destroy(device, &cs);
    return r;
}

static VkResult smoke_read_alias(const struct dm_m4_device *device,
        const struct dm_m4_draw_requirements *requirements, const wchar_t *directory, const char **step) {
    static const uint8_t key[8] = {'g','m','e','m','r','e','a','d'};
    struct dm_m4_shader cs = {0};
    struct dm_m4_draw *draw = NULL;
    *step = "load the checked raw-read fixture";
    VkResult r = dm_m4_shader_load(device, directory, key, VK_SHADER_STAGE_COMPUTE_BIT, &cs);
    if (r != VK_SUCCESS) return r;
    struct dm_m4_draw_binding bindings[4] = {0};
    _Alignas(16) uint32_t words[GMEM_SMOKE_WORDS];
    struct gmem_smoke_pools pools = {.words = words};
    uint64_t address;
    uint32_t result[4];
    unsigned found = 0;
    if (cs.gmem == 1 && cs.binding_count <= _countof(bindings)) {
        for (uint32_t i = 0; i < cs.binding_count; ++i) {
            const struct dm_m4_shader_binding *b = &cs.bindings[i];
            if (b->source != DM_M4_SHADER_SBUF || b->sbuf.cb_slot) continue;
            if (!b->sbuf.flags && b->sbuf.cb_offset == DMSH_C0_SSBO_CS / 4) {
                bindings[i].buffer = (struct dm_m4_draw_span){&address, sizeof address};
                found |= 1u;
            } else if (b->sbuf.flags == 1 && b->sbuf.cb_offset == DMSH_C0_SSBO_CS / 4 + 4) {
                bindings[i].buffer = (struct dm_m4_draw_span){result, sizeof result};
                bindings[i].writeback = result;
                found |= 2u;
            }
        }
    }
    if (found != 3) { r = VK_ERROR_INVALID_SHADER_NV; goto done; }
    r = dm_m4_draw_create(&draw, device, requirements, step);
    if (r != VK_SUCCESS) goto done;
    const struct dm_m4_gmem_view view = {words + 4, 16, 1};
    struct dm_m4_compute_info info = {.cs = {&cs, bindings}, .groups = {1,1,1},
        .gmem = {.visit = smoke_visit_gmem, .context = &pools, .views = &view, .view_count = 1}};
    // Same map, writable view and program throughout. Change only the guest
    // pointer. A fault must not publish the staged SSBO or poison the next dispatch.
    const uint32_t offsets[] = {0, 16, 32};
    for (unsigned i = 0; i < _countof(offsets); ++i) {
        address = SMOKE_GUEST_BASE + offsets[i];
        for (unsigned k = 0; k < GMEM_SMOKE_WORDS; ++k) words[k] = UINT32_C(0x90300000) + k + 256 * i;
        const uint32_t canary[] = {1,2,3,4};
        memcpy(result, canary, sizeof result);
        r = dm_m4_compute_submit(draw, &info, step);
        const int blocked = offsets[i] == 16;
        if ((!blocked && r != VK_SUCCESS) || (blocked && r == VK_ERROR_DEVICE_LOST)) goto done;
        if ((blocked && r != VK_ERROR_FEATURE_NOT_PRESENT) ||
            memcmp(result, blocked ? canary : words + offsets[i] / 4, sizeof result)) {
            *step = "check the exact executed raw read before publishing output"; r = VK_ERROR_UNKNOWN; goto done;
        }
        r = VK_SUCCESS;
    }
done:
    dm_m4_draw_destroy(draw);
    dm_m4_shader_destroy(device, &cs);
    return r;
}

static int smoke_gmem_capabilities(const wchar_t *directory, uint32_t *count, uint32_t values[256])
{
    const wchar_t suffix[] = L"\\compute.vkc";
    const size_t n = wcslen(directory), tail = sizeof suffix / sizeof *suffix;
    if (n > SIZE_MAX / sizeof(wchar_t) - tail) return 0;
    wchar_t *path = malloc((n + tail) * sizeof *path);
    if (!path) return 0;
    memcpy(path, directory, n * sizeof *path);
    memcpy(path + n, suffix, sizeof suffix);
    FILE *f = NULL;
    const int opened = !_wfopen_s(&f, path, L"rb") && f;
    free(path);
    if (!opened) return 0;
    char magic[8];
    int ok = fread(magic, 1, sizeof magic, f) == sizeof magic && !memcmp(magic, "DMVKCSC1", sizeof magic) &&
        fread(count, sizeof *count, 1, f) == 1 && *count && *count <= 256 &&
        fread(values, sizeof *values, *count, f) == *count && fgetc(f) == EOF && !ferror(f);
    fclose(f);
    return ok;
}

static int run_gmem_smoke(const wchar_t *directory)
{
    uint32_t count, cs_caps[256];
    if (!smoke_gmem_capabilities(directory, &count, cs_caps)) {
        fputs("could not read GMEM fixture compute.vkc; regenerate with win_gmem_smoke_shader.py\n", stderr);
        return 1;
    }
    const struct dm_m4_draw_capabilities caps = {VK_SHADER_STAGE_COMPUTE_BIT, count, cs_caps};
    struct dm_m4_draw_requirements requirements;
    struct dm_m4_window *window = NULL;
    const char *step = NULL;
    VkResult r = dm_m4_draw_requirements_init(&requirements, 0, &caps, 1, &step);
    if (r == VK_SUCCESS) {
        step = "create GMEM smoke Vulkan device";
        r = dm_m4_window_create(&window, L"GMEM dispatch smoke", 320, 180, &requirements.requirements);
    }
    if (r == VK_SUCCESS)
        r = smoke_gmem_dispatch(dm_m4_window_get_device(window), &requirements, directory, &step);
    if (r == VK_SUCCESS)
        r = smoke_read_alias(dm_m4_window_get_device(window), &requirements, directory, &step);
    if (window) dm_m4_window_destroy(window);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "%s: VkResult %d\n", step ? step : "GMEM dispatch smoke", (int)r);
        return 1;
    }
    puts("Checked compute GMEM: real guest read/write, overlapping views and address guards passed");
    return 0;
}
#undef SMOKE_GUEST_BASE
