#define VK_NO_PROTOTYPES
#include "m4_guest_resources.h"
#include "m4_replay.h"
#include "m4_overlay.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static uint32_t max_index(const uint8_t *indices, uint32_t count, size_t index_size, uint64_t skip, uint32_t *minimum)
{
    uint32_t maximum = 0;
    *minimum = UINT32_MAX;
    // The guest indices may be unaligned. Keep each load a fixed width so the
    // scan never needs a memcpy call per index.
    if (index_size == 1) {
        for (uint32_t i = 0; i < count; ++i) {
            if (indices[i] == skip) continue;
            if (indices[i] > maximum) maximum = indices[i];
            if (indices[i] < *minimum) *minimum = indices[i];
        }
    } else if (index_size == 2) {
        for (uint32_t i = 0; i < count; ++i, indices += sizeof(uint16_t)) {
            uint16_t index;
            memcpy(&index, indices, sizeof(index));
            if (index == skip) continue;
            if (index > maximum) maximum = index;
            if (index < *minimum) *minimum = index;
        }
    } else {
        for (uint32_t i = 0; i < count; ++i, indices += sizeof(uint32_t)) {
            uint32_t index;
            memcpy(&index, indices, sizeof(index));
            if (index == skip) continue;
            if (index > maximum) maximum = index;
            if (index < *minimum) *minimum = index;
        }
    }
    return maximum;
}

