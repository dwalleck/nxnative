static VkResult ensure_blit_image(struct dm_m4_draw *d, struct draw_image *image, VkFormat format,
        const struct dm_m4_blit_plane *p, int volume, VkFormatFeatureFlags features, const char **step)
{
    const uint32_t depth = volume ? p->images : 0, layers = volume ? 1 : p->images;
    if (image->image && image->format == format && image->width == p->width && image->height == p->height &&
        image->depth == depth && image->array_layers == layers) {
        if ((image->features & features) == features) return VK_SUCCESS;
        return failure(step, "cached Vulkan blit format lacks required features", VK_ERROR_FORMAT_NOT_SUPPORTED);
    }
    VkFormatProperties props;
    d->GetPhysicalDeviceFormatProperties(d->device->physical, format, &props);
    if ((props.optimalTilingFeatures & features) != features)
        return failure(step, "Vulkan optimal format cannot blit with requested filter", VK_ERROR_FORMAT_NOT_SUPPORTED);
    const VkImageCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
        .format = format, .extent = {p->width, p->height, volume ? depth : 1},
        .mipLevels = 1, .arrayLayers = layers, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    };
    VkImageFormatProperties shape;
    VkResult r = d->GetPhysicalDeviceImageFormatProperties(d->device->physical, format, ci.imageType,
                                                           ci.tiling, ci.usage, 0, &shape);
    if (r != VK_SUCCESS) return failure(step, "query Vulkan blit image shape", r);
    if (p->width > shape.maxExtent.width || p->height > shape.maxExtent.height ||
        ci.extent.depth > shape.maxExtent.depth || layers > shape.maxArrayLayers)
        return failure(step, "Vulkan blit image exceeds format limits", VK_ERROR_FORMAT_NOT_SUPPORTED);
    r = create_draw_image(d, image, &ci, step);
    if (r != VK_SUCCESS) return r;
    image->format = format;
    image->features = props.optimalTilingFeatures;
    image->aspects = VK_IMAGE_ASPECT_COLOR_BIT;
    image->width = p->width;
    image->height = p->height;
    image->depth = depth;
    image->levels = 1;
    image->array_layers = layers;
    return VK_SUCCESS;
}

static int blit_integer_type(VkFormat format)
{
#define BLIT_INT_CASES(suffix) \
        case VK_FORMAT_R8_##suffix: case VK_FORMAT_R8G8_##suffix: \
        case VK_FORMAT_R8G8B8_##suffix: case VK_FORMAT_R8G8B8A8_##suffix: \
        case VK_FORMAT_R16_##suffix: case VK_FORMAT_R16G16_##suffix: \
        case VK_FORMAT_R16G16B16_##suffix: case VK_FORMAT_R16G16B16A16_##suffix: \
        case VK_FORMAT_R32_##suffix: case VK_FORMAT_R32G32_##suffix: \
        case VK_FORMAT_R32G32B32_##suffix: case VK_FORMAT_R32G32B32A32_##suffix: \
        case VK_FORMAT_A2B10G10R10_##suffix##_PACK32
    switch (format) {
    BLIT_INT_CASES(UINT): return 1;
    BLIT_INT_CASES(SINT): return -1;
    default: return 0;
    }
#undef BLIT_INT_CASES
}

static int blit_plane_size(const struct dm_m4_blit_plane *p, VkFormat format)
{
    const uint32_t bpb = texel_bytes(format);
    if (!bpb || !p->transfer || !p->width || !p->height || !p->images ||
        p->width > INT32_MAX || p->height > INT32_MAX || p->images > INT32_MAX)
        return 0;
    const size_t row = (size_t)p->width * bpb;
    return p->height <= SIZE_MAX / row && p->images <= SIZE_MAX / (row * p->height) &&
           p->bytes == row * p->height * p->images;
}

