enum { REFLECTED_IMAGE_STORE = 1u << 2 };

static VkResult check_compute_queue(const struct dm_m4_device *device)
{
    PFN_vkGetPhysicalDeviceQueueFamilyProperties get_families = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
        device->get_instance_proc(device->instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (!get_families) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t count = 0;
    get_families(device->physical, &count, NULL);
    if (device->queue_family >= count) return VK_ERROR_INITIALIZATION_FAILED;
    VkQueueFamilyProperties local[16];
    const uint32_t capacity = device->queue_family + 1;
    VkQueueFamilyProperties *families = local;
    if (capacity > sizeof local / sizeof *local) {
        if (capacity > SIZE_MAX / sizeof *families) return VK_ERROR_OUT_OF_HOST_MEMORY;
        families = malloc((size_t)capacity * sizeof *families);
        if (!families) return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    count = capacity;
    get_families(device->physical, &count, families);
    const int supported = device->queue_family < count &&
                          (families[device->queue_family].queueFlags & VK_QUEUE_COMPUTE_BIT);
    if (families != local) free(families);
    return supported ? VK_SUCCESS : VK_ERROR_FEATURE_NOT_PRESENT;
}

static int overlap(const void *a, size_t a_size, const void *b, size_t b_size)
{
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return a_size && b_size && (x <= y ? y - x < a_size : x - y < b_size);
}

static VkResult prepare_storage_image(struct dm_m4_draw *d, const struct dm_m4_shader_binding *recipe,
                                      const struct dm_m4_draw_storage_image *src, VkDeviceSize *used,
                                      struct draw_binding *out, const char **step)
{
    if ((recipe->image.type != 3u && recipe->image.type != 4u) || recipe->image.row_bias ||
        recipe->image.tbx >= 0 || recipe->image.separate)
        return failure(step, "compute requires a plain 2D or 3D storage image", VK_ERROR_FEATURE_NOT_PRESENT);
    const int writable = (recipe->image.flags & REFLECTED_IMAGE_STORE) != 0;
    if (!src || (writable && (!src->writeback || src->writeback != src->pixels.data)))
        return failure(step, "unresolved compute storage image or writeback", VK_ERROR_INITIALIZATION_FAILED);
    const struct dm_m4_draw_pixels *p = &src->pixels;
    const uint32_t bytes = texel_bytes(src->format);
    if (!bytes || (recipe->image.format != VK_FORMAT_UNDEFINED && recipe->image.format != src->format))
        return failure(step, "unsupported compute storage image format or reflection mismatch", VK_ERROR_FORMAT_NOT_SUPPORTED);
    if (recipe->image.format == VK_FORMAT_UNDEFINED &&
        !(writable ? d->features.shaderStorageImageWriteWithoutFormat : d->features.shaderStorageImageReadWithoutFormat))
        return failure(step, "untyped storage image without its Vulkan feature", VK_ERROR_FEATURE_NOT_PRESENT);
    if ((recipe->image.type == 4u) != (src->depth != 0))
        return failure(step, "compute storage view dimension disagrees with reflection", VK_ERROR_INITIALIZATION_FAILED);
    const uint32_t limit = src->depth ? d->limits.maxImageDimension3D : d->limits.maxImageDimension2D;
    if (src->width > limit || src->height > limit || src->depth > limit)
        return failure(step, "compute storage image exceeds device dimensions", VK_ERROR_FEATURE_NOT_PRESENT);
    struct draw_storage_image *image = NULL;
    for (uint32_t i = 0; i < d->storage_image_count; ++i) {
        struct draw_storage_image *candidate = &d->storage_images[i];
        const struct dm_m4_draw_storage_image *prior = candidate->source;
        if (prior->pixels.data == p->data && prior->pixels.size == p->size && prior->pixels.row_pitch == p->row_pitch &&
            prior->format == src->format && prior->width == src->width && prior->height == src->height &&
            prior->depth == src->depth) {
            image = candidate;
            break;
        }
        if ((writable || candidate->writeback) && overlap(prior->pixels.data, prior->pixels.size, p->data, p->size))
            return failure(step, "overlapping compute storage image planes", VK_ERROR_FEATURE_NOT_PRESENT);
    }
    if (!image) {
        void *images = d->storage_images;
        VkResult r = grow_array(&images, sizeof *d->storage_images, d->storage_image_count + 1u,
                                &d->storage_image_capacity, 1);
        if (r != VK_SUCCESS) return failure(step, "grow staged storage image cache", r);
        d->storage_images = images;
        image = &d->storage_images[d->storage_image_count];
        if (!plan_planes(p, 1, 0, src->width, src->height, src->depth ? src->depth : 1,
                         (struct draw_block){1, 1, bytes}, used, &image->transfer))
            return failure(step, "invalid linear compute storage image rows", VK_ERROR_INITIALIZATION_FAILED);
        r = ensure_image_shape(d, &image->image, src->format, src->width, src->height, src->depth, 1, 0, VK_IMAGE_ASPECT_COLOR_BIT,
                         (VkComponentMapping){0}, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                         VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT, step);
        if (r != VK_SUCCESS) return r;
        image->source = src;
        image->writeback = NULL;
        ++d->storage_image_count;
    }
    if (recipe->image.atomic_word && !(image->image.features & VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT))
        return failure(step, "storage format lacks Vulkan image atomics", VK_ERROR_FORMAT_NOT_SUPPORTED);
    if (writable) image->writeback = src->writeback;
    out->image = (VkDescriptorImageInfo){VK_NULL_HANDLE, image->image.view, VK_IMAGE_LAYOUT_GENERAL};
    return VK_SUCCESS;
}

static VkResult prepare_separate_sampler(struct dm_m4_draw *d, const VkSamplerCreateInfo *src,
                                         struct draw_binding *out, const char **step)
{
    VkResult r = check_sampler(d, src, step);
    if (r != VK_SUCCESS) return r;
    for (uint32_t i = 0; i < d->sampler_count; ++i) {
        if (sampler_equal(&d->samplers[i].state, src)) {
            out->image.sampler = d->samplers[i].handle;
            return VK_SUCCESS;
        }
    }
    void *samplers = d->samplers;
    r = grow_array(&samplers, sizeof *d->samplers, d->sampler_count + 1u, &d->sampler_capacity, 1);
    if (r != VK_SUCCESS) return failure(step, "grow staged sampler cache", r);
    d->samplers = samplers;
    struct draw_sampler *sampler = &d->samplers[d->sampler_count];
    r = ensure_sampler(d, sampler, src, step);
    if (r != VK_SUCCESS) return r;
    ++d->sampler_count;
    out->image.sampler = sampler->handle;
    return VK_SUCCESS;
}

static VkResult plan_texel_buffer(struct dm_m4_draw *d, const struct dm_m4_shader_binding *recipe,
                                  const struct dm_m4_draw_binding *src, struct draw_binding *out,
                                  const char **step)
{
    const int storage = recipe->descriptor_type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    const VkFormat format = src->texel_format == VK_FORMAT_UNDEFINED ? recipe->image.format : src->texel_format;
    const uint32_t bytes = texel_bytes(format);
    if (recipe->image.type != 2u || recipe->image.row_bias || !bytes ||
        (recipe->image.format != VK_FORMAT_UNDEFINED && recipe->image.format != format))
        return failure(step, "unsupported compute texel buffer type or format", VK_ERROR_FORMAT_NOT_SUPPORTED);
    if (!src->buffer.size || src->buffer.size % bytes || src->buffer.size / bytes > d->limits.maxTexelBufferElements)
        return failure(step, "invalid compute texel buffer range", VK_ERROR_FEATURE_NOT_PRESENT);
    if (storage && recipe->image.format == VK_FORMAT_UNDEFINED &&
        !((recipe->image.flags & REFLECTED_IMAGE_STORE) ? d->features.shaderStorageImageWriteWithoutFormat :
                                                        d->features.shaderStorageImageReadWithoutFormat))
        return failure(step, "untyped storage texel buffer without its Vulkan feature", VK_ERROR_FEATURE_NOT_PRESENT);
    VkFormatProperties properties;
    d->GetPhysicalDeviceFormatProperties(d->device->physical, format, &properties);
    const VkFormatFeatureFlags required = storage ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT :
                                                    VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
    if (!(properties.bufferFeatures & required))
        return failure(step, "compute texel buffer format unsupported on device", VK_ERROR_FORMAT_NOT_SUPPORTED);
    // A view of the imported buffer must support both of its texel usages.
    const VkFormatFeatureFlags both = VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT;
    out->import_texel = (properties.bufferFeatures & both) == both;
    out->data = src->buffer.data;
    out->size = src->buffer.size;
    out->texel_format = format;
    out->storage_texel = storage;
    if (storage && (recipe->image.flags & REFLECTED_IMAGE_STORE)) {
        if (!src->writeback || src->writeback != out->data)
            return failure(step, "missing writable compute texel buffer span", VK_ERROR_INITIALIZATION_FAILED);
        out->writeback = src->writeback;
    }
    return VK_SUCCESS;
}

static VkResult ensure_texel_views(struct dm_m4_draw *d, uint32_t count, uint32_t binding_count, const char **step)
{
    if (!count) return VK_SUCCESS;
    void *views = d->texel_views;
    VkResult r = grow_array(&views, sizeof *d->texel_views, count, &d->texel_view_capacity, 1);
    if (r != VK_SUCCESS) return failure(step, "grow staged texel view cache", r);
    d->texel_views = views;
    const VkDevice vk = d->device->device;
    uint32_t slot = 0;
    for (uint32_t i = 0; i < binding_count; ++i) {
        struct draw_binding *binding = &d->bindings[i];
        if (binding->texel_format == VK_FORMAT_UNDEFINED) continue;
        struct draw_texel_view *view = &d->texel_views[slot++];
        const VkBuffer backing = binding->buffer.buffer ? binding->buffer.buffer : d->texel_buffers[binding->storage_texel];
        if (!view->handle || view->buffer != backing || view->format != binding->texel_format ||
            view->offset != binding->buffer.offset || view->range != binding->buffer.range) {
            const VkBufferViewCreateInfo info = {
                .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = backing, .format = binding->texel_format,
                .offset = binding->buffer.offset, .range = binding->buffer.range,
            };
            VkBufferView next;
            r = d->CreateBufferView(vk, &info, NULL, &next);
            if (r != VK_SUCCESS) return failure(step, "create compute texel view", r);
            if (view->handle) d->DestroyBufferView(vk, view->handle, NULL);
            *view = (struct draw_texel_view){next, backing, info.format, info.offset, info.range};
        }
        binding->texel_view = view->handle;
    }
    return VK_SUCCESS;
}

static void upload_storage_image(struct dm_m4_draw *d, const struct draw_storage_image *s)
{
    const struct draw_image *image = &s->image;
    VkBufferImageCopy copy = image_copy_region(&s->transfer, image->width, image->height, 0, image->aspects);
    copy.imageExtent.depth = image->depth ? image->depth : 1;
    upload_plane(d, &s->transfer, s->source->pixels.data);
    image_barrier(d, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    d->CmdCopyBufferToImage(d->cmd, d->buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    image_barrier(d, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT | (s->writeback ? VK_ACCESS_SHADER_WRITE_BIT : 0));
}

static void readback_storage_image(struct dm_m4_draw *d, const struct draw_storage_image *s)
{
    const struct draw_image *image = &s->image;
    VkBufferImageCopy copy = image_copy_region(&s->transfer, image->width, image->height, 0, image->aspects);
    copy.imageExtent.depth = image->depth ? image->depth : 1;
    // Reuse the upload range only after both the initial copy and compute have completed.
    image_barrier(d, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    d->CmdCopyImageToBuffer(d->cmd, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->buffer, 1, &copy);
}
