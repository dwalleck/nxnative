static void destroy_compute_pipeline(struct dm_m4_draw *d, struct compute_pipeline *p)
{
    const VkDevice vk = d->device->device;
    if (p->pipeline) d->DestroyPipeline(vk, p->pipeline, NULL);
    if (p->layout) d->DestroyPipelineLayout(vk, p->layout, NULL);
    *p = (struct compute_pipeline){0};
}

static VkResult get_compute_pipeline(struct dm_m4_draw *d, const struct dm_m4_shader *shader,
                                     struct sampled_types types, struct compute_pipeline **out, const char **step)
{
    VkShaderModule module;
    VkResult r = sampled_module(d, shader, types, &module, step);
    if (r != VK_SUCCESS) return r;
    struct compute_pipeline *oldest = d->compute_pipelines;
    for (size_t i = 0; i < COMPUTE_PIPELINE_CACHE_SIZE; ++i) {
        struct compute_pipeline *p = &d->compute_pipelines[i];
        if (p->shader == module && p->set == shader->set_layout) {
            p->used = ++d->serial;
            *out = p;
            return VK_SUCCESS;
        }
        if (p->used < oldest->used) oldest = p;
    }
    r = check_shader(d, shader, step);
    if (r != VK_SUCCESS) return r;
    struct compute_pipeline next = {.shader = module, .set = shader->set_layout};
    const VkPipelineLayoutCreateInfo layout = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &next.set,
    };
    const VkDevice vk = d->device->device;
    r = d->CreatePipelineLayout(vk, &layout, NULL, &next.layout);
    if (r != VK_SUCCESS) return failure(step, "create compute pipeline layout", r);
    const VkComputePipelineCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = next.layout,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = DM_M4_SHADER_ENTRY,
        },
    };
    r = d->CreateComputePipelines(vk, VK_NULL_HANDLE, 1, &info, NULL, &next.pipeline);
    if (r != VK_SUCCESS) {
        destroy_compute_pipeline(d, &next);
        return failure(step, "create staged compute pipeline", r);
    }
    destroy_compute_pipeline(d, oldest);
    next.used = ++d->serial;
    *oldest = next;
    *out = oldest;
    return VK_SUCCESS;
}

static int compute_descriptor_limits(const VkPhysicalDeviceLimits *l, const uint32_t counts[8])
{
    const uint64_t combined = counts[VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER];
    const uint64_t samplers = combined + counts[VK_DESCRIPTOR_TYPE_SAMPLER];
    const uint64_t sampled = combined + counts[VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE] + counts[VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER];
    const uint64_t storage = (uint64_t)counts[VK_DESCRIPTOR_TYPE_STORAGE_IMAGE] + counts[VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER];
    const uint32_t uniform = counts[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER], buffers = counts[VK_DESCRIPTOR_TYPE_STORAGE_BUFFER];
    return samplers <= l->maxPerStageDescriptorSamplers && samplers <= l->maxDescriptorSetSamplers &&
           sampled <= l->maxPerStageDescriptorSampledImages && sampled <= l->maxDescriptorSetSampledImages &&
           storage <= l->maxPerStageDescriptorStorageImages && storage <= l->maxDescriptorSetStorageImages &&
           uniform <= l->maxPerStageDescriptorUniformBuffers && uniform <= l->maxDescriptorSetUniformBuffers &&
           buffers <= l->maxPerStageDescriptorStorageBuffers && buffers <= l->maxDescriptorSetStorageBuffers &&
           sampled + storage + uniform + buffers <= l->maxPerStageResources;
}

