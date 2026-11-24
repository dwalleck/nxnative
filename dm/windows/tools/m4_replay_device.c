#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <wchar.h>
#include "m4_replay_device.h"
#include "m4_draw.h"

struct shader_entry {
    struct shader_entry *next;
    uint8_t key[8];
    struct dm_m4_shader shader;
};
struct dm_replay_device {
    wchar_t *dir;
    struct dm_m4_draw_requirements requirements;
    struct dm_m4_draw *draw;
    const struct dm_m4_device *device;
    int has_compute;
    // Manifest memory retained to check that lazily loaded modules match it.
    uint32_t compute_count, compute_caps[256];
    uint32_t mesh_count[2], mesh_caps[256]; // MS followed by TS
    struct shader_entry *shaders[256];
};

static int ordered_caps(const uint32_t *caps, uint32_t count) {
    for (uint32_t i = 1; i < count; ++i)
        if (caps[i - 1] >= caps[i]) return 0;
    return 1;
}

VkResult dm_replay_config(struct dm_replay_device **out, const struct dm_m4_device_requirements **req) {
    *req = NULL;
    *out = NULL;
    wchar_t *directory = NULL;
    size_t n = 0;
    if (_wdupenv_s(&directory, &n, L"DM_VK_SHADERS")) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!directory || n < 2) { free(directory); return VK_SUCCESS; }
    struct dm_replay_device *r = calloc(1, sizeof(*r));
    if (!r) { free(directory); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    r->dir = directory;
    static const wchar_t suffix[] = L"\\graphics.vkc";
    wchar_t *path = malloc((n - 1 + _countof(suffix)) * sizeof(*path));
    if (!path) { dm_replay_destroy(r); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    memcpy(path, directory, (n - 1) * sizeof(*path));
    memcpy(path + n - 1, suffix, sizeof(suffix));
    FILE *f;
    errno_t err = _wfopen_s(&f, path, L"rb");
    if (err || !f) {
        free(path);
        fprintf(stderr, "[RED] Vulkan: missing graphics.vkc in DM_VK_SHADERS; rebuild the library with win_nvn_shaders.py\n");
        dm_replay_destroy(r); return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint32_t count[2], values[256];
    char magic[8];
    int valid = fread(magic, 1, 8, f) == 8 && !memcmp(magic, "DMVKCAP1", 8) &&
        fread(count, sizeof(*count), 2, f) == 2 && count[0] <= _countof(values) && count[1] <= _countof(values) - count[0] &&
        fread(values, sizeof(*values), count[0] + count[1], f) == count[0] + count[1] && fgetc(f) == EOF && !ferror(f);
    fclose(f);
    if (!valid) { free(path); dm_replay_destroy(r); return VK_ERROR_INVALID_SHADER_NV; }
    static const wchar_t compute_suffix[] = L"\\compute.vkc";
    static_assert(sizeof(compute_suffix) <= sizeof(suffix), "manifest path capacity");
    memcpy(path + n - 1, compute_suffix, sizeof(compute_suffix));
    f = NULL;
    err = _wfopen_s(&f, path, L"rb");
    // Keep the path buffer for the optional mesh manifest below.
    if (!err && f) {
        valid = fread(magic, 1, 8, f) == 8 && !memcmp(magic, "DMVKCSC1", 8) &&
            fread(&r->compute_count, sizeof(r->compute_count), 1, f) == 1 &&
            r->compute_count && r->compute_count <= _countof(r->compute_caps) &&
            fread_s(r->compute_caps, sizeof(r->compute_caps), sizeof(*r->compute_caps), r->compute_count, f) == r->compute_count &&
            fgetc(f) == EOF && !ferror(f);
        fclose(f);
        valid = valid && ordered_caps(r->compute_caps, r->compute_count);
        if (!valid) {
            free(path);
            fprintf(stderr, "[RED] Vulkan: invalid compute.vkc in DM_VK_SHADERS\n");
            dm_replay_destroy(r); return VK_ERROR_INVALID_SHADER_NV;
        }
        r->has_compute = 1;
    } else if (err != ENOENT) {
        free(path);
        fprintf(stderr, "[RED] Vulkan: could not open compute.vkc in DM_VK_SHADERS (%d)\n", (int)err);
        dm_replay_destroy(r); return VK_ERROR_INITIALIZATION_FAILED;
    }
    static const wchar_t mesh_suffix[] = L"\\mesh.vkc";
    static_assert(sizeof(mesh_suffix) <= sizeof(suffix), "mesh manifest path capacity");
    memcpy(path + n - 1, mesh_suffix, sizeof(mesh_suffix));
    f = NULL;
    err = _wfopen_s(&f, path, L"rb");
    free(path);
    if (!err && f) {
        uint32_t *mc = r->mesh_count;
        valid = fread(magic, 1, 8, f) == 8 && !memcmp(magic, "DMVKMSC1", 8) &&
            fread(mc, sizeof(*mc), 2, f) == 2 && mc[0] && mc[0] <= _countof(r->mesh_caps) &&
            mc[1] <= _countof(r->mesh_caps) - mc[0] &&
            fread(r->mesh_caps, sizeof(*r->mesh_caps), mc[0] + mc[1], f) == mc[0] + mc[1] &&
            fgetc(f) == EOF && !ferror(f) && ordered_caps(r->mesh_caps, mc[0]) &&
            ordered_caps(r->mesh_caps + mc[0], mc[1]);
        fclose(f);
        if (!valid) {
            fprintf(stderr, "[RED] Vulkan: invalid mesh.vkc in DM_VK_SHADERS\n");
            dm_replay_destroy(r); return VK_ERROR_INVALID_SHADER_NV;
        }
    } else if (err != ENOENT) {
        fprintf(stderr, "[RED] Vulkan: could not open mesh.vkc in DM_VK_SHADERS (%d)\n", (int)err);
        dm_replay_destroy(r); return VK_ERROR_INITIALIZATION_FAILED;
    }
    struct dm_m4_draw_capabilities caps[5] = {
        {VK_SHADER_STAGE_VERTEX_BIT, count[0], values},
        {VK_SHADER_STAGE_FRAGMENT_BIT, count[1], values + count[0]},
    };
    uint32_t stages = 2;
    if (r->has_compute)
        caps[stages++] = (struct dm_m4_draw_capabilities){VK_SHADER_STAGE_COMPUTE_BIT, r->compute_count, r->compute_caps};
    if (r->mesh_count[0])
        caps[stages++] = (struct dm_m4_draw_capabilities){VK_SHADER_STAGE_MESH_BIT_EXT, r->mesh_count[0], r->mesh_caps};
    if (r->mesh_count[1])
        caps[stages++] = (struct dm_m4_draw_capabilities){VK_SHADER_STAGE_TASK_BIT_EXT, r->mesh_count[1], r->mesh_caps + r->mesh_count[0]};
    const char *step = "device requirements";
    VkResult result = dm_m4_draw_requirements_init(&r->requirements,
        DM_M4_DRAW_NEGATIVE_ONE_TO_ONE | DM_M4_DRAW_BC | DM_M4_DRAW_ASTC_LDR | DM_M4_DRAW_ANISOTROPY |
        DM_M4_DRAW_DEPTH_CLAMP | DM_M4_DRAW_DEPTH_BIAS_CLAMP | DM_M4_DRAW_DEPTH_BOUNDS | DM_M4_DRAW_CONSERVATIVE_RASTER |
        DM_M4_DRAW_INDEPENDENT_BLEND | DM_M4_DRAW_VERTEX_STORAGE_WRITES | DM_M4_DRAW_FRAGMENT_STORAGE_WRITES |
        DM_M4_DRAW_NON_SOLID | DM_M4_DRAW_WIDE_LINES | DM_M4_DRAW_LARGE_POINTS | DM_M4_DRAW_SHADING_RATE_IMAGE,
        caps, stages, &step);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[RED] Vulkan: %s (VkResult %d)\n", step, (int)result);
        dm_replay_destroy(r); return result;
    }
    *out = r;
    *req = &r->requirements.requirements;
    return VK_SUCCESS;
}
VkResult dm_replay_start(struct dm_replay_device *r, const struct dm_m4_device *device) {
    if (!r) return VK_SUCCESS;
    const char *step = "draw context";
    r->device = device;
    VkResult result = dm_m4_draw_create(&r->draw, device, &r->requirements, &step);
    if (result != VK_SUCCESS) fprintf(stderr, "[RED] Vulkan: %s (VkResult %d)\n", step, (int)result);
    return result;
}
static const struct dm_m4_shader *cached_shader(struct dm_replay_device *r, VkShaderStageFlagBits stage, const uint8_t key[8]) {
    struct shader_entry **bucket = &r->shaders[key[0]];
    for (const struct shader_entry *e = *bucket; e; e = e->next)
        if (e->shader.stage == stage && !memcmp(e->key, key, 8)) return &e->shader;
    struct shader_entry *e = malloc(sizeof(*e));
    if (!e) return NULL;
    VkResult result = dm_m4_shader_load(r->device, r->dir, key, stage, &e->shader);
    const uint32_t *manifest = NULL;
    uint32_t count = 0;
    const char *file = NULL, *stage_name = NULL;
    switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT: stage_name = "VS"; break;
    case VK_SHADER_STAGE_FRAGMENT_BIT: stage_name = "PS"; break;
    case VK_SHADER_STAGE_COMPUTE_BIT:
        stage_name = "CS"; file = "compute.vkc"; manifest = r->compute_caps; count = r->compute_count; break;
    case VK_SHADER_STAGE_MESH_BIT_EXT:
        stage_name = "MS"; file = "mesh.vkc"; manifest = r->mesh_caps; count = r->mesh_count[0]; break;
    case VK_SHADER_STAGE_TASK_BIT_EXT:
        stage_name = "TS"; file = "mesh.vkc"; manifest = r->mesh_caps + r->mesh_count[0]; count = r->mesh_count[1]; break;
    default: stage_name = "unknown stage"; break;
    }
    if (result == VK_SUCCESS && manifest) {
        const uint32_t *caps = e->shader.capabilities;
        for (uint32_t i = 0; i < e->shader.capability_count; ++i) {
            uint32_t j = 0;
            while (j < count && manifest[j] < caps[i]) ++j;
            if (j < count && manifest[j] == caps[i]) continue;
            fprintf(stderr, "[RED] Vulkan: SPIR-V capability %u absent from %s (%s)\n", caps[i], file, stage_name);
            dm_m4_shader_destroy(r->device, &e->shader);
            result = VK_ERROR_FEATURE_NOT_PRESENT;
            break;
        }
    }
    if (result != VK_SUCCESS) {
        fprintf(stderr, "[RED] Vulkan: could not load %s ", stage_name);
        for (unsigned i = 0; i < 8; ++i) fprintf(stderr, "%02x", key[i]);
        fprintf(stderr, " (VkResult %d)\n", (int)result);
        free(e); return NULL;
    }
    memcpy(e->key, key, 8);
    e->next = *bucket;
    *bucket = e;
    return &e->shader;
}
const char *dm_replay_shaders(struct dm_replay_device *r, const struct dm_m4_replay_keys *keys,
                              struct dm_m4_replay_session *out) {
    if (!r || !r->draw) return "no Vulkan draw device (set DM_WINDOW and DM_VK_SHADERS)";
    if (keys->ms && !r->mesh_count[0]) return "no mesh.vkc in DM_VK_SHADERS";
    if (keys->ts && !r->mesh_count[1]) return "no task shaders in mesh.vkc";
    struct dm_m4_replay_session session = {.draw = r->draw};
    if (keys->vs) session.vs = cached_shader(r, VK_SHADER_STAGE_VERTEX_BIT, keys->vs);
    if (keys->ps) session.ps = cached_shader(r, VK_SHADER_STAGE_FRAGMENT_BIT, keys->ps);
    if (keys->ms) session.ms = cached_shader(r, VK_SHADER_STAGE_MESH_BIT_EXT, keys->ms);
    if (keys->ts) session.ts = cached_shader(r, VK_SHADER_STAGE_TASK_BIT_EXT, keys->ts);
    if ((keys->vs && !session.vs) || (keys->ps && !session.ps) ||
        (keys->ms && !session.ms) || (keys->ts && !session.ts)) return "required Vulkan shader unavailable";
    *out = session;
    return NULL;
}
const char *dm_replay_compute_shader(struct dm_replay_device *r, const uint8_t cs[8],
                                     struct dm_m4_compute_session *out) {
    if (!r || !r->draw) return "no Vulkan compute device (set DM_WINDOW and DM_VK_SHADERS)";
    if (!r->has_compute) return "no compute.vkc in DM_VK_SHADERS";
    const struct dm_m4_shader *shader = cached_shader(r, VK_SHADER_STAGE_COMPUTE_BIT, cs);
    if (!shader) return "required Vulkan compute shader unavailable";
    *out = (struct dm_m4_compute_session){r->draw, shader};
    return NULL;
}
void dm_replay_destroy(struct dm_replay_device *r) {
    if (!r) return;
    if (r->draw) dm_m4_draw_destroy(r->draw);
    for (unsigned i = 0; i < _countof(r->shaders); ++i) {
        struct shader_entry *entry = r->shaders[i];
        while (entry) {
            struct shader_entry *next = entry->next;
            dm_m4_shader_destroy(r->device, &entry->shader);
            free(entry);
            entry = next;
        }
    }
    free(r->dir);
    free(r);
}