VkResult dm_m4_texture_blit(struct dm_m4_draw *d, const struct dm_m4_blit_plane planes[2], int volume,
                           uint32_t flags, const char **step)
{
    if (step) *step = NULL;
    const VkImageLayout read = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, write = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkAccessFlags read_access = VK_ACCESS_TRANSFER_READ_BIT, write_access = VK_ACCESS_TRANSFER_WRITE_BIT;
    const int filtered = (flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) != 0;
    const int mirror_z = (flags & NVN_COPY_FLAGS_MIRROR_Z_BIT) != 0;
    const VkFilter filter = filtered ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    const VkFormatFeatureFlags common = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    const VkFormatFeatureFlags features[] = {
        common | VK_FORMAT_FEATURE_BLIT_SRC_BIT | (filtered ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT : 0),
        common | VK_FORMAT_FEATURE_BLIT_DST_BIT,
    };
    VkFormat formats[2];
    VkBufferImageCopy copies[2] = {0};
    VkImageBlit blit = {0};
    VkDeviceSize capacity = 0;
    if (!volume && planes[0].images != planes[1].images)
        return failure(step, "Vulkan blit array layer counts differ", VK_ERROR_FORMAT_NOT_SUPPORTED);
    for (unsigned i = 0; i < 2; ++i) {
        const struct dm_m4_blit_plane *p = &planes[i];
        formats[i] = dm_m4_nvn_attachment_format(p->format);
        if (!blit_plane_size(p, formats[i]))
            return failure(step, "unsupported NVN texture format or blit extent", VK_ERROR_FORMAT_NOT_SUPPORTED);
        copies[i].imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, volume ? 1 : p->images};
        copies[i].imageExtent = (VkExtent3D){p->width, p->height, volume ? p->images : 1};
        if (p->bytes > capacity) capacity = p->bytes;
    }
    if (blit_integer_type(formats[0]) != blit_integer_type(formats[1]) || (filtered && blit_integer_type(formats[0])))
        return failure(step, "Vulkan blit cannot filter integer or mix incompatible numeric formats", VK_ERROR_FORMAT_NOT_SUPPORTED);
    blit.srcSubresource = copies[0].imageSubresource;
    blit.dstSubresource = copies[1].imageSubresource;
    blit.srcOffsets[1] = (VkOffset3D){(int32_t)planes[0].width, (int32_t)planes[0].height,
                                     (int32_t)copies[0].imageExtent.depth};
    blit.dstOffsets[1] = (VkOffset3D){(int32_t)planes[1].width, (int32_t)planes[1].height,
                                     (int32_t)copies[1].imageExtent.depth};
    if (flags & NVN_COPY_FLAGS_MIRROR_X_BIT) { blit.srcOffsets[0].x = blit.srcOffsets[1].x; blit.srcOffsets[1].x = 0; }
    if (flags & NVN_COPY_FLAGS_MIRROR_Y_BIT) { blit.srcOffsets[0].y = blit.srcOffsets[1].y; blit.srcOffsets[1].y = 0; }
    if (volume && mirror_z) { blit.srcOffsets[0].z = blit.srcOffsets[1].z; blit.srcOffsets[1].z = 0; }

    VkResult r = reset_recording(d, step);
    if (r != VK_SUCCESS) return r;
    r = ensure_buffer(d, capacity, 0, step);
    if (r != VK_SUCCESS) return r;
    for (unsigned i = 0; i < 2; ++i) {
        r = ensure_blit_image(d, &d->blit_images[i], formats[i], &planes[i], volume, features[i], step);
        if (r != VK_SUCCESS) return r;
    }
    r = begin_recording(d, step);
    if (r != VK_SUCCESS) return r;
    const struct draw_image *src = &d->blit_images[0], *dst = &d->blit_images[1];
    planes[0].transfer(planes[0].context, 0, d->mapped);
    image_barrier(d, src, VK_IMAGE_LAYOUT_UNDEFINED, write, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, transfer, 0, write_access);
    d->CmdCopyBufferToImage(d->cmd, d->buffer, src->image, write, 1, &copies[0]);
    image_barrier(d, src, write, read, transfer, transfer, write_access, read_access);
    image_barrier(d, dst, VK_IMAGE_LAYOUT_UNDEFINED, write, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, transfer, 0, write_access);
    if (mirror_z && !volume && planes[0].images > 1) {
        blit.srcSubresource.layerCount = blit.dstSubresource.layerCount = 1;
        for (uint32_t z = 0; z < planes[0].images; ++z) {
            blit.srcSubresource.baseArrayLayer = planes[0].images - 1 - z;
            blit.dstSubresource.baseArrayLayer = z;
            d->CmdBlitImage(d->cmd, src->image, read, dst->image, write, 1, &blit, filter);
        }
    } else {
        d->CmdBlitImage(d->cmd, src->image, read, dst->image, write, 1, &blit, filter);
    }
    image_barrier(d, dst, write, read, transfer, transfer, write_access, read_access);
    // The image barriers order the two transfer uses of this same staging range.
    d->CmdCopyImageToBuffer(d->cmd, dst->image, read, d->buffer, 1, &copies[1]);
    r = finish_recording(d, capacity, transfer, write_access, step);
    if (r == VK_SUCCESS) planes[1].transfer(planes[1].context, 1, d->mapped);
    return r;
}
