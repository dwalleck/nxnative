static int reserve(VkDeviceSize *used, VkDeviceSize alignment, VkDeviceSize count, VkDeviceSize *offset)
{
    VkDeviceSize rem = *used % alignment;
    VkDeviceSize padding = rem ? alignment - rem : 0;
    if (padding > SIZE_MAX - *used || count > SIZE_MAX - *used - padding) return 0;
    *offset = *used + padding;
    *used = *offset + count;
    return 1;
}

static VkResult grow_array(void **array, size_t element, uint32_t count, uint32_t *capacity, int zero)
{
    if (count <= *capacity) return VK_SUCCESS;
    uint32_t next = *capacity ? *capacity : 8u;
    while (next < count) {
        if (next > UINT32_MAX / 2u) { next = count; break; }
        next *= 2u;
    }
    if (next > SIZE_MAX / element) return VK_ERROR_OUT_OF_HOST_MEMORY;
    void *p = realloc(*array, (size_t)next * element);
    if (!p) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (zero) memset((uint8_t *)p + (size_t)*capacity * element, 0, (size_t)(next - *capacity) * element);
    *array = p;
    *capacity = next;
    return VK_SUCCESS;
}

static uint32_t memory_type(const struct dm_m4_draw *d, uint32_t allowed, VkMemoryPropertyFlags required,
                            VkMemoryPropertyFlags preferred)
{
    uint32_t candidate = UINT32_MAX;
    for (uint32_t i = 0; i < d->memory.memoryTypeCount; ++i) {
        VkMemoryPropertyFlags flags = d->memory.memoryTypes[i].propertyFlags;
        if (!(allowed & (UINT32_C(1) << i)) || (flags & required) != required) continue;
        if ((flags & preferred) == preferred) return i;
        if (candidate == UINT32_MAX) candidate = i;
    }
    return candidate;
}

static void destroy_buffer(struct dm_m4_draw *d)
{
    const VkDevice vk = d->device->device;
    for (uint32_t i = 0; i < d->texel_view_capacity; ++i) {
        struct draw_texel_view *view = &d->texel_views[i];
        if (view->handle) d->DestroyBufferView(vk, view->handle, NULL);
        *view = (struct draw_texel_view){0};
    }
    if (d->mapped) d->UnmapMemory(vk, d->buffer_memory);
    if (d->buffer) d->DestroyBuffer(vk, d->buffer, NULL);
    for (size_t i = 0; i < sizeof d->texel_buffers / sizeof *d->texel_buffers; ++i) {
        if (d->texel_buffers[i]) d->DestroyBuffer(vk, d->texel_buffers[i], NULL);
        d->texel_buffers[i] = VK_NULL_HANDLE;
    }
    if (d->buffer_memory) d->FreeMemory(vk, d->buffer_memory, NULL);
    d->mapped = NULL;
    d->buffer = VK_NULL_HANDLE;
    d->buffer_memory = VK_NULL_HANDLE;
    d->buffer_capacity = 0;
    d->buffer_address = 0;
}

static VkResult ensure_buffer(struct dm_m4_draw *d, VkDeviceSize size, VkBufferUsageFlags extra_usage, const char **step)
{
    static const VkBufferUsageFlagBits view_usage[] = {
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT,
    };
    VkBufferUsageFlags existing = 0;
    for (size_t i = 0; i < sizeof view_usage / sizeof *view_usage; ++i)
        if (d->texel_buffers[i]) existing |= view_usage[i];
    if (d->buffer_address) existing |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (size <= d->buffer_capacity && !(extra_usage & ~existing)) return VK_SUCCESS;
    extra_usage |= existing;
    const VkBool32 addressed = (extra_usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
    if (addressed && !d->GetBufferDeviceAddress)
        return failure(step, "Vulkan buffer device address not enabled", VK_ERROR_FEATURE_NOT_PRESENT);
    if (size < d->buffer_capacity) size = d->buffer_capacity;
    destroy_buffer(d);
    const VkDevice vk = d->device->device;
    // Grow to the next power of two but do not risk doubling a very large draw.
    VkDeviceSize capacity = 65536;
    while (capacity < size && capacity < (UINT64_C(1) << 26)) capacity *= 2;
    if (capacity < size) capacity = size;
    VkBufferCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = capacity,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 (extra_usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT),
    };
    VkResult r = d->CreateBuffer(vk, &ci, NULL, &d->buffer);
    if (r != VK_SUCCESS) return failure(step, "create draw staging buffer", r);
    VkMemoryRequirements mem;
    d->GetBufferMemoryRequirements(vk, d->buffer, &mem);
    // A buffer view's format must support every texel usage on its VkBuffer.
    // Give each usage its own handle, without splitting or copying mapped bytes.
    for (size_t i = 0; i < sizeof view_usage / sizeof *view_usage; ++i) {
        if (!(extra_usage & view_usage[i])) continue;
        ci.usage = view_usage[i];
        r = d->CreateBuffer(vk, &ci, NULL, &d->texel_buffers[i]);
        if (r != VK_SUCCESS) return failure(step, "create staged texel buffer alias", r);
        VkMemoryRequirements alias;
        d->GetBufferMemoryRequirements(vk, d->texel_buffers[i], &alias);
        if (mem.size < alias.size) mem.size = alias.size;
        mem.memoryTypeBits &= alias.memoryTypeBits;
    }
    uint32_t type = memory_type(d, mem.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type == UINT32_MAX) return failure(step, "find host-visible draw staging memory", VK_ERROR_FEATURE_NOT_PRESENT);
    const VkMemoryAllocateFlagsInfo address_flags = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
    };
    const VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = addressed ? &address_flags : NULL,
        .allocationSize = mem.size, .memoryTypeIndex = type,
    };
    r = d->AllocateMemory(vk, &ai, NULL, &d->buffer_memory);
    if (r != VK_SUCCESS) return failure(step, "allocate draw staging memory", r);
    r = d->BindBufferMemory(vk, d->buffer, d->buffer_memory, 0);
    if (r != VK_SUCCESS) return failure(step, "bind draw staging memory", r);
    for (size_t i = 0; i < sizeof view_usage / sizeof *view_usage; ++i) {
        if (!d->texel_buffers[i]) continue;
        r = d->BindBufferMemory(vk, d->texel_buffers[i], d->buffer_memory, 0);
        if (r != VK_SUCCESS) return failure(step, "bind staged texel buffer alias", r);
    }
    void *mapped = NULL;
    r = d->MapMemory(vk, d->buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped);
    if (r != VK_SUCCESS) return failure(step, "map draw staging memory", r);
    d->mapped = mapped;
    d->coherent = (d->memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    if (addressed) {
        const VkBufferDeviceAddressInfo address_info = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = d->buffer,
        };
        d->buffer_address = d->GetBufferDeviceAddress(vk, &address_info);
        if (!d->buffer_address || d->buffer_address % 16 || capacity > UINT64_MAX - d->buffer_address) {
            destroy_buffer(d);
            return failure(step, "invalid Vulkan global memory address or extent", VK_ERROR_INITIALIZATION_FAILED);
        }
    }
    d->buffer_capacity = capacity;
    return VK_SUCCESS;
}