static const char *bind_geometry(const struct dm_m4_draw_state *state,
                                 const struct dm_m4_indexed_draw *draw,
                                 const struct dm_m4_draw_resolver *resolve,
                                 struct dm_m4_nvn_vertex_input *vertex,
                                 struct dm_m4_draw_info *info)
{
    info->pipeline.topology = dm_m4_nvn_topology(draw->primitive);
    switch (info->pipeline.topology) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
        break;
    default:
        return "unsupported NVN indexed draw topology";
    }

    size_t index_size = 0;
    const uint8_t *indices = NULL;
    if (draw->arrays) {
        if (draw->base_vertex < 0 || draw->count - 1u > UINT32_MAX - (uint32_t)draw->base_vertex)
            return "array draw vertex range overflows";
        info->vertex_count = draw->count;
    } else {
        info->index_type = dm_m4_nvn_index_type(draw->index_type);
        switch (info->index_type) {
        case VK_INDEX_TYPE_UINT8_EXT: index_size = 1; break;
        case VK_INDEX_TYPE_UINT16: index_size = 2; break;
        case VK_INDEX_TYPE_UINT32: index_size = 4; break;
        default: return "invalid indexed draw index type";
        }
        if (draw->count > SIZE_MAX / index_size) return "indexed draw index range overflows";
        size_t index_bytes = draw->count * index_size;
        indices = resolve->buffer(resolve->context, draw->indices, index_bytes, 0);
        if (!indices) return "indexed draw indices are outside guest buffer memory";
        info->indices = (struct dm_m4_draw_span){indices, index_bytes};
        info->pipeline.primitive_restart_enable = state->restart_enable;
        info->restart_index = state->restart_index;
        info->index_count = draw->count;
    }
    info->instance_count = draw->instances;
    info->first_instance = draw->first_instance;
    info->base_vertex = draw->base_vertex;

    if (state->attrib_count > DM_M4_DRAW_ATTRIBS || state->stream_count > DM_M4_DRAW_STREAMS)
        return "indexed draw vertex state exceeds the guest slot count";
    VkResult result = dm_m4_nvn_vertex_input(state->attribs, state->attrib_count,
                                            state->streams, state->stream_count, vertex);
    if (result == VK_ERROR_FORMAT_NOT_SUPPORTED) return "unsupported NVN vertex attribute format";
    if (result != VK_SUCCESS) return "NVN vertex attribute references an unconfigured stream";
    info->pipeline.vertex_input = &vertex->state;

    // Upload only the prefix the indexed fetch can read, without rebasing the
    // vertex index or instance index seen by the shader.
    uint32_t extent[DM_M4_DRAW_STREAMS] = {0};
    for (uint32_t i = 0; i < vertex->state.vertexAttributeDescriptionCount; ++i) {
        const VkVertexInputAttributeDescription *attr = &vertex->attributes[i];
        int format = (uint8_t)state->attribs[attr->location].reserved[0];
        int bytes = nvn_fmt_bpb(format, NULL, NULL);
        if (!bytes) return "NVN vertex attribute byte width is unsupported";
        uint32_t end = attr->offset + (uint32_t)bytes;
        if (end > extent[attr->binding]) extent[attr->binding] = end;
    }
    uint32_t maximum = 0;
    for (uint32_t i = 0; i < vertex->state.vertexBindingDescriptionCount; ++i) {
        const VkVertexInputBindingDescription *binding = &vertex->bindings[i];
        if (binding->inputRate == VK_VERTEX_INPUT_RATE_VERTEX && binding->stride) {
            if (draw->arrays) {
                maximum = (uint32_t)draw->base_vertex + draw->count - 1u;
                break;
            }
            uint32_t minimum;
            uint64_t skip = state->restart_enable ? (uint64_t)state->restart_index : UINT64_MAX;
            uint32_t high = max_index(indices, draw->count, index_size, skip, &minimum);
            if (minimum == UINT32_MAX && high == 0) { // Only restart markers: no vertex fetch or invocation.
                info->index_count = 0;
                return NULL;
            }
            int64_t last = (int64_t)high + draw->base_vertex;
            if ((int64_t)minimum + draw->base_vertex < 0 || last > UINT32_MAX)
                return "base vertex moves an index outside the vertex stream";
            maximum = (uint32_t)last;
            break;
        }
    }
    for (uint32_t i = 0; i < vertex->state.vertexBindingDescriptionCount; ++i) {
        const VkVertexInputBindingDescription *binding = &vertex->bindings[i];
        uint32_t slot = binding->binding;
        uint64_t last = maximum;
        if (binding->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE) {
            // NVN stream word 1 is its fetch divisor, as in dm_m4_nvn_vertex_input.
            uint32_t divisor;
            memcpy(&divisor, (const uint8_t *)&state->streams[slot] + 4, sizeof(divisor));
            last = (uint64_t)draw->first_instance + (draw->instances - 1u) / divisor;
        }
        if (last && binding->stride > (SIZE_MAX - extent[slot]) / last)
            return "indexed draw vertex fetch range overflows";
        size_t bytes = (size_t)last * binding->stride + extent[slot];
        const NVNbufferRange *range = &state->vertex[slot];
        const size_t valid = bytes < range->size ? bytes : range->size;
        const void *data = valid ? resolve->buffer(resolve->context, range->address, valid, 0) : NULL;
        if (valid && !data) return "indexed draw vertex stream is outside guest buffer memory";
        info->vertices[slot] = (struct dm_m4_draw_span){data, valid};
        info->vertex_zero_tail[slot] = bytes - valid;
    }
    return NULL;
}

static int32_t clip_edge(int64_t edge, int32_t limit)
{
    return edge <= 0 ? 0 : edge >= limit ? limit : (int32_t)edge;
}

static int valid_origin(NVNwindowOriginMode origin)
{
    return origin == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT || origin == NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT;
}

static VkExtent2D intersect_target(VkExtent2D extent, const struct dm_m4_draw_attachment *target)
{
    if (target->format == VK_FORMAT_UNDEFINED) return extent;
    return (VkExtent2D){extent.width && extent.width < target->width ? extent.width : target->width,
                        extent.height && extent.height < target->height ? extent.height : target->height};
}

static const char *set_coordinates(const struct dm_m4_draw_state *state,
                                   const struct dm_m4_raster_modes *modes,
                                   struct dm_m4_draw_info *info)
{
    if ((state->viewport_set | state->scissor_set) & ~1u)
        return "draw supports only viewport and scissor slot zero";
    NVNwindowOriginMode origin = state->viewport_set & 1u ? state->viewport_origin[0] : modes->origin;
    if (!valid_origin(origin) || !valid_origin(modes->queue_origin)) return "invalid NVN window origin";
    if (modes->depth_mode != NVN_DEPTH_MODE_NEAR_IS_ZERO && modes->depth_mode != NVN_DEPTH_MODE_NEAR_IS_MINUS_W)
        return "invalid NVN clip depth mode";

