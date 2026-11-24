static int supports_subgroup(const struct dm_m4_draw *d, VkSubgroupFeatureFlagBits op, VkShaderStageFlags stages)
{
    return (d->subgroup.supportedStages & stages) == stages && (d->subgroup.supportedOperations & op) &&
           (!(stages & (VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT)) ||
            d->subgroup.subgroupSize == 32) &&
           (op != VK_SUBGROUP_FEATURE_QUAD_BIT ||
            !(stages & ~(VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)) ||
            d->subgroup.quadOperationsInAllStages);
}

static VkResult check_shader(const struct dm_m4_draw *d, const struct dm_m4_shader *shader, const char **step)
{
    struct dm_m4_draw_requirements need = {0};
    for (uint32_t i = 0; i < shader->capability_count; ++i) {
        VkResult r = enable_capability(&need, shader->stage, shader->capabilities[i], step);
        if (r != VK_SUCCESS) return r;
    }
#define CHECK_CAP(number, field) if (need.core.field && !d->features.field) \
    return failure(step, "draw shader requires enabled " #field, VK_ERROR_FEATURE_NOT_PRESENT);
    DM_M4_DRAW_CORE_CAPS(CHECK_CAP)
#undef CHECK_CAP
#define CHECK_CAP(number, field) if (need.vk12.field && !d->shader_float16) \
    return failure(step, "shader requires enabled " #field, VK_ERROR_FEATURE_NOT_PRESENT);
    DM_M4_DRAW_VK12_CAPS(CHECK_CAP)
#undef CHECK_CAP
    if (need.vk12.shaderSampledImageArrayNonUniformIndexing && !d->sampled_nonuniform)
        return failure(step, "draw shader requires enabled partially bound nonuniform descriptors",
                       VK_ERROR_FEATURE_NOT_PRESENT);
    if (need.core.fragmentStoresAndAtomics && !d->features.fragmentStoresAndAtomics)
        return failure(step, "draw shader requires enabled fragment fault writes", VK_ERROR_FEATURE_NOT_PRESENT);
    if (need.core.vertexPipelineStoresAndAtomics && !d->features.vertexPipelineStoresAndAtomics)
        return failure(step, "draw shader requires enabled pre-raster fault writes", VK_ERROR_FEATURE_NOT_PRESENT);
    if (need.vk11.shaderDrawParameters && !d->shader_draw_parameters)
        return failure(step, "draw shader requires shaderDrawParameters", VK_ERROR_FEATURE_NOT_PRESENT);
    if (!!shader->vprs_viewport != !!need.vk12.shaderOutputViewportIndex)
        return failure(step, "draw shader has an unsupported native viewport index", VK_ERROR_INVALID_SHADER_NV);
    if (need.vk12.shaderOutputViewportIndex && !d->shader_output_viewport_index)
        return failure(step, "draw shader requires shaderOutputViewportIndex", VK_ERROR_FEATURE_NOT_PRESENT);
    if (need.interlock.fragmentShaderPixelInterlock && !d->pixel_interlock)
        return failure(step, "draw fragment shader requires pixel interlock", VK_ERROR_FEATURE_NOT_PRESENT);
    if (need.rate.shadingRateImage && !d->CmdBindShadingRateImageNV)
        return failure(step, "draw fragment shader requires NV shading rate", VK_ERROR_FEATURE_NOT_PRESENT);
    for (size_t i = 0; i < sizeof subgroup_features / sizeof *subgroup_features; ++i) {
        if (need.subgroup_stages[i] && !supports_subgroup(d, subgroup_features[i], need.subgroup_stages[i]))
            return failure(step, "draw shader subgroup capability unsupported in its stage", VK_ERROR_FEATURE_NOT_PRESENT);
    }
    return VK_SUCCESS;
}

static uint32_t float_word(float value)
{
    uint32_t word;
    memcpy(&word, &value, sizeof word);
    return word;
}

static int dual_src(VkBlendFactor factor)
{
    return factor >= VK_BLEND_FACTOR_SRC1_COLOR && factor <= VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
}

static int stage_ok(const struct dm_m4_draw_stage *stage, VkShaderStageFlagBits expected, uint32_t set)
{
    const struct dm_m4_shader *s = stage->shader;
    return s && s->module && s->set_layout && s->stage == expected && s->set == set &&
           (!s->binding_count || (s->bindings && stage->bindings)) && (!s->capability_count || s->capabilities);
}

static int mesh_workgroup_ok(const struct dm_m4_shader *s, const uint32_t size[3],
                             uint32_t invocations, uint32_t shared)
{
    for (size_t i = 0; i < 3; ++i) {
        if (!s->local[i] || s->local[i] > size[i] || s->local[i] > invocations) return 0;
        invocations /= s->local[i];
    }
    return s->shared <= shared;
}

static VkShaderStageFlagBits mesh_launch_stage(const struct dm_m4_draw_info *info)
{
    return info->ts.shader ? VK_SHADER_STAGE_TASK_BIT_EXT : VK_SHADER_STAGE_MESH_BIT_EXT;
}

static uint32_t mesh_group_limit(const struct dm_m4_draw *d, const struct dm_m4_draw_info *info)
{
    const VkPhysicalDeviceMeshShaderPropertiesEXT *m = &d->mesh_limits;
    const uint32_t x = info->ts.shader ? m->maxTaskWorkGroupCount[0] : m->maxMeshWorkGroupCount[0];
    const uint32_t total = info->ts.shader ? m->maxTaskWorkGroupTotalCount : m->maxMeshWorkGroupTotalCount;
    return x < total ? x : total;
}

static int viewport_ok(const VkViewport *v, const VkPhysicalDeviceLimits *l)
{
    const float min = l->viewportBoundsRange[0], max = l->viewportBoundsRange[1];
    const float end_y = v->y + v->height;
    return v->width > 0.0f && v->width <= (float)l->maxViewportDimensions[0] &&
           fabsf(v->height) > 0.0f && fabsf(v->height) <= (float)l->maxViewportDimensions[1] &&
           v->x >= min && v->x + v->width <= max && v->y >= min && v->y <= max && end_y >= min && end_y <= max &&
           v->minDepth >= 0.0f && v->minDepth <= 1.0f && v->maxDepth >= 0.0f && v->maxDepth <= 1.0f;
}

static const VkShadingRatePaletteEntryNV *rate_palette_at(const struct dm_m4_rate_image *rate, uint32_t viewport)
{
    return viewport ? rate->additional_palettes[viewport - 1] : rate->palette;
}

// Serialize only fixed-function fields read by the Vulkan pipeline builder.
// Avoid pointer and padding identity: the caller may rebuild snapshots on its stack.
static VkResult pipeline_key(struct dm_m4_draw *d, const struct dm_m4_draw_info *info,
                             VkPrimitiveTopology topology, VkBool32 restart,
                             struct pipeline_key *key, const char **step)
{
    const struct dm_m4_pipeline_info *p = &info->pipeline;
    const VkPipelineVertexInputStateCreateInfo *v = p->vertex_input;
    const VkPipelineRasterizationStateCreateInfo *r = p->rasterization;
    const VkPipelineMultisampleStateCreateInfo *m = p->multisample;
    const VkPipelineDepthStencilStateCreateInfo *z = p->depth_stencil;
    const VkPipelineColorBlendStateCreateInfo *b = p->blend;
    const struct dm_m4_shader *ms = info->ms.shader, *ts = info->ts.shader, *ps = info->ps.shader;
    const struct dm_m4_shader *primary = ms ? ms : info->vs.shader;
    if (ms) {
        const VkPhysicalDeviceMeshShaderPropertiesEXT *lim = &d->mesh_limits;
        if (!d->mesh_shader || (ts && !d->task_shader))
            return failure(step, "VK_EXT_mesh_shader draw stages not enabled", VK_ERROR_FEATURE_NOT_PRESENT);
        if (v || (!!ts != !!ms->mesh_has_task))
            return failure(step, "mesh pipeline has vertex input or a mismatched task stage", VK_ERROR_INVALID_SHADER_NV);
        if (!mesh_workgroup_ok(ms, lim->maxMeshWorkGroupSize, lim->maxMeshWorkGroupInvocations, lim->maxMeshSharedMemorySize) ||
            ms->mesh_vertices > lim->maxMeshOutputVertices || ms->mesh_primitives > lim->maxMeshOutputPrimitives ||
            (ts && !mesh_workgroup_ok(ts, lim->maxTaskWorkGroupSize, lim->maxTaskWorkGroupInvocations, lim->maxTaskSharedMemorySize)))
            return failure(step, "mesh shader workgroup, shared or output size exceeds device limit", VK_ERROR_FEATURE_NOT_PRESENT);
    } else {
        switch (topology) {
        case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
        case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
        case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
            break;
        default:
            return failure(step, "unsupported indexed primitive topology", VK_ERROR_FEATURE_NOT_PRESENT);
        }
    }
    if (!p->viewport_count || p->viewport_count > DM_M4_RATE_MAX_PALETTES ||
        p->viewport_count > d->limits.maxViewports || !viewport_ok(&info->viewport, &d->limits))
        return failure(step, "draw viewport count or bounds exceed device limits", VK_ERROR_FEATURE_NOT_PRESENT);
    if (p->viewport_count > 1) {
        if (ms || !primary->vprs_viewport || !info->rate_image || !info->rate_image->additional_palettes)
            return failure(step, "draw requires a lowered vertex VPRS output and one palette per viewport",
                           VK_ERROR_FEATURE_NOT_PRESENT);
        if (ps && ps->reads_viewport_index)
            return failure(step, "VPRS palette selection cannot change a guest fragment shader's viewport index",
                           VK_ERROR_FEATURE_NOT_PRESENT);
        if (!d->features.multiViewport || !d->shader_output_viewport_index)
            return failure(step, "draw requires multiViewport and shaderOutputViewportIndex",
                           VK_ERROR_FEATURE_NOT_PRESENT);
    }
    if (p->negative_one_to_one && !(d->draw_features & DM_M4_DRAW_NEGATIVE_ONE_TO_ONE))
        return failure(step, "negative-one-to-one depth clip not enabled", VK_ERROR_FEATURE_NOT_PRESENT);
    if (!ms && (!v || v->sType != VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO || v->flags ||
        v->vertexBindingDescriptionCount > DM_M4_NVN_VERTEX_SLOTS ||
        v->vertexAttributeDescriptionCount > DM_M4_NVN_VERTEX_SLOTS ||
        v->vertexBindingDescriptionCount > d->limits.maxVertexInputBindings ||
        v->vertexAttributeDescriptionCount > d->limits.maxVertexInputAttributes ||
        (v->vertexBindingDescriptionCount && !v->pVertexBindingDescriptions) ||
        (v->vertexAttributeDescriptionCount && !v->pVertexAttributeDescriptions)))
        return failure(step, "unsupported vertex layout or device input count", VK_ERROR_FEATURE_NOT_PRESENT);
    if (!r || r->sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO || r->flags ||
        (unsigned)r->polygonMode > VK_POLYGON_MODE_POINT || (r->cullMode & ~(VkCullModeFlags)VK_CULL_MODE_FRONT_AND_BACK) ||
        (unsigned)r->frontFace > VK_FRONT_FACE_CLOCKWISE || !(r->lineWidth > 0.0f) || !isfinite(r->lineWidth))
        return failure(step, "unsupported draw rasterization state", VK_ERROR_FEATURE_NOT_PRESENT);
    const VkPipelineRasterizationConservativeStateCreateInfoEXT *conservative = r->pNext;
    if (conservative && (!(d->draw_features & DM_M4_DRAW_CONSERVATIVE_RASTER) ||
        conservative->sType != VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT ||
        conservative->pNext || conservative->flags || conservative->extraPrimitiveOverestimationSize != 0 ||
        conservative->conservativeRasterizationMode != VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT))
        return failure(step, "unsupported conservative rasterization state", VK_ERROR_FEATURE_NOT_PRESENT);
    if (r->depthClampEnable && !d->features.depthClamp)
        return failure(step, "draw depth clamping not enabled", VK_ERROR_FEATURE_NOT_PRESENT);
    if (r->depthBiasEnable && r->depthBiasClamp != 0.0f && !d->features.depthBiasClamp)
        return failure(step, "draw depth-bias clamping not enabled", VK_ERROR_FEATURE_NOT_PRESENT);
    if (r->polygonMode != VK_POLYGON_MODE_FILL && !d->features.fillModeNonSolid)
        return failure(step, "draw non-solid polygon mode not enabled", VK_ERROR_FEATURE_NOT_PRESENT);
    if (r->lineWidth != 1.0f && (!d->features.wideLines || r->lineWidth < d->limits.lineWidthRange[0] ||
                               r->lineWidth > d->limits.lineWidthRange[1]))
        return failure(step, "draw wide lines not enabled or outside device range", VK_ERROR_FEATURE_NOT_PRESENT);
    if (info->point_size != 0.0f) {
        if (!primary->point_size_fallback)
            return failure(step, "primary shader has no NVN fixed point size output", VK_ERROR_FEATURE_NOT_PRESENT);
        if (!d->features.largePoints || !isfinite(info->point_size) ||
            info->point_size < d->limits.pointSizeRange[0] || info->point_size > d->limits.pointSizeRange[1])
            return failure(step, "draw large points not enabled or outside device range", VK_ERROR_FEATURE_NOT_PRESENT);
    }
    if (!m || m->sType != VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO || m->pNext || m->flags ||
        m->rasterizationSamples != VK_SAMPLE_COUNT_1_BIT ||
        (m->sampleShadingEnable && (!d->features.sampleRateShading || !(m->minSampleShading >= 0.0f) ||
                                    m->minSampleShading > 1.0f)) || (m->alphaToOneEnable && !d->features.alphaToOne))
        return failure(step, "unsupported draw multisampling state", VK_ERROR_FEATURE_NOT_PRESENT);
    const int depth = info->depth.format != VK_FORMAT_UNDEFINED && info->depth.format != VK_FORMAT_S8_UINT;
    if ((info->depth.format != VK_FORMAT_UNDEFINED && !z) ||
        (z && (z->sType != VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO || z->pNext || z->flags ||
               (!depth && (z->depthTestEnable || z->depthWriteEnable || z->depthBoundsTestEnable)) ||
               (z->depthBoundsTestEnable && (!d->features.depthBounds || !(z->minDepthBounds >= 0.0f) ||
                                            !(z->maxDepthBounds <= 1.0f) || z->minDepthBounds > z->maxDepthBounds)) ||
               (z->stencilTestEnable && !has_stencil(info->depth.format)))))
        return failure(step, "unsupported depth/stencil state or missing plane", VK_ERROR_FEATURE_NOT_PRESENT);
    if (info->color_count ? (!b || b->sType != VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO || b->pNext ||
                 b->flags || b->attachmentCount != info->color_count || !b->pAttachments ||
                 (info->color_count > 1 && !d->features.independentBlend) ||
                 (b->logicOpEnable && !d->features.logicOp)) : b != NULL)
        return failure(step, "unsupported draw blending state or independent blend not enabled", VK_ERROR_FEATURE_NOT_PRESENT);
    for (uint32_t i = 0; i < info->color_count; ++i) {
        const VkFormat format = info->colors[i].format;
        const VkPipelineColorBlendAttachmentState *a = &b->pAttachments[i];
        if (a->blendEnable && !d->features.dualSrcBlend &&
            (dual_src(a->srcColorBlendFactor) || dual_src(a->dstColorBlendFactor) ||
             dual_src(a->srcAlphaBlendFactor) || dual_src(a->dstAlphaBlendFactor)))
            return failure(step, "draw dual-source blending not enabled", VK_ERROR_FEATURE_NOT_PRESENT);
        // NVN ignores fragment results for unbound targets. Keep their
        // locations, rather than moving a later result into an earlier slot.
        if (!ps || format == VK_FORMAT_UNDEFINED || !(ps->frag_write_mask & (15u << (4 * i)))) continue;
        const uint32_t bit = 1u << i;
        const enum dm_m4_color_type output = (ps->frag_uint_mask & bit) ? DM_M4_COLOR_UINT :
                                            (ps->frag_sint_mask & bit) ? DM_M4_COLOR_SINT : DM_M4_COLOR_FLOAT;
        if (output != dm_m4_attachment_color_type(format))
            return failure(step, "fragment output numeric type does not match its render target", VK_ERROR_FORMAT_NOT_SUPPORTED);
    }

    key->primary_set = primary->set_layout;
    key->ps_set = ps ? ps->set_layout : (ts ? d->empty_set : VK_NULL_HANDLE);
    key->task_set = ts ? ts->set_layout : VK_NULL_HANDLE;
    key->count = 0;
#define KEY(value) key->words[key->count++] = (uint32_t)(value)
#define KEY_F(value) KEY(float_word(value))
    KEY(topology); KEY(restart); KEY(p->negative_one_to_one);
    KEY(info->depth.format); KEY(info->color_count);
    for (uint32_t i = 0; i < info->color_count; ++i) KEY(info->colors[i].format);
    KEY(ms != NULL);
    KEY(p->viewport_count); KEY(info->rate_image != NULL);
    if (info->rate_image) {
        static_assert(DM_M4_RATE_PALETTE_SIZE == 16 &&
                      VK_SHADING_RATE_PALETTE_ENTRY_1_INVOCATION_PER_4X4_PIXELS_NV < 16, "packed palette key");
        for (uint32_t viewport = 0; viewport < p->viewport_count; ++viewport) {
            const VkShadingRatePaletteEntryNV *palette = rate_palette_at(info->rate_image, viewport);
            uint64_t packed = 0;
            for (uint32_t i = 0; i < DM_M4_RATE_PALETTE_SIZE; ++i) {
                const unsigned entry = (unsigned)palette[i];
                if (entry > VK_SHADING_RATE_PALETTE_ENTRY_1_INVOCATION_PER_4X4_PIXELS_NV)
                    return failure(step, "invalid NV shading-rate palette entry", VK_ERROR_INITIALIZATION_FAILED);
                packed |= (uint64_t)entry << (4 * i);
            }
            KEY(packed); KEY(packed >> 32);
        }
    }
    if (v) {
        KEY(v->vertexBindingDescriptionCount);
        uint32_t valid_bindings = 0, instance_bindings = 0;
        for (uint32_t i = 0; i < v->vertexBindingDescriptionCount; ++i) {
            const VkVertexInputBindingDescription *x = &v->pVertexBindingDescriptions[i];
            if (x->binding >= DM_M4_NVN_VERTEX_SLOTS || x->binding >= d->limits.maxVertexInputBindings ||
                x->stride > d->limits.maxVertexInputBindingStride || (unsigned)x->inputRate > VK_VERTEX_INPUT_RATE_INSTANCE ||
                (valid_bindings & (UINT32_C(1) << x->binding)))
                return failure(step, "invalid vertex binding index, stride or rate", VK_ERROR_FEATURE_NOT_PRESENT);
            valid_bindings |= UINT32_C(1) << x->binding;
            if (x->inputRate == VK_VERTEX_INPUT_RATE_INSTANCE) instance_bindings |= UINT32_C(1) << x->binding;
            KEY(x->binding); KEY(x->stride); KEY(x->inputRate);
        }
        KEY(v->vertexAttributeDescriptionCount);
        uint32_t valid_locations = 0;
        for (uint32_t i = 0; i < v->vertexAttributeDescriptionCount; ++i) {
            const VkVertexInputAttributeDescription *x = &v->pVertexAttributeDescriptions[i];
            if (x->location >= DM_M4_NVN_VERTEX_SLOTS || x->location >= d->limits.maxVertexInputAttributes ||
                x->binding >= DM_M4_NVN_VERTEX_SLOTS || !(valid_bindings & (UINT32_C(1) << x->binding)) ||
                x->offset > d->limits.maxVertexInputAttributeOffset || (valid_locations & (UINT32_C(1) << x->location)))
                return failure(step, "invalid vertex attribute location, binding or offset", VK_ERROR_FEATURE_NOT_PRESENT);
            valid_locations |= UINT32_C(1) << x->location;
            KEY(x->location); KEY(x->binding); KEY(x->format); KEY(x->offset);
        }
        const VkPipelineVertexInputDivisorStateCreateInfoEXT *divisor = v->pNext;
        if (divisor && (!(d->draw_features & DM_M4_DRAW_VERTEX_DIVISOR) ||
                        divisor->sType != VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT || divisor->pNext ||
                        divisor->vertexBindingDivisorCount > v->vertexBindingDescriptionCount || !divisor->pVertexBindingDivisors))
            return failure(step, "vertex attribute divisor feature not enabled or invalid state", VK_ERROR_FEATURE_NOT_PRESENT);
        uint32_t divisors = divisor ? divisor->vertexBindingDivisorCount : 0;
        KEY(divisors);
        for (uint32_t i = 0; i < divisors; ++i) {
            const VkVertexInputBindingDivisorDescriptionEXT *x = &divisor->pVertexBindingDivisors[i];
            if (x->binding >= DM_M4_NVN_VERTEX_SLOTS || !(instance_bindings & (UINT32_C(1) << x->binding)) ||
                !x->divisor || x->divisor > d->max_vertex_divisor)
                return failure(step, "vertex attribute divisor exceeds device limits", VK_ERROR_FEATURE_NOT_PRESENT);
            if (x->divisor != 1u && info->first_instance)
                return failure(step, "nonzero first instance with an instance divisor unsupported", VK_ERROR_FEATURE_NOT_PRESENT);
            instance_bindings &= ~(UINT32_C(1) << x->binding);
            KEY(x->binding); KEY(x->divisor);
        }
    }
    KEY(r->depthClampEnable); KEY(r->rasterizerDiscardEnable); KEY(r->polygonMode); KEY(r->cullMode); KEY(r->frontFace);
    KEY(conservative != NULL);
    KEY(r->depthBiasEnable); KEY_F(r->depthBiasConstantFactor); KEY_F(r->depthBiasClamp);
    KEY_F(r->depthBiasSlopeFactor); KEY_F(r->lineWidth);
    KEY_F(info->point_size);
    KEY(m->sampleShadingEnable); KEY_F(m->minSampleShading); KEY(m->pSampleMask ? (*m->pSampleMask & 1u) : 1u);
    KEY(m->alphaToCoverageEnable); KEY(m->alphaToOneEnable);
    KEY(z != NULL);
    if (z) {
        KEY(z->depthTestEnable); KEY(z->depthWriteEnable); KEY(z->depthCompareOp);
        KEY(z->depthBoundsTestEnable); KEY_F(z->minDepthBounds); KEY_F(z->maxDepthBounds); KEY(z->stencilTestEnable);
        const VkStencilOpState faces[] = {z->front, z->back};
        for (size_t i = 0; i < sizeof faces / sizeof *faces; ++i) {
            const VkStencilOpState *s = &faces[i];
            KEY(s->failOp); KEY(s->passOp); KEY(s->depthFailOp); KEY(s->compareOp);
            KEY(s->compareMask); KEY(s->writeMask); KEY(s->reference);
        }
    }
    if (b) {
        KEY(b->logicOpEnable); KEY(b->logicOp);
        for (uint32_t i = 0; i < info->color_count; ++i) {
            const VkPipelineColorBlendAttachmentState *a = &b->pAttachments[i];
            KEY(a->blendEnable); KEY(a->srcColorBlendFactor); KEY(a->dstColorBlendFactor); KEY(a->colorBlendOp);
            KEY(a->srcAlphaBlendFactor); KEY(a->dstAlphaBlendFactor); KEY(a->alphaBlendOp); KEY(a->colorWriteMask);
        }
        for (size_t i = 0; i < sizeof b->blendConstants / sizeof *b->blendConstants; ++i) KEY_F(b->blendConstants[i]);
    }
#undef KEY
#undef KEY_F
    return VK_SUCCESS;
}

static void destroy_pipeline(struct dm_m4_draw *d, struct draw_pipeline *p)
{
    dm_m4_pipeline_destroy(d->device, p->pipeline);
    if (p->layout) d->DestroyPipelineLayout(d->device->device, p->layout, NULL);
    p->pipeline = VK_NULL_HANDLE;
    p->layout = VK_NULL_HANDLE;
}

static VkResult get_pipeline(struct dm_m4_draw *d, const struct dm_m4_draw_info *info,
                             VkPrimitiveTopology topology, VkBool32 restart, const struct sampled_types types[3],
                             struct draw_pipeline **out, const char **step)
{
    struct pipeline_key key;
    VkResult r = pipeline_key(d, info, topology, restart, &key, step);
    if (r != VK_SUCCESS) return r;
    const struct dm_m4_shader *primary = info->ms.shader ? info->ms.shader : info->vs.shader;
    r = sampled_module(d, primary, types[0], &key.primary, step);
    if (r != VK_SUCCESS) return r;
    r = sampled_module(d, info->ps.shader, types[1], &key.ps, step);
    if (r != VK_SUCCESS) return r;
    r = sampled_module(d, info->ts.shader, types[2], &key.task, step);
    if (r != VK_SUCCESS) return r;
    struct draw_pipeline *candidate = &d->pipelines[0];
    for (size_t i = 0; i < PIPELINE_CACHE_SIZE; ++i) {
        struct draw_pipeline *entry = &d->pipelines[i];
        if (entry->pipeline && entry->key.primary == key.primary && entry->key.ps == key.ps && entry->key.task == key.task &&
            entry->key.primary_set == key.primary_set && entry->key.ps_set == key.ps_set &&
            entry->key.task_set == key.task_set && entry->key.count == key.count &&
            !memcmp(entry->key.words, key.words, key.count * sizeof *key.words)) {
            entry->used = ++d->serial;
            *out = entry;
            return VK_SUCCESS;
        }
        if (!entry->pipeline || (candidate->pipeline && candidate->used > entry->used)) candidate = entry;
    }
    r = check_shader(d, primary, step);
    if (r != VK_SUCCESS) return r;
    if (info->ps.shader && (r = check_shader(d, info->ps.shader, step)) != VK_SUCCESS) return r;
    if (info->ts.shader && (r = check_shader(d, info->ts.shader, step)) != VK_SUCCESS) return r;
    const VkPipelineVertexInputStateCreateInfo *v = info->pipeline.vertex_input;
    for (uint32_t i = 0; v && i < v->vertexAttributeDescriptionCount; ++i) {
        VkFormatProperties properties;
        d->GetPhysicalDeviceFormatProperties(d->device->physical, v->pVertexAttributeDescriptions[i].format, &properties);
        if (!(properties.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))
            return failure(step, "draw vertex attribute format unavailable for vertex buffers", VK_ERROR_FORMAT_NOT_SUPPORTED);
    }
    destroy_pipeline(d, candidate);
    const VkDescriptorSetLayout sets[] = {key.primary_set, key.ps_set, key.task_set};
    const VkPushConstantRange first_task = {.stageFlags = mesh_launch_stage(info), .size = sizeof(uint32_t)};
    const VkPipelineLayoutCreateInfo layout_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = key.task_set ? 3u : key.ps_set ? 2u : 1u, .pSetLayouts = sets,
        .pushConstantRangeCount = info->ms.shader ? 1u : 0u, .pPushConstantRanges = &first_task,
    };
    r = d->CreatePipelineLayout(d->device->device, &layout_info, NULL, &candidate->layout);
    if (r != VK_SUCCESS) return failure(step, "create draw pipeline layout", r);
    struct dm_m4_pipeline_info pipeline = info->pipeline;
    pipeline.layout = candidate->layout;
    pipeline.topology = topology;
    pipeline.primitive_restart_enable = restart;
    pipeline.vs = info->ms.shader ? VK_NULL_HANDLE : key.primary;
    pipeline.ms = info->ms.shader ? key.primary : VK_NULL_HANDLE;
    pipeline.ps = key.ps;
    pipeline.ts = key.task;
    pipeline.vs_entry = pipeline.ps_entry = pipeline.ms_entry = pipeline.ts_entry = DM_M4_SHADER_ENTRY;
    const struct draw_specialization {
        float point_size;
        VkBool32 vprs_viewport;
    } data = {info->point_size, VK_TRUE};
    VkSpecializationMapEntry entries[2];
    uint32_t spec_count = 0;
    if (info->point_size != 0.0f)
        entries[spec_count++] = (VkSpecializationMapEntry){
            DM_M4_POINT_SIZE_SPEC_ID, (uint32_t)offsetof(struct draw_specialization, point_size), sizeof data.point_size,
        };
    if (pipeline.viewport_count > 1)
        entries[spec_count++] = (VkSpecializationMapEntry){
            DM_M4_VPRS_VIEWPORT_SPEC_ID, (uint32_t)offsetof(struct draw_specialization, vprs_viewport),
            sizeof data.vprs_viewport,
        };
    const VkSpecializationInfo spec = {spec_count, entries, sizeof data, &data};
    pipeline.primary_specialization = spec_count ? &spec : NULL;
    VkShadingRatePaletteNV rates[DM_M4_RATE_MAX_PALETTES];
    pipeline.shading_rate_palettes = NULL;
    if (info->rate_image) {
        for (uint32_t i = 0; i < pipeline.viewport_count; ++i)
            rates[i] = (VkShadingRatePaletteNV){DM_M4_RATE_PALETTE_SIZE, rate_palette_at(info->rate_image, i)};
        pipeline.shading_rate_palettes = rates;
    }
    VkFormat formats[DM_M4_NVN_COLOR_TARGETS];
    for (uint32_t i = 0; i < info->color_count; ++i) formats[i] = info->colors[i].format;
    pipeline.color_formats = info->color_count ? formats : NULL;
    pipeline.depth_format = info->depth.format == VK_FORMAT_S8_UINT ? VK_FORMAT_UNDEFINED : info->depth.format;
    pipeline.stencil_format = has_stencil(info->depth.format) ? info->depth.format : VK_FORMAT_UNDEFINED;
    r = dm_m4_pipeline_create(d->device, &pipeline, &candidate->pipeline);
    if (r != VK_SUCCESS) return failure(step, "create staged dynamic-rendering graphics pipeline", r);
    memcpy(&candidate->key, &key, offsetof(struct pipeline_key, words) + key.count * sizeof *key.words);
    candidate->used = ++d->serial;
    *out = candidate;
    return VK_SUCCESS;
}