static int components_equal(VkComponentMapping a, VkComponentMapping b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

static void destroy_image(struct dm_m4_draw *d, struct draw_image *image)
{
    const VkDevice vk = d->device->device;
    if (image->view) d->DestroyImageView(vk, image->view, NULL);
    if (image->image) d->DestroyImage(vk, image->image, NULL);
    if (image->memory) d->FreeMemory(vk, image->memory, NULL);
    *image = (struct draw_image){0};
}

static VkResult create_draw_image(struct dm_m4_draw *d, struct draw_image *out, const VkImageCreateInfo *ci,
                                  const char **step)
{
    destroy_image(d, out);
    const VkDevice vk = d->device->device;
    VkResult r = d->CreateImage(vk, ci, NULL, &out->image);
    if (r != VK_SUCCESS) return failure(step, "create staged image", r);
    VkMemoryRequirements mem;
    d->GetImageMemoryRequirements(vk, out->image, &mem);
    uint32_t type = memory_type(d, mem.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) return failure(step, "find staged image memory", VK_ERROR_FEATURE_NOT_PRESENT);
    const VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mem.size, .memoryTypeIndex = type,
    };
    r = d->AllocateMemory(vk, &ai, NULL, &out->memory);
    if (r != VK_SUCCESS) return failure(step, "allocate staged image memory", r);
    out->allocation_size = mem.size;
    r = d->BindImageMemory(vk, out->image, out->memory, 0);
    return r == VK_SUCCESS ? r : failure(step, "bind staged image memory", r);
}

static uint32_t image_layer_count(uint32_t array_layers, VkBool32 cube)
{
    return cube ? 6u : array_layers ? array_layers : 1u;
}