    VkExtent2D extent = intersect_target((VkExtent2D){0}, &info->depth);
    for (unsigned i = 0; i < info->color_count; ++i)
        extent = intersect_target(extent, &info->colors[i]);
    if (!info->color_count && info->depth.format == VK_FORMAT_UNDEFINED) {
        // Without images, NVN still rasterizes shader side effects. Its pass
        // covers viewport zero, else the scissor, with a 1x1 minimum.
        double right = 1.0, bottom = 1.0;
        if (state->viewport_set & 1u) {
            const struct dm_m4_draw_viewport *v = &state->viewports[0];
            right = (double)v->x + v->width;
            bottom = (double)v->y + v->height;
        } else if (state->scissor_set & 1u) {
            const struct dm_m4_draw_scissor *s = &state->scissors[0];
            right = (double)s->x + s->width;
            bottom = (double)s->y + s->height;
        }
        extent = (VkExtent2D){(uint32_t)fmin(fmax(ceil(right), 1.0), 16384.0),
                              (uint32_t)fmin(fmax(ceil(bottom), 1.0), 16384.0)};
    }
    if (!extent.width || !extent.height || extent.width > INT32_MAX || extent.height > INT32_MAX)
        return "draw render extent is outside Vulkan's signed scissor range";
    info->extent = extent;
    int32_t width = (int32_t)extent.width, height = (int32_t)extent.height;
    struct dm_m4_draw_viewport viewport = state->viewport_set & 1u ? state->viewports[0] :
        (struct dm_m4_draw_viewport){0.0f, 0.0f, (float)width, (float)height};
    if (!(viewport.width > 0.0f && viewport.height > 0.0f) ||
        !isfinite(viewport.x + viewport.width) || !isfinite(viewport.y + viewport.height))
        return "NVN viewport is empty or nonfinite";
    struct dm_m4_draw_depth_range depth = state->depth_ranges[0];
    if (!(depth.near_z >= 0.0f && depth.near_z <= 1.0f && depth.far_z >= 0.0f && depth.far_z <= 1.0f))
        return "NVN viewport depth range lies outside [0, 1]";

    // NVN rectangles address the same storage rows in either window origin.
    // A negative Vulkan viewport height reverses clip Y inside the rectangle.
    int upper_left = origin == NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT;
    info->viewport = (VkViewport){viewport.x, upper_left ? viewport.y + viewport.height : viewport.y,
        viewport.width, upper_left ? -viewport.height : viewport.height, depth.near_z, depth.far_z};
    info->pipeline.negative_one_to_one = modes->depth_mode == NVN_DEPTH_MODE_NEAR_IS_MINUS_W;
    info->support.viewport_size[0] = viewport.width;
    info->support.viewport_size[1] = viewport.height;
    info->support.viewport_inverse[0] = 2.0f / viewport.width;
    info->support.viewport_inverse[1] = 2.0f / viewport.height;