static VkResult check_compute_aliases(const struct dm_m4_draw *d, uint32_t bindings, const char **step)
{
    // A staging image and a buffer (or separate sampled image) do not share
    // device memory. Reject any writable overlap that cannot be represented.
    for (uint32_t i = 0; i < d->storage_image_count; ++i) {
        const struct draw_storage_image *image = &d->storage_images[i];
        const struct dm_m4_draw_pixels *p = &image->source->pixels;
        const size_t size = image->transfer.row + (image->transfer.rows - 1) * image->transfer.pitch;
        for (uint32_t j = 0; j < bindings; ++j) {
            const struct draw_binding *buffer = &d->bindings[j];
            if ((image->writeback || buffer->writeback) && overlap(p->data, size, buffer->data, buffer->size))
                return failure(step, "overlapping compute image and buffer spans", VK_ERROR_FEATURE_NOT_PRESENT);
        }
    }
    for (uint32_t i = 0; i < d->sample_count; ++i) {
        const struct draw_sample *sample = &d->samples[i];
        for (uint32_t k = 0; k < sample->image.levels; ++k) {
            const struct draw_transfer *t = &sample->levels[k];
            const void *data = sample->pixels[k].data;
            const size_t size = t->row + (t->rows - 1) * t->pitch;
            for (uint32_t j = 0; j < d->storage_image_count; ++j) {
                const struct draw_storage_image *image = &d->storage_images[j];
                if (!image->writeback) continue;
                const struct draw_transfer *p = &image->transfer;
                if (overlap(data, size, image->writeback, p->row + (p->rows - 1) * p->pitch))
                    return failure(step, "overlapping compute sampled and writable image planes", VK_ERROR_FEATURE_NOT_PRESENT);
            }
            for (uint32_t j = 0; j < bindings; ++j) {
                const struct draw_binding *buffer = &d->bindings[j];
                if (overlap(data, size, buffer->writeback, buffer->writeback ? buffer->size : 0))
                    return failure(step, "overlapping compute sampled image and writable buffer", VK_ERROR_FEATURE_NOT_PRESENT);
            }
        }
    }
    return VK_SUCCESS;
}