static VkResult ensure_image_view(struct dm_m4_draw *d, struct draw_image *out, VkFormat format, uint32_t width,
                                  uint32_t height, uint32_t depth, uint32_t levels, uint32_t array_layers, VkBool32 cube,
                                  VkImageAspectFlags aspects, VkComponentMapping components, VkImageUsageFlags usage,
                                  VkFormatFeatureFlags features, const char **step)
{
    if (out->view && out->format == format && out->width == width && out->height == height &&
        out->depth == depth && out->levels == levels && out->array_layers == array_layers && out->aspects == aspects &&
        out->cube == cube && components_equal(out->components, components)) {
        if ((out->features & features) == features) return VK_SUCCESS;
        return failure(step, "cached draw image format lacks required optimal-tiling features", VK_ERROR_FORMAT_NOT_SUPPORTED);
    }
    VkFormatProperties properties;
    d->GetPhysicalDeviceFormatProperties(d->device->physical, format, &properties);
    if ((properties.optimalTilingFeatures & features) != features)
        return failure(step, "draw image format lacks required optimal-tiling features", VK_ERROR_FORMAT_NOT_SUPPORTED);
    const VkImageType image_type = depth ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    const VkImageCreateFlags flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    const uint32_t layers = image_layer_count(array_layers, cube);
    if (array_layers || depth || cube) {
        VkImageFormatProperties image_props;
        VkResult r = d->GetPhysicalDeviceImageFormatProperties(d->device->physical, format, image_type,
                                                               VK_IMAGE_TILING_OPTIMAL, usage, flags, &image_props);
        if (r != VK_SUCCESS) return failure(step, "query sampled image shape format limits", r);
        if (width > image_props.maxExtent.width || height > image_props.maxExtent.height ||
            depth > image_props.maxExtent.depth || levels > image_props.maxMipLevels || layers > image_props.maxArrayLayers)
            return failure(step, "sampled image shape exceeds format limits", VK_ERROR_FORMAT_NOT_SUPPORTED);
    }
    const VkDevice vk = d->device->device;
    const VkImageCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = image_type, .format = format,
        .flags = flags,
        .extent = {width, height, depth ? depth : 1u},
        .mipLevels = levels, .arrayLayers = layers, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
    };
    VkResult r = create_draw_image(d, out, &ci, step);
    if (r != VK_SUCCESS) return r;
    const VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = out->image,
        .viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE :
            depth ? VK_IMAGE_VIEW_TYPE_3D : array_layers ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
        .format = format, .components = components, .subresourceRange = {aspects, 0, levels, 0, layers},
    };
    r = d->CreateImageView(vk, &vi, NULL, &out->view);
    if (r != VK_SUCCESS) return failure(step, "create staged draw image view", r);
    out->format = format;
    out->width = width;
    out->height = height;
    out->depth = depth;
    out->features = properties.optimalTilingFeatures;
    out->levels = levels;
    out->array_layers = array_layers;
    out->cube = cube;
    out->aspects = aspects;
    out->components = components;
    return VK_SUCCESS;
}

static VkResult ensure_image_shape(struct dm_m4_draw *d, struct draw_image *out, VkFormat format, uint32_t width,
                                   uint32_t height, uint32_t depth, uint32_t levels, uint32_t array_layers,
                                   VkImageAspectFlags aspects, VkComponentMapping components, VkImageUsageFlags usage,
                                   VkFormatFeatureFlags features, const char **step)
{
    return ensure_image_view(d, out, format, width, height, depth, levels, array_layers, VK_FALSE,
                             aspects, components, usage, features, step);
}

static VkResult ensure_image(struct dm_m4_draw *d, struct draw_image *out, VkFormat format, uint32_t width,
                             uint32_t height, uint32_t levels, VkImageAspectFlags aspects,
                             VkComponentMapping components, VkImageUsageFlags usage,
                             VkFormatFeatureFlags features, const char **step)
{
    return ensure_image_shape(d, out, format, width, height, 0, levels, 0, aspects, components, usage, features, step);
}

struct draw_block { uint32_t width, height, bytes; };

static uint32_t depth_bytes(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_D16_UNORM: return 2;
    case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return 4;
    default: return 0;
    }
}