    struct dm_m4_draw_scissor rect = state->scissor_set & 1u ? state->scissors[0] :
        (struct dm_m4_draw_scissor){0, 0, width, height};
    if (rect.width < 0 || rect.height < 0) return "NVN scissor has a negative extent";
    int32_t x0 = clip_edge(rect.x, width), x1 = clip_edge((int64_t)rect.x + rect.width, width);
    int32_t y0 = clip_edge(rect.y, height), y1 = clip_edge((int64_t)rect.y + rect.height, height);
    info->scissor = (VkRect2D){{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    return NULL;
}

struct guest_raster {
    VkPipelineRasterizationStateCreateInfo raster;
    VkPipelineRasterizationConservativeStateCreateInfoEXT conservative;
    VkPipelineMultisampleStateCreateInfo samples;
    VkPipelineDepthStencilStateCreateInfo depth;
    struct dm_m4_nvn_blend blend;
};

static const char *set_raster(const struct dm_m4_draw_state *state,
                              const struct dm_m4_raster_modes *modes, NVNdrawPrimitive primitive,
                              struct guest_raster *raster, struct dm_m4_draw_info *info)
{
    // Front-face convention is latched by queue initialization; changing the
    // device or recorded viewport origin can reverse actual facing, as on NVN.
    VkBool32 flip = modes->queue_origin == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT;
    const struct dm_m4_draw_polygon_offset *offset = &state->polygon_offset;
    const float width = state->rasterizer_discard ? 1.0f : state->line_width;
    if (dm_m4_nvn_rasterization(&state->polygon, primitive, flip, state->depth_clamp, width,
                                offset->factor, offset->units, offset->clamp, &raster->raster) != VK_SUCCESS)
        return "unsupported NVN polygon rasterization state";
    raster->raster.rasterizerDiscardEnable = state->rasterizer_discard;
    const int points = primitive == NVN_DRAW_PRIMITIVE_POINTS ||
        ((primitive == NVN_DRAW_PRIMITIVE_TRIANGLES || primitive == NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP ||
          primitive == NVN_DRAW_PRIMITIVE_TRIANGLE_FAN) && raster->raster.polygonMode == VK_POLYGON_MODE_POINT);
    unsigned last_stage = info->ms.shader ? 1u << DM_M4_NV_MESH_STAGE : NVN_SHADER_STAGE_VERTEX_BIT;
    if (points && !state->rasterizer_discard && !(state->point_size_outputs & last_stage)) {
        if (!(state->point_size > 0.0f) || !isfinite(state->point_size)) return "invalid NVN fixed point size";
        if (state->point_size != 1.0f) info->point_size = state->point_size;
    }
    if (state->conservative_raster) {
        raster->conservative = (VkPipelineRasterizationConservativeStateCreateInfoEXT){
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT,
            .conservativeRasterizationMode = VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT};
        raster->raster.pNext = &raster->conservative;
    }
    if (dm_m4_nvn_multisample(&state->multisample, VK_SAMPLE_COUNT_1_BIT, &raster->samples) != VK_SUCCESS)
        return "draw does not support the NVN multisample or sample-location state";
    raster->samples.pSampleMask = &state->sample_mask;
    struct dm_m4_nvn_stencil_values stencil[2];
    for (unsigned i = 0; i < 2; ++i) {
        const struct dm_m4_draw_stencil *face = &state->stencil[i];
        stencil[i] = (struct dm_m4_nvn_stencil_values){face->compare_mask, face->write_mask, face->reference};
    }
    VkBool32 stencil_only = info->depth.format == VK_FORMAT_S8_UINT;
    if (dm_m4_nvn_depth_stencil(&state->depth_stencil, !stencil_only && info->depth.pixels.data,
                                stencil_only || info->depth.stencil.data, stencil, &raster->depth) != VK_SUCCESS)
        return "invalid NVN depth or stencil comparison/operation";
    if (state->depth_bounds_test && !stencil_only && info->depth.pixels.data) {
        raster->depth.depthBoundsTestEnable = VK_TRUE;
        raster->depth.minDepthBounds = state->depth_bounds.near_z;
        raster->depth.maxDepthBounds = state->depth_bounds.far_z;
    }

    if (info->ps.shader && (state->color.reserved[3] & 15u) != NVN_ALPHA_FUNC_ALWAYS)
        return "NVN alpha testing requires a fragment shader variant and is unsupported";
    if (info->color_count) {
        VkFormat formats[DM_M4_NVN_COLOR_TARGETS];
        for (unsigned i = 0; i < info->color_count; ++i) formats[i] = info->colors[i].format;
        VkResult result = dm_m4_nvn_blend(&state->color, &state->channel_mask, state->blend,
                                         formats, info->color_count, info->ps.shader ? info->ps.shader->frag_write_mask : 0,
                                         state->blend_color, &raster->blend);
        if (result == VK_ERROR_FEATURE_NOT_PRESENT) return "advanced NVN color blending is unsupported";
        if (result != VK_SUCCESS) return "invalid NVN logic operation or blend equation/factor";
        for (unsigned i = 0; i < info->color_count; ++i) {
            const VkPipelineColorBlendAttachmentState *blend = &raster->blend.attachments[i];
            if (blend->blendEnable &&
                (blend->srcColorBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR ||
                 blend->dstColorBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR ||
                 blend->srcAlphaBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR ||
                 blend->dstAlphaBlendFactor >= VK_BLEND_FACTOR_SRC1_COLOR))
                return "NVN dual-source blending requires a fragment shader variant and is unsupported";
        }
        info->pipeline.blend = &raster->blend.state;
    }

    info->pipeline.rasterization = &raster->raster;
    info->pipeline.multisample = &raster->samples;
    info->pipeline.depth_stencil = &raster->depth;
    return NULL;
}

static const void *overlay_target(const struct dm_m4_draw_info *info, const struct dm_m4_draw_state *state,
                                  const struct dm_m4_draw_resolver *resolve)
{
    if (!info->color_count || !resolve->texture_storage || !info->scissor.extent.width || !info->scissor.extent.height ||
        (info->pipeline.rasterization && info->pipeline.rasterization->rasterizerDiscardEnable)) return NULL;
    const struct dm_m4_draw_texture *target = &state->colors[0];
    if (!target->pool || target->desc.target != NVN_TEXTURE_TARGET_2D || target->desc.samples > 1 ||
        (target->view.set & DM_M4_VIEW_LEVELS && target->view.base_level) ||
        (target->view.set & DM_M4_VIEW_LAYERS && target->view.base_layer) ||
        target->desc.width != info->colors[0].width || target->desc.height != info->colors[0].height) return NULL;
    nvn_texlayout layout;
    return resolve->texture_storage(resolve->context, target, &layout, 0);
}

static const char *submit_graphics(struct dm_m4_guest_resources *resources,
                                   const struct dm_m4_draw_state *state, const struct dm_m4_draw_resolver *resolve,
                                   const struct dm_m4_replay_session *session, const struct dm_m4_raster_modes *modes,
                                   NVNdrawPrimitive primitive, struct dm_m4_draw_info *info,
                                   enum dm_m4_overlay_role role)
{
    info->vs.shader = session->vs;
    info->ps.shader = session->ps;
    info->ms.shader = session->ms;
    info->ts.shader = session->ts;
    if (resources->count_samples) info->samples_passed = &resources->samples_passed;
    dm_m4_draw_support_init(&info->support);

    struct guest_raster raster;
    const char *error = dm_m4_guest_resources_prepare(resources, state, resolve, info);
    if (!error) error = set_coordinates(state, modes, info);
    if (!error) error = set_raster(state, modes, primitive, &raster, info);
    if (!error) {
        const char *step = NULL;
        struct dm_m4_draw_attachment_writes writes;
        info->attachment_writes = &writes;
        const int capture = role != DM_M4_OVERLAY_NONE &&
            dm_m4_overlay_draw_begin(session->overlay, role, info, overlay_target(info, state, resolve));
        if (dm_m4_draw_submit(session->draw, info, &step) == VK_SUCCESS)
            error = dm_m4_guest_resources_writeback(resources, &writes);
        else
            error = step ? step : "Vulkan draw submission failed";
        if (capture) dm_m4_overlay_draw_end(session->overlay, role, info, !error);
    }
    resources->writeback = NULL;
    return error;
}

const char *dm_m4_execute_nvn_indexed(struct dm_m4_guest_resources *resources,
                                     const struct dm_m4_draw_state *state,
                                     const struct dm_m4_indexed_draw *draw,
                                     const struct dm_m4_draw_resolver *resolve)
{
    if (!resources || !state || !draw || !resolve || !resolve->buffer || !draw->guest_device)
        return "indexed draw is missing its guest device, state, or buffer resolver";
    if (!draw->count) return NULL; // A zero-index command invokes no stages.
    if (state->geometry_mode != 0) return "indexed draw requires classic NVN geometry processing mode";

    // Mesh/task and compute stages are only invoked by their own commands.
    const uint32_t stages = state->shader_mask & ~((1u << DM_M4_NV_TASK_STAGE) |
        (1u << DM_M4_NV_MESH_STAGE) | NVN_SHADER_STAGE_COMPUTE_BIT);
    if (stages & NVN_SHADER_STAGE_GEOMETRY_BIT) return "indexed draw has an unsupported geometry shader";
    if (stages & (NVN_SHADER_STAGE_TESS_CONTROL_BIT | NVN_SHADER_STAGE_TESS_EVALUATION_BIT))
        return "indexed draw has unsupported tessellation shaders";
    if (!(state->shader_mask & NVN_SHADER_STAGE_VERTEX_BIT)) return "indexed draw has no active vertex shader";

    struct dm_m4_draw_info info = {0};
    struct dm_m4_nvn_vertex_input vertex;
    const char *error = bind_geometry(state, draw, resolve, &vertex, &info);
    if (error) return error;
    if (!info.index_count && !info.vertex_count) return NULL;

    struct dm_m4_replay_session session;
    const struct dm_m4_replay_keys keys = {
        .vs = state->shader_keys[NVN_SHADER_STAGE_VERTEX],
        .ps = stages & NVN_SHADER_STAGE_FRAGMENT_BIT ? state->shader_keys[NVN_SHADER_STAGE_FRAGMENT] : NULL,
    };
    error = dm_m4_replay_begin(draw->guest_device, &keys, &session);
    if (error) return error;
    const enum dm_m4_overlay_role role = session.overlay ?
        dm_m4_overlay_select(session.overlay, &keys) : DM_M4_OVERLAY_NONE;
    error = submit_graphics(resources, state, resolve, &session, &draw->raster, draw->primitive, &info, role);
    if (!error) {
        // The replay lock serializes submissions and this first-use report.
        static unsigned reported_gmem;
        const struct dm_m4_shader *const shaders[] = {session.vs, session.ps};
        const uint8_t *const ids[] = {keys.vs, keys.ps};
        for (unsigned i = 0; i < 2; ++i) {
            if (!shaders[i] || !shaders[i]->gmem || (reported_gmem & (1u << i))) continue;
            reported_gmem |= 1u << i;
            printf("[nvn] first indexed draw with checked %s global memory completed (", i ? "PS" : "VS");
            for (unsigned j = 0; j < 8; ++j) printf("%02x", (unsigned)ids[i][j]);
            puts(")");
            fflush(stdout);
        }
    }
    dm_m4_replay_end();
    return error;
}

const char *dm_m4_execute_nvn_mesh(struct dm_m4_guest_resources *resources,
                                  const struct dm_m4_draw_state *state,
                                  const struct dm_m4_mesh_draw *draw,
                                  const struct dm_m4_draw_resolver *resolve)
{
    if (!resources || !state || !draw || !resolve || !resolve->buffer || !draw->guest_device)
        return "mesh draw is missing its guest device, state, or buffer resolver";
    if (!draw->count) return NULL;
    if (state->geometry_mode == 0) return "mesh draw requires NVN mesh geometry processing mode";

    const uint32_t stages = state->shader_mask;
    if (!(stages & (1u << DM_M4_NV_MESH_STAGE))) return "mesh draw has no active mesh shader";
    const struct dm_m4_replay_keys keys = {
        .ms = state->shader_keys[DM_M4_NV_MESH_STAGE],
        .ts = stages & (1u << DM_M4_NV_TASK_STAGE) ? state->shader_keys[DM_M4_NV_TASK_STAGE] : NULL,
        .ps = stages & NVN_SHADER_STAGE_FRAGMENT_BIT ? state->shader_keys[NVN_SHADER_STAGE_FRAGMENT] : NULL,
    };
    struct dm_m4_replay_session session;
    const char *error = dm_m4_replay_begin(draw->guest_device, &keys, &session);
    if (error) return error;
    const NVNdrawPrimitive primitive = session.ms->mesh_topology == 1 ? NVN_DRAW_PRIMITIVE_POINTS :
                                       session.ms->mesh_topology == 2 ? NVN_DRAW_PRIMITIVE_LINES : NVN_DRAW_PRIMITIVE_TRIANGLES;
    struct dm_m4_draw_info info = {.first_task = draw->first, .task_count = draw->count};
    error = submit_graphics(resources, state, resolve, &session, &draw->raster, primitive, &info, DM_M4_OVERLAY_NONE);
    dm_m4_replay_end();
    return error;
}