VkResult dm_m4_compute_submit(struct dm_m4_draw *d, const struct dm_m4_compute_info *info, const char **step)
{
    if (step) *step = NULL;
    if (!d || !info) return failure(step, "compute submit arguments", VK_ERROR_INITIALIZATION_FAILED);
    if (!d->compute) return failure(step, "Vulkan replay device has no configured compute shaders", VK_ERROR_FEATURE_NOT_PRESENT);
    if (!stage_ok(&info->cs, VK_SHADER_STAGE_COMPUTE_BIT, 0))
        return failure(step, "compute requires shader set 0", VK_ERROR_INVALID_SHADER_NV);
    const struct dm_m4_shader *shader = info->cs.shader;
    const VkPhysicalDeviceLimits *l = &d->limits;
    uint32_t local_invocations = 1;
    for (size_t i = 0; i < 3; ++i) {
        const uint32_t local = shader->local[i];
        if (info->groups[i] > l->maxComputeWorkGroupCount[i] || !local || local > l->maxComputeWorkGroupSize[i] ||
            local_invocations > l->maxComputeWorkGroupInvocations / local)
            return failure(step, "compute workgroup counts or local sizes exceed device limits", VK_ERROR_FEATURE_NOT_PRESENT);
        local_invocations *= local;
    }
    if (shader->shared > l->maxComputeSharedMemorySize)
        return failure(step, "compute shared memory exceeds device limit", VK_ERROR_FEATURE_NOT_PRESENT);
    VkResult r = reset_recording(d, step);
    if (r != VK_SUCCESS) return r;
    VkDeviceSize used = 0;
    uint32_t counts[8] = {0};
    const uint32_t total = shader->binding_count;
    r = gmem_prepare(d, &info->gmem, shader->gmem, shader->gmem_read_alias, &used, step);
    if (r != VK_SUCCESS) return r;
    struct sampled_types numeric;
    r = prepare_bindings(d, &info->cs, 1, &info->support, &used, counts, total, &numeric, step);
    if (r != VK_SUCCESS) return r;
    if (!compute_descriptor_limits(l, counts))
        return failure(step, "compute descriptors exceed device limits", VK_ERROR_FEATURE_NOT_PRESENT);
    r = check_compute_aliases(d, total, step);
    if (r != VK_SUCCESS) return r;
    if (shader->gmem) {
        for (uint32_t i = 0; i < info->gmem.view_count; ++i) {
            const struct dm_m4_gmem_view *view = &info->gmem.views[i];
            if (!view->writable || !gmem_host_range(d, view->data, view->size)) continue;
            if (!d->gmem_read_alias)
                return failure(step, "writable compute view aliases raw global memory", VK_ERROR_FEATURE_NOT_PRESENT);
            // Only the checked raw accesses are guarded. A normal descriptor
            // overlapping these original bytes still cannot share the image.
            for (uint32_t j = 0; j < total; ++j) {
                const struct draw_binding *buffer = &d->bindings[j];
                if (buffer->data && overlap(view->data, view->size, buffer->data, buffer->size))
                    return failure(step, "overlapping compute view and buffer spans", VK_ERROR_FEATURE_NOT_PRESENT);
            }
            d->gmem_read_protected = 1;
        }
        // Direct callers can omit view metadata for their linear images.
        for (uint32_t i = 0; i < d->storage_image_count; ++i) {
            const struct draw_storage_image *image = &d->storage_images[i];
            const struct dm_m4_draw_pixels *p = &image->source->pixels;
            if (!image->writeback) continue;
            const size_t size = d->gmem_read_alias ? gmem_plane_size(&image->transfer) : p->size;
            if (d->gmem_read_alias && size > UINTPTR_MAX - (uintptr_t)p->data)
                return failure(step, "invalid writable compute image span", VK_ERROR_INITIALIZATION_FAILED);
            if (d->gmem_read_protected || !gmem_host_range(d, p->data, size)) continue;
            if (!d->gmem_read_alias)
                return failure(step, "writable compute image aliases raw global memory", VK_ERROR_FEATURE_NOT_PRESENT);
            d->gmem_read_protected = 1;
        }
    }
    VkBufferUsageFlags usage = 0;
    r = gmem_backing(d, &info->gmem, total, &used, &usage, step);
    if (r != VK_SUCCESS) return r;
    r = plan_buffer_bindings(d, total, 1, &used, &usage, step);
    if (r != VK_SUCCESS) return r;
    struct compute_pipeline *pipeline;
    r = get_compute_pipeline(d, shader, numeric, &pipeline, step);
    if (r != VK_SUCCESS) return r;
    const uint32_t uniform_texels = counts[VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER];
    const uint32_t storage_texels = counts[VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER];
    r = ensure_buffer(d, used, usage, step);
    if (r != VK_SUCCESS) return r;
    r = ensure_texel_views(d, uniform_texels + storage_texels, total, step);
    if (r != VK_SUCCESS) return r;
    VkDescriptorSet set;
    r = prepare_descriptor_sets(d, &info->cs, 1, counts, &set, step);
    if (r != VK_SUCCESS) return r;
    uint32_t gmem_entries;
    r = gmem_upload(d, &info->gmem, shader->gmem, &gmem_entries, step);
    if (r != VK_SUCCESS) return r;
    r = begin_recording(d, step);
    if (r != VK_SUCCESS) return r;

    const int buffer_write = upload_buffers(d, total) || d->gmem_writes;
    r = upload_samples(d, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, step);
    if (r != VK_SUCCESS) return r;
    for (uint32_t i = 0; i < d->storage_image_count; ++i) upload_storage_image(d, &d->storage_images[i]);
    d->CmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);
    d->CmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout, 0, 1, &set, 0, NULL);
    d->CmdDispatch(d->cmd, info->groups[0], info->groups[1], info->groups[2]);

    // The map fault is a compute shader write, even with no writable buffers.
    const int shader_write = buffer_write || d->gmem_read_alias;
    VkPipelineStageFlags write_stages = shader_write ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT : 0;
    VkAccessFlags write_access = shader_write ? VK_ACCESS_SHADER_WRITE_BIT : 0;
    for (uint32_t i = 0; i < d->storage_image_count; ++i) {
        const struct draw_storage_image *image = &d->storage_images[i];
        if (!image->writeback) continue;
        readback_storage_image(d, image);
        write_stages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
        write_access |= VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    r = finish_recording(d, used, write_stages, write_access, step);
    if (r != VK_SUCCESS) return r;
    // Host barrier, fence and any non-coherent invalidation are complete.
    // Do not publish staged buffer or image results on a raw-read fault.
    r = gmem_read_alias_fault(d, step);
    if (r != VK_SUCCESS) return r;
    r = bindless_faults(d, total, step);
    if (r != VK_SUCCESS) return r;
    r = gmem_download(d, &info->gmem, gmem_entries, step);
    if (r != VK_SUCCESS) return r;
    if (buffer_write) download_buffers(d, total);
    for (uint32_t i = 0; i < d->storage_image_count; ++i) {
        const struct draw_storage_image *image = &d->storage_images[i];
        if (image->writeback) download_plane(d, &image->transfer, image->writeback);
    }
    complete_samples(d);
    return VK_SUCCESS;
}