static int has_stencil(VkFormat format)
{
    return format == VK_FORMAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

static uint32_t texel_bytes(VkFormat f)
{
    if (f == VK_FORMAT_R4G4_UNORM_PACK8 || (f >= VK_FORMAT_R8_UNORM && f <= VK_FORMAT_R8_SRGB)) return 1;
    if ((f >= VK_FORMAT_R4G4B4A4_UNORM_PACK16 && f <= VK_FORMAT_A1R5G5B5_UNORM_PACK16) ||
        (f >= VK_FORMAT_R8G8_UNORM && f <= VK_FORMAT_R8G8_SRGB) ||
        (f >= VK_FORMAT_R16_UNORM && f <= VK_FORMAT_R16_SFLOAT)) return 2;
    if (f >= VK_FORMAT_R8G8B8_UNORM && f <= VK_FORMAT_B8G8R8_SRGB) return 3;
    if ((f >= VK_FORMAT_R8G8B8A8_UNORM && f <= VK_FORMAT_A2B10G10R10_SINT_PACK32) ||
        (f >= VK_FORMAT_R16G16_UNORM && f <= VK_FORMAT_R16G16_SFLOAT) ||
        (f >= VK_FORMAT_R32_UINT && f <= VK_FORMAT_R32_SFLOAT) ||
        f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32) return 4;
    if (f >= VK_FORMAT_R16G16B16_UNORM && f <= VK_FORMAT_R16G16B16_SFLOAT) return 6;
    if ((f >= VK_FORMAT_R16G16B16A16_UNORM && f <= VK_FORMAT_R16G16B16A16_SFLOAT) ||
        (f >= VK_FORMAT_R32G32_UINT && f <= VK_FORMAT_R32G32_SFLOAT)) return 8;
    if (f >= VK_FORMAT_R32G32B32_UINT && f <= VK_FORMAT_R32G32B32_SFLOAT) return 12;
    if (f >= VK_FORMAT_R32G32B32A32_UINT && f <= VK_FORMAT_R32G32B32A32_SFLOAT) return 16;
    return 0;
}

static VkResult sampled_block(const struct dm_m4_draw *d, VkFormat format, struct draw_block *out,
                              VkImageAspectFlags *aspects, const char **step)
{
    *aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    *out = (struct draw_block){1, 1, texel_bytes(format)};
    if (out->bytes) return VK_SUCCESS;
    if (!has_stencil(format) && (out->bytes = depth_bytes(format)) != 0) {
        *aspects = VK_IMAGE_ASPECT_DEPTH_BIT;
        return VK_SUCCESS;
    }
    if (format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && format <= VK_FORMAT_BC7_SRGB_BLOCK) {
        if (!d->features.textureCompressionBC)
            return failure(step, "BC texture compression not enabled", VK_ERROR_FEATURE_NOT_PRESENT);
        uint32_t bytes = format <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK ||
                         format == VK_FORMAT_BC4_UNORM_BLOCK || format == VK_FORMAT_BC4_SNORM_BLOCK ? 8 : 16;
        *out = (struct draw_block){4, 4, bytes};
    } else if (format >= VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && format <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK) {
        if (!d->features.textureCompressionETC2)
            return failure(step, "ETC2/EAC texture compression not enabled", VK_ERROR_FEATURE_NOT_PRESENT);
        uint32_t bytes = ((format >= VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK &&
                           format <= VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK) ||
                          format >= VK_FORMAT_EAC_R11G11_UNORM_BLOCK) ? 16 : 8;
        *out = (struct draw_block){4, 4, bytes};
    } else if (format >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && format <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) {
        if (!d->features.textureCompressionASTC_LDR)
            return failure(step, "ASTC LDR texture compression not enabled", VK_ERROR_FEATURE_NOT_PRESENT);
        static const uint8_t extent[][2] = {
            {4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8},
            {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12},
        };
        unsigned i = (unsigned)(format - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) / 2u;
        *out = (struct draw_block){extent[i][0], extent[i][1], 16};
    } else {
        return failure(step, "unsupported linear sampled image format", VK_ERROR_FORMAT_NOT_SUPPORTED);
    }
    return VK_SUCCESS;
}

static int plan_planes(const struct dm_m4_draw_pixels *pixels, uint32_t layers, uint32_t layer_stride,
                       uint32_t width, uint32_t height, uint32_t depth, struct draw_block block,
                       VkDeviceSize *used, struct draw_transfer *t)
{
    if (!width || !height) return 0;
    size_t columns = 1u + (width - 1u) / block.width;
    size_t rows = 1u + (height - 1u) / block.height;
    if (rows > SIZE_MAX / depth) return 0;
    rows *= depth; // a volume's z slices are consecutive sets of block rows
    if (columns > SIZE_MAX / block.bytes) return 0;
    size_t row = columns * block.bytes;
    if (rows > SIZE_MAX / row) return 0;
    const size_t layer_bytes = rows * row;
    if (layers > SIZE_MAX / layer_bytes) return 0;
    if (pixels) {
        for (uint32_t layer = 0; layer < layers; ++layer) {
            const struct dm_m4_draw_pixels *p = &pixels[(size_t)layer * layer_stride];
            const size_t pitch = p->row_pitch ? p->row_pitch : row;
            if (!p->data || pitch < row || p->size < row || rows - 1u > (p->size - row) / pitch) return 0;
        }
    }
    VkDeviceSize alignment = (block.bytes % 4u == 0) ? block.bytes : block.bytes * 4u;
    VkDeviceSize offset;
    if (!reserve(used, alignment, layers * layer_bytes, &offset)) return 0;
    *t = (struct draw_transfer){offset, row, pixels && pixels->row_pitch ? pixels->row_pitch : row, rows};
    return 1;
}

static int plan_plane(const void *data, size_t size, size_t pitch, uint32_t width, uint32_t height,
                      struct draw_block block, VkDeviceSize *used, struct draw_transfer *t)
{
    const struct dm_m4_draw_pixels pixels = {data, size, pitch};
    return plan_planes(&pixels, 1, 0, width, height, 1, block, used, t);
}

static void upload_plane(struct dm_m4_draw *d, const struct draw_transfer *t, const void *src)
{
    uint8_t *to = d->mapped + t->offset;
    const uint8_t *from = src;
    if (t->row == t->pitch) {
        memcpy(to, from, t->rows * t->row);
    } else {
        for (size_t y = 0; y < t->rows; ++y) memcpy(to + y * t->row, from + y * t->pitch, t->row);
    }
}

static void download_plane(struct dm_m4_draw *d, const struct draw_transfer *t, void *dst)
{
    uint8_t *to = dst;
    const uint8_t *from = d->mapped + t->offset;
    if (t->row == t->pitch) {
        memcpy(to, from, t->rows * t->row);
    } else {
        for (size_t y = 0; y < t->rows; ++y) memcpy(to + y * t->pitch, from + y * t->row, t->row);
    }
}

static void image_barrier(struct dm_m4_draw *d, const struct draw_image *image,
                          VkImageLayout old_layout, VkImageLayout new_layout,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                          VkAccessFlags src_access, VkAccessFlags dst_access)
{
    const VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .image = image->image,
        .oldLayout = old_layout, .newLayout = new_layout,
        .srcAccessMask = src_access, .dstAccessMask = dst_access,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .subresourceRange = {image->aspects, 0, image->levels, 0, image_layer_count(image->array_layers, image->cube)},
    };
    d->CmdPipelineBarrier(d->cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static VkBufferImageCopy image_copy_region(const struct draw_transfer *t, uint32_t width, uint32_t height,
                                            uint32_t level, VkImageAspectFlags aspect)
{
    return (VkBufferImageCopy){
        .bufferOffset = t->offset, .imageSubresource = {aspect, level, 0, 1}, .imageExtent = {width, height, 1},
    };
}

static int sampler_equal(const VkSamplerCreateInfo *a, const VkSamplerCreateInfo *b)
{
    return a->magFilter == b->magFilter && a->minFilter == b->minFilter && a->mipmapMode == b->mipmapMode &&
           a->addressModeU == b->addressModeU && a->addressModeV == b->addressModeV &&
           a->addressModeW == b->addressModeW && a->mipLodBias == b->mipLodBias &&
           a->minLod == b->minLod && a->maxLod == b->maxLod && a->borderColor == b->borderColor &&
           a->compareEnable == b->compareEnable && (!a->compareEnable || a->compareOp == b->compareOp) &&
           a->anisotropyEnable == b->anisotropyEnable &&
           (!a->anisotropyEnable || a->maxAnisotropy == b->maxAnisotropy);
}

static VkResult check_sampler(const struct dm_m4_draw *d, const VkSamplerCreateInfo *ci, const char **step)
{
    if (!ci || ci->sType != VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO || ci->pNext || ci->flags ||
        (ci->compareEnable && (unsigned)ci->compareOp > VK_COMPARE_OP_ALWAYS) ||
        ci->unnormalizedCoordinates || (unsigned)ci->magFilter > VK_FILTER_LINEAR ||
        (unsigned)ci->minFilter > VK_FILTER_LINEAR || (unsigned)ci->mipmapMode > VK_SAMPLER_MIPMAP_MODE_LINEAR ||
        (unsigned)ci->addressModeU > VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
        (unsigned)ci->addressModeV > VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
        (unsigned)ci->addressModeW > VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
        (unsigned)ci->borderColor > VK_BORDER_COLOR_INT_OPAQUE_WHITE ||
        !isfinite(ci->minLod) || !isfinite(ci->maxLod) || ci->minLod > ci->maxLod ||
        !isfinite(ci->mipLodBias) || fabsf(ci->mipLodBias) > d->limits.maxSamplerLodBias)
        return failure(step, "unsupported sampler state", VK_ERROR_FEATURE_NOT_PRESENT);
    if (ci->anisotropyEnable && (!d->features.samplerAnisotropy || !isfinite(ci->maxAnisotropy) ||
                                 ci->maxAnisotropy < 1.0f || ci->maxAnisotropy > d->limits.maxSamplerAnisotropy))
        return failure(step, "sampled image anisotropy not enabled or exceeds device limit", VK_ERROR_FEATURE_NOT_PRESENT);
    return VK_SUCCESS;
}

static void destroy_sampler(struct dm_m4_draw *d, struct draw_sampler *sampler)
{
    if (sampler->handle) d->DestroySampler(d->device->device, sampler->handle, NULL);
    sampler->handle = VK_NULL_HANDLE;
}

static VkResult ensure_sampler(struct dm_m4_draw *d, struct draw_sampler *sampler,
                               const VkSamplerCreateInfo *ci, const char **step)
{
    if (sampler->handle && sampler_equal(&sampler->state, ci)) return VK_SUCCESS;
    VkSampler next;
    VkResult r = d->CreateSampler(d->device->device, ci, NULL, &next);
    if (r != VK_SUCCESS) return failure(step, "create staged sampler", r);
    destroy_sampler(d, sampler);
    *sampler = (struct draw_sampler){.handle = next, .state = *ci};
    return VK_SUCCESS;
}

static uint32_t mip_count(uint32_t width, uint32_t height, uint32_t depth)
{
    uint32_t n = 1;
    uint32_t size = width > height ? width : height;
    if (depth > size) size = depth;
    for (; size > 1; size >>= 1) ++n;
    return n;
}

static uint32_t mip_size(uint32_t value, uint32_t level)
{
    value >>= level;
    return value ? value : 1;
}

static int same_sample_backing(const struct draw_sample *sample, const struct dm_m4_draw_sample_upload *upload)
{
    if (!sample->resident || !sample->snapshot || sample->snapshot_size != upload->layer_bytes * upload->layers)
        return 0;
    if (upload->layer_stride == upload->layer_bytes)
        return m4_bytes_equal(sample->snapshot, upload->encoded, sample->snapshot_size);
    for (uint32_t layer = 0; layer < upload->layers; ++layer)
        if (!m4_bytes_equal(sample->snapshot + layer * upload->layer_bytes,
                            upload->encoded + layer * upload->layer_stride, upload->layer_bytes)) return 0;
    return 1;
}

enum { SAMPLE_CACHE_BYTES = 512u * 1024u * 1024u, SAMPLE_INACTIVE_SLOTS = 4096 };

static void remember_sample_backing(struct dm_m4_draw *d, struct draw_sample *sample,
                                    const struct dm_m4_draw_sample_upload *upload)
{
    size_t size = upload ? upload->layer_bytes * upload->layers : 0;
    // Active images may exceed the cache budget. Such images still draw;
    // they need fresh staging instead of a retained encoded snapshot.
    if (sample->image.allocation_size > SAMPLE_CACHE_BYTES ||
        size > SAMPLE_CACHE_BYTES - (d->sample_snapshot_bytes - sample->snapshot_size)) size = 0;
    if (size != sample->snapshot_size) {
        d->sample_snapshot_bytes -= sample->snapshot_size;
        free(sample->snapshot);
        sample->snapshot = size ? malloc(size) : NULL;
        sample->snapshot_size = sample->snapshot ? size : 0;
        d->sample_snapshot_bytes += sample->snapshot_size;
    }
    if (!upload || !sample->snapshot) return;
    if (upload->layer_stride == upload->layer_bytes) {
        memcpy(sample->snapshot, upload->encoded, size);
    } else {
        for (uint32_t layer = 0; layer < upload->layers; ++layer)
            memcpy(sample->snapshot + layer * upload->layer_bytes,
                   upload->encoded + layer * upload->layer_stride, upload->layer_bytes);
    }
}

static int sample_layout_equal(const struct dm_m4_draw_sample_layout *a, const struct dm_m4_draw_sample_layout *b)
{
    const nvn_texdesc *x = &a->desc, *y = &b->desc;
    return a->offset == b->offset && a->pitch == b->pitch && a->base_level == b->base_level &&
           a->base_layer == b->base_layer && a->format == b->format &&
           x->target == y->target && x->format == y->format && x->flags == y->flags && x->samples == y->samples &&
           x->width == y->width && x->height == y->height && x->depth == y->depth && x->levels == y->levels &&
           x->stride == y->stride && x->pkg_layout == y->pkg_layout && x->gob_order == y->gob_order;
}

static int sample_key_equal(const struct draw_sample_key *a, const struct draw_sample_key *b)
{
    return a->address == b->address && a->generation == b->generation &&
           (!a->generation || (a->id == b->id && a->kind == b->kind &&
            a->layer_bytes == b->layer_bytes && a->layer_stride == b->layer_stride &&
            sample_layout_equal(&a->layout, &b->layout)));
}

static uint32_t sample_hash(const struct draw_sample_key *key)
{
    uint64_t hash = key->address ^ key->generation ^ ((uint64_t)key->id << 32) ^ key->kind;
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    return (uint32_t)(hash ^ (hash >> 33));
}

static int sample_view_equal(const struct draw_image *image, const struct dm_m4_draw_sampled_image *src)
{
    return image->format == src->format && image->width == src->width && image->height == src->height &&
           image->depth == src->depth && image->levels == src->level_count && image->array_layers == src->array_layers &&
           image->cube == src->cube && components_equal(image->components, src->components);
}

// Indices are stable for active samples. Swaps and deletes update only the
// affected buckets; resizing is the only time we walk the retained table.
static void index_sample(struct dm_m4_draw *d, uint32_t index)
{
    struct draw_sample *sample = &d->samples[index];
    const uint32_t mask = d->sample_lookup_capacity - 1u;
    uint32_t bucket = sample->hash & mask;
    while (d->sample_lookup[bucket]) bucket = (bucket + 1u) & mask;
    d->sample_lookup[bucket] = index + 1u;
    sample->bucket = bucket;
}

static void unindex_sample(struct dm_m4_draw *d, struct draw_sample *sample)
{
    if (!sample->key.address) return;
    const uint32_t mask = d->sample_lookup_capacity - 1u;
    uint32_t hole = sample->bucket;
    for (uint32_t i = (hole + 1u) & mask; d->sample_lookup[i]; i = (i + 1u) & mask) {
        struct draw_sample *next = &d->samples[d->sample_lookup[i] - 1u];
        const uint32_t start = next->hash & mask;
        if (((hole - start) & mask) < ((i - start) & mask)) {
            d->sample_lookup[hole] = d->sample_lookup[i];
            next->bucket = hole;
            hole = i;
        }
    }
    d->sample_lookup[hole] = 0;
    sample->key.address = 0;
}

static void swap_sample_slots(struct dm_m4_draw *d, uint32_t a, uint32_t b)
{
    if (a == b) return;
    struct draw_sample *left = &d->samples[a], *right = &d->samples[b];
    const struct draw_sample temp = *left;
    *left = *right;
    *right = temp;
    if (left->key.address) d->sample_lookup[left->bucket] = a + 1u;
    if (right->key.address) d->sample_lookup[right->bucket] = b + 1u;
}

static uint32_t find_sample(struct dm_m4_draw *d, const struct draw_sample_key *key, uint32_t hash,
                            const struct dm_m4_draw_sampled_image *src)
{
    const uint32_t mask = d->sample_lookup_capacity - 1u;
    for (uint32_t i = hash & mask; d->sample_lookup[i]; i = (i + 1u) & mask) {
        const uint32_t index = d->sample_lookup[i] - 1u;
        const struct draw_sample *s = &d->samples[index];
        if (s->hash == hash && sample_key_equal(&s->key, key) && sample_view_equal(&s->image, src))
            return index;
    }
    return UINT32_MAX;
}

static VkResult prepare_sample_lookup(struct dm_m4_draw *d, uint32_t need)
{
    if (need > UINT32_MAX / 4u) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if (d->sample_lookup_capacity >= need * 4u) return VK_SUCCESS;
    uint32_t capacity = d->sample_lookup_capacity ? d->sample_lookup_capacity : 64u;
    while (capacity < need * 4u) {
        if (capacity > UINT32_MAX / 2u) return VK_ERROR_OUT_OF_HOST_MEMORY;
        capacity *= 2u;
    }
    uint32_t *lookup = realloc(d->sample_lookup, (size_t)capacity * sizeof *lookup);
    if (!lookup) return VK_ERROR_OUT_OF_HOST_MEMORY;
    d->sample_lookup = lookup;
    d->sample_lookup_capacity = capacity;
    memset(lookup, 0, (size_t)capacity * sizeof *lookup);
    for (uint32_t i = 0; i < d->sample_slots; ++i)
        if (d->samples[i].key.address) index_sample(d, i);
    return VK_SUCCESS;
}

static void trim_inactive_samples(struct dm_m4_draw *d)
{
    while (d->sample_slots > d->sample_count &&
           (d->sample_image_bytes > SAMPLE_CACHE_BYTES ||
            d->sample_slots - d->sample_count > SAMPLE_INACTIVE_SLOTS)) {
        struct draw_sample *sample = &d->samples[--d->sample_slots];
        unindex_sample(d, sample);
        d->sample_image_bytes -= sample->image.allocation_size;
        d->sample_snapshot_bytes -= sample->snapshot_size;
        destroy_image(d, &sample->image);
        free(sample->snapshot);
        *sample = (struct draw_sample){0};
    }
}

static VkResult prepare_sample(struct dm_m4_draw *d, const struct dm_m4_draw_sampled_image *src,
                               const VkSamplerCreateInfo *ci, VkDeviceSize *used, uint32_t *index, const char **step)
{
    if (!src || (!src->levels && !src->upload) || !src->level_count ||
        (src->depth && src->array_layers) || (src->cube && (src->depth || src->array_layers || src->width != src->height)))
        return failure(step, "invalid sampled image shape or mip data", VK_ERROR_INITIALIZATION_FAILED);
    const uint32_t layers = image_layer_count(src->array_layers, src->cube);
    const struct dm_m4_draw_sample_upload *upload = src->upload;
    if (upload && (!upload->encoded || !upload->layer_bytes || upload->layers != layers ||
                   upload->layer_stride < upload->layer_bytes || upload->layer_bytes > upload->encoded_size ||
                   layers - 1u > (upload->encoded_size - upload->layer_bytes) / upload->layer_stride ||
                   upload->encoded_size > UINTPTR_MAX - (uintptr_t)upload->encoded ||
                   ((upload->materialize != NULL) == (upload->write_mip != NULL))))
        return failure(step, "invalid checked sampled image backing", VK_ERROR_INITIALIZATION_FAILED);
    VkFormatFeatureFlags features = VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (ci && !ci->compareEnable && (ci->magFilter == VK_FILTER_LINEAR || ci->minFilter == VK_FILTER_LINEAR ||
                                    ci->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR || ci->anisotropyEnable))
        features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    struct draw_sample_key key = {.address = (uintptr_t)src};
    if (upload && upload->pool_generation && upload->id) key = (struct draw_sample_key){
        .address = (uintptr_t)upload->encoded, .generation = upload->pool_generation, .id = upload->id,
        .kind = upload->kind, .layer_bytes = upload->layer_bytes, .layer_stride = upload->layer_stride,
        .layout = upload->layout,
    };
    const uint32_t hash = sample_hash(&key);
    VkResult r = prepare_sample_lookup(d, d->sample_slots + 1u);
    if (r != VK_SUCCESS) return failure(step, "allocate sampled image lookup", r);
    const uint32_t i = find_sample(d, &key, hash, src);
    if (i != UINT32_MAX) {
        struct draw_sample *sample = &d->samples[i];
        if ((sample->image.features & features) != features)
            return failure(step, "cached draw image format lacks required optimal-tiling features", VK_ERROR_FORMAT_NOT_SUPPORTED);
        if (i < d->sample_count) { *index = i; return VK_SUCCESS; }
        struct draw_sample *next = &d->samples[d->sample_count];
        swap_sample_slots(d, d->sample_count, i);
        if (key.generation && same_sample_backing(next, upload)) {
            next->upload = NULL;
            next->pixels = NULL;
            *index = d->sample_count++;
            return VK_SUCCESS;
        }
    }
    const uint32_t limit = src->cube ? d->limits.maxImageDimensionCube :
                           src->depth ? d->limits.maxImageDimension3D : d->limits.maxImageDimension2D;
    if (!src->width || !src->height || src->width > limit || src->height > limit || src->depth > limit ||
        src->level_count > mip_count(src->width, src->height, src->depth))
        return failure(step, "invalid sampled image dimensions or mip levels", VK_ERROR_INITIALIZATION_FAILED);
    if (layers > d->limits.maxImageArrayLayers)
        return failure(step, "sampled image layers exceed device limit", VK_ERROR_FEATURE_NOT_PRESENT);
    if (layers > SIZE_MAX / sizeof *src->levels / src->level_count)
        return failure(step, "sampled image plane count overflow", VK_ERROR_OUT_OF_HOST_MEMORY);
    if ((unsigned)src->components.r > VK_COMPONENT_SWIZZLE_A || (unsigned)src->components.g > VK_COMPONENT_SWIZZLE_A ||
        (unsigned)src->components.b > VK_COMPONENT_SWIZZLE_A || (unsigned)src->components.a > VK_COMPONENT_SWIZZLE_A)
        return failure(step, "unsupported sampled image swizzle", VK_ERROR_FEATURE_NOT_PRESENT);
    struct draw_block block;
    VkImageAspectFlags aspects;
    r = sampled_block(d, src->format, &block, &aspects, step);
    if (r != VK_SUCCESS) return r;
    // A miss needs an active slot, not ownership of the previous draw's
    // first resident image. Keep that image indexed when there is room;
    // otherwise reuse an inactive tail slot instead of an active binding.
    const int preserve = i == UINT32_MAX && d->sample_count < d->sample_slots &&
                         d->samples[d->sample_count].resident;
    uint32_t slots = d->sample_slots;
    if (slots == d->sample_count || (preserve && slots - d->sample_count < SAMPLE_INACTIVE_SLOTS &&
        d->sample_image_bytes < SAMPLE_CACHE_BYTES && d->sample_snapshot_bytes < SAMPLE_CACHE_BYTES))
        ++slots;
    void *samples = d->samples;
    r = grow_array(&samples, sizeof *d->samples, slots, &d->sample_capacity, 1);
    if (r != VK_SUCCESS) return failure(step, "grow staged sampled images", r);
    d->samples = samples;
    d->sample_slots = slots;
    if (preserve) swap_sample_slots(d, d->sample_count, slots - 1u);
    struct draw_sample *sample = &d->samples[d->sample_count];
    const struct dm_m4_draw_pixels *pixels = upload && upload->write_mip ? NULL : src->levels;
    if (upload && upload->materialize) {
        const char *err = upload->materialize(upload->context, &pixels);
        if (err || !pixels)
            return failure(step, err ? err : "empty checked sampled mip data", VK_ERROR_INITIALIZATION_FAILED);
    }
    // One copy region per mip covers its contiguous array layers or depth slices.
    for (uint32_t mip = 0; mip < src->level_count; ++mip) {
        if (!plan_planes(pixels ? &pixels[mip] : NULL, layers, src->level_count,
                         mip_size(src->width, mip), mip_size(src->height, mip),
                         mip_size(src->depth, mip), block, used, &sample->levels[mip]))
            return failure(step, "invalid linear sampled image rows", VK_ERROR_INITIALIZATION_FAILED);
    }
    unindex_sample(d, sample);
    sample->resident = 0;
    d->sample_image_bytes -= sample->image.allocation_size;
    r = ensure_image_view(d, &sample->image, src->format, src->width, src->height, src->depth, src->level_count,
                          src->array_layers, src->cube, aspects, src->components,
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, features, step);
    if (r != VK_SUCCESS) {
        destroy_image(d, &sample->image);
        remember_sample_backing(d, sample, NULL);
        return r;
    }
    d->sample_image_bytes += sample->image.allocation_size;
    sample->key = key;
    sample->hash = hash;
    sample->upload = upload;
    sample->pixels = pixels;
    index_sample(d, d->sample_count);
    *index = d->sample_count++;
    return VK_SUCCESS;
}

static VkResult upload_sample(struct dm_m4_draw *d, struct draw_sample *sample,
                              VkPipelineStageFlags stages, const char **step)
{
    const struct draw_image *image = &sample->image;
    const struct dm_m4_draw_sample_upload *upload = sample->upload;
    const struct dm_m4_draw_pixels *pixels = sample->pixels;
    sample->upload = NULL;
    sample->pixels = NULL;
    const uint32_t layers = image_layer_count(image->array_layers, image->cube);
    VkBufferImageCopy copies[DRAW_MIPS];
    for (uint32_t mip = 0; mip < image->levels; ++mip) {
        struct draw_transfer t = sample->levels[mip];
        copies[mip] = image_copy_region(&t, mip_size(image->width, mip), mip_size(image->height, mip), mip, image->aspects);
        copies[mip].imageSubresource.layerCount = layers;
        copies[mip].imageExtent.depth = mip_size(image->depth, mip);
        const size_t layer_bytes = t.row * t.rows;
        if (upload && upload->write_mip) {
            const struct dm_m4_draw_mutable_pixels dst = {d->mapped + t.offset, layers * layer_bytes, t.row};
            const char *err = upload->write_mip(upload->context, mip, dst);
            if (err) return failure(step, err, VK_ERROR_INITIALIZATION_FAILED);
        } else {
            for (uint32_t layer = 0; layer < layers; ++layer) {
                const struct dm_m4_draw_pixels *p = &pixels[(size_t)layer * image->levels + mip];
                t.pitch = p->row_pitch ? p->row_pitch : t.row;
                upload_plane(d, &t, p->data);
                t.offset += layer_bytes;
            }
        }
    }
    // Capture the input before the GPU or CPU writeback can change the guest
    // bytes. The submission publishes residency only after every check passes.
    remember_sample_backing(d, sample, sample->key.generation ? upload : NULL);
    image_barrier(d, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    d->CmdCopyBufferToImage(d->cmd, d->buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, image->levels, copies);
    image_barrier(d, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, sample->snapshot ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : stages,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    return VK_SUCCESS;
}
