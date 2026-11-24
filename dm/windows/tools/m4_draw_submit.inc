enum { TARGET_READBACK_CHUNK = 4096 };

struct draw_target_range {
    size_t first, end; // Bytes for a tight plane, rows otherwise; end == 0 means full copy.
};

struct draw_target {
    struct draw_image *image;
    struct draw_transfer pixels, stencil;
    struct draw_target_range partial;
    VkBufferImageCopy copies[2];
    uint32_t copy_count;
    VkImageAspectFlags load, resident, store, retain;
    VkImageLayout layout;
    VkPipelineStageFlags stage;
    VkAccessFlags read, write;
};

static int stencil_may_write(const VkStencilOpState *s)
{
    // All supported stencil planes are S8.
    return (s->writeMask & UINT8_MAX) &&
           (s->failOp != VK_STENCIL_OP_KEEP || s->passOp != VK_STENCIL_OP_KEEP ||
            s->depthFailOp != VK_STENCIL_OP_KEEP);
}

// Called after pipeline validation and only when rasterization can cover samples.
// These states are static in the Vk pipeline; do not infer fragment outputs.
static struct dm_m4_draw_attachment_writes target_writes(const struct dm_m4_draw_info *info)
{
    struct dm_m4_draw_attachment_writes writes = {0};
    const VkFormat format = info->depth.format;
    const VkPipelineDepthStencilStateCreateInfo *zs = info->pipeline.depth_stencil;
    const int have_depth = format != VK_FORMAT_UNDEFINED && format != VK_FORMAT_S8_UINT;
    const int depth_rejects = have_depth && zs->depthTestEnable && zs->depthCompareOp == VK_COMPARE_OP_NEVER;
    if (!depth_rejects) {
        const VkPipelineColorBlendStateCreateInfo *blend = info->pipeline.blend;
        for (uint32_t i = 0; i < info->color_count; ++i)
            if (info->colors[i].format != VK_FORMAT_UNDEFINED && blend->pAttachments[i].colorWriteMask)
                writes.colors |= 1u << i;
        if (have_depth && zs->depthTestEnable && zs->depthWriteEnable)
            writes.depth_stencil |= VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    // Rejected fragments can still write stencil through failOp or depthFailOp.
    if (has_stencil(format) && zs->stencilTestEnable && (stencil_may_write(&zs->front) || stencil_may_write(&zs->back)))
        writes.depth_stencil |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return writes;
}

static VkImageAspectFlags depth_stencil_load(const struct dm_m4_draw_info *info, VkImageAspectFlags writes)
{
    const VkPipelineDepthStencilStateCreateInfo *zs = info->pipeline.depth_stencil;
    // Full-plane readback needs all original texels of a potentially writable
    // plane, even when the test itself does not use the old depth value.
    if (zs->depthBoundsTestEnable ||
        (zs->depthTestEnable && zs->depthCompareOp != VK_COMPARE_OP_ALWAYS && zs->depthCompareOp != VK_COMPARE_OP_NEVER))
        writes |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (zs->stencilTestEnable) writes |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return writes;
}

static VkImageAspectFlags reusable_target_aspects(VkFormat format)
{
    // Keep float and SNORM planes on the original readback/reupload path:
    // NaN payloads and alternate encodings do not prove an exact round trip.
    switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_D24_UNORM_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_STENCIL_BIT;
#define FIXED_COLOR(format) \
    case VK_FORMAT_##format##_UNORM: case VK_FORMAT_##format##_UINT: case VK_FORMAT_##format##_SINT:
    FIXED_COLOR(R8)
    FIXED_COLOR(R8G8)
    FIXED_COLOR(R8G8B8)
    FIXED_COLOR(R8G8B8A8)
    FIXED_COLOR(R16)
    FIXED_COLOR(R16G16)
    FIXED_COLOR(R16G16B16)
    FIXED_COLOR(R16G16B16A16)
#undef FIXED_COLOR
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT:
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT:
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16:
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_A2B10G10R10_SINT_PACK32:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    default:
        return 0;
    }
}

static VkResult prepare_target(struct dm_m4_draw *d, struct draw_image *image, const struct dm_m4_draw_attachment *src,
                               int depth, int blend, VkImageAspectFlags load, VkImageAspectFlags store,
                               VkDeviceSize *used, struct draw_target *target, const char **step)
{
    const int stencil_only = depth && src->format == VK_FORMAT_S8_UINT;
    const int stencil = depth && !stencil_only && has_stencil(src->format);
    const uint32_t bytes = stencil_only ? 1u : depth ? depth_bytes(src->format) : texel_bytes(src->format);
    if (!bytes) return failure(step, depth ? "unsupported depth attachment format" : "unsupported color attachment format",
                               VK_ERROR_FORMAT_NOT_SUPPORTED);
    if (!src->width || !src->height || src->width > d->limits.maxFramebufferWidth ||
        src->height > d->limits.maxFramebufferHeight || src->width > d->limits.maxImageDimension2D ||
        src->height > d->limits.maxImageDimension2D || src->width > INT32_MAX || src->height > INT32_MAX)
        return failure(step, "draw target dimensions exceed device limits", VK_ERROR_FEATURE_NOT_PRESENT);
    const VkImageAspectFlags aspect = stencil_only ? VK_IMAGE_ASPECT_STENCIL_BIT :
                                      depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageAspectFlags aspects = aspect | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    target->store = store & aspects;
    target->load = (load | store) & aspects;
    target->partial.end = 0;
    target->retain = src->output_token ? target->store & reusable_target_aspects(src->format) : 0;
    // A proof lives on this exact image, and ensure_image keeps a target image
    // only for the same format and extent. Replacing it resets its proof.
    target->resident = src->input_token && src->input_token == image->target_token &&
        image->format == src->format && image->width == src->width && image->height == src->height ?
        target->load & image->target_stored : 0;
    const VkImageAspectFlags staging = (target->load & ~target->resident) | target->store;
    // Always validate supplied rows. Reserve staging only for transfers.
    VkDeviceSize unused = 0;
    if (!plan_plane(src->pixels.data, src->pixels.size, src->pixels.row_pitch, src->width, src->height,
                    (struct draw_block){1, 1, bytes}, staging & aspect ? used : &unused, &target->pixels))
        return failure(step, depth ? "invalid linear depth target rows" : "invalid linear color target rows",
                       VK_ERROR_INITIALIZATION_FAILED);
    if (stencil) {
        if (!plan_plane(src->stencil.data, src->stencil.size, src->stencil.row_pitch, src->width, src->height,
                        (struct draw_block){1, 1, 1}, staging & VK_IMAGE_ASPECT_STENCIL_BIT ? used : &unused, &target->stencil))
            return failure(step, "invalid linear stencil target rows", VK_ERROR_INITIALIZATION_FAILED);
    } else if (src->stencil.data || src->stencil.size || src->stencil.row_pitch) {
        return failure(step, "stencil plane supplied without a combined depth/stencil format", VK_ERROR_FORMAT_NOT_SUPPORTED);
    }
    target->copy_count = stencil ? 2u : 1u;
    target->copies[0] = image_copy_region(&target->pixels, src->width, src->height, 0, aspect);
    if (stencil) target->copies[1] = image_copy_region(&target->stencil, src->width, src->height, 0, VK_IMAGE_ASPECT_STENCIL_BIT);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkFormatFeatureFlags features = VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (depth) {
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        target->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        target->stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        target->read = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        target->write = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    } else {
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        if (blend) features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
        target->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        target->stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        target->read = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
        target->write = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    }
    target->image = image;
    return ensure_image(d, image, src->format, src->width, src->height, 1, aspects,
                        (VkComponentMapping){0}, usage, features, step);
}

static uint32_t target_copies(const struct draw_target *t, VkImageAspectFlags aspects, const VkBufferImageCopy **copies)
{
    const uint32_t first = (aspects & t->copies[0].imageSubresource.aspectMask) ? 0u : 1u;
    *copies = t->copies + first;
    return !first && t->copy_count == 2 && (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? 2u : 1u;
}

static void upload_target(struct dm_m4_draw *d, const struct dm_m4_draw_attachment *src, const struct draw_target *t)
{
    // Invalidate on use, including STORE_OP_NONE and failures after recording.
    // Only a completed readback below can publish another proof.
    t->image->target_token = 0;
    const VkImageAspectFlags upload = t->load & ~t->resident;
    const VkImageLayout old = t->resident ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    const VkPipelineStageFlags from = t->resident ? t->stage | VK_PIPELINE_STAGE_TRANSFER_BIT :
                                                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    const VkAccessFlags access = t->resident ? t->write | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT : 0;
    if (!upload) {
        image_barrier(d, t->image, old, t->layout, from, t->stage, access, t->write | (t->load ? t->read : 0));
        return;
    }
    if (upload & t->copies[0].imageSubresource.aspectMask) upload_plane(d, &t->pixels, src->pixels.data);
    if (t->copy_count == 2 && (upload & VK_IMAGE_ASPECT_STENCIL_BIT)) upload_plane(d, &t->stencil, src->stencil.data);
    // Transition both aspects together; a miss in one aspect must not discard
    // the other aspect when it has a matching proof.
    image_barrier(d, t->image, old, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, from,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, access, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkBufferImageCopy *copies;
    const uint32_t count = target_copies(t, upload, &copies);
    d->CmdCopyBufferToImage(d->cmd, d->buffer, t->image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, count, copies);
    image_barrier(d, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t->layout,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, t->stage, VK_ACCESS_TRANSFER_WRITE_BIT, t->read | t->write);
}

static void readback_target(struct dm_m4_draw *d, const struct draw_target *t)
{
    // An upload may have read the same staging range and written texels
    // outside the render area. Include these transfer accesses.
    image_barrier(d, t->image, t->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  t->stage | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  t->write | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferImageCopy *copies;
    const uint32_t count = target_copies(t, t->store, &copies);
    d->CmdCopyImageToBuffer(d->cmd, t->image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->buffer, count, copies);
}

static VkRenderingAttachmentInfoKHR render_target(const struct draw_target *t, VkImageAspectFlagBits aspect)
{
    return (VkRenderingAttachmentInfoKHR){
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR,
        .imageView = t->image->view,
        .imageLayout = t->layout,
        .loadOp = (t->load & aspect) ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = (t->store & aspect) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_NONE_KHR,
    };
}

static int target_span_equal(const uint8_t *a, const uint8_t *b, size_t bytes, int depth24)
{
    if (m4_bytes_equal(a, b, bytes)) return 1;
    if (!depth24) return 0;
    // Vulkan leaves X8 undefined in an X8_D24 depth copy.
    for (size_t x = 0; x < bytes; x += 4)
        if (memcmp(a + x, b + x, 3)) return 0;
    return 1;
}

static int target_plane_changed(const struct dm_m4_draw *d, const struct draw_transfer *t, const void *input, int depth24)
{
    const uint8_t *returned = d->mapped + t->offset, *original = input;
    const int tight = t->row == t->pitch;
    const size_t bytes = tight ? t->rows * t->row : t->row;
    const size_t spans = tight ? 1 : t->rows;
    for (size_t y = 0; y < spans; ++y)
        if (!target_span_equal(returned + y * t->row, original + y * t->pitch, bytes, depth24)) return 1;
    return 0;
}

static int plan_target_range(const struct dm_m4_draw *d, const struct draw_transfer *t, const void *input,
                             int depth24, struct draw_target_range *partial)
{
    const uint8_t *returned = d->mapped + t->offset, *original = input;
    const int tight = t->row == t->pitch;
    const size_t bytes = t->rows * t->row;
    const size_t step = tight ? TARGET_READBACK_CHUNK : t->row;
    const size_t pitch = tight ? step : t->pitch;
    const size_t count = tight ? 1 + (bytes - 1) / step : t->rows;
    const size_t tail = tight ? bytes - (count - 1) * step : step;

    // Find the two ends without rescanning the equal prefix or inspecting
    // the middle of a dense change. For padded inputs, compare only texels.
    size_t first = 0;
    while (first < count && target_span_equal(returned + first * step, original + first * pitch,
                                               first == count - 1 ? tail : step, depth24))
        ++first;
    if (first == count) return 0;
    size_t last = count - 1;
    while (last > first && target_span_equal(returned + last * step, original + last * pitch,
                                             last == count - 1 ? tail : step, depth24))
        --last;

    if (first || last != count - 1) {
        partial->first = tight ? first * step : first;
        partial->end = tight ? last * step + (last == count - 1 ? tail : step) : last + 1;
    }
    return 1;
}

static int target_download_aliases(const struct dm_m4_draw *d, const struct dm_m4_draw_info *info,
                                   const struct dm_m4_draw_mutable_pixels *pixels, uint32_t earlier_colors)
{
    // A direct caller with overlapping outputs still gets whole-plane copies
    // in their original order. The guest path rejects overlapping targets.
    if ((d->gmem_writes && gmem_host_range(d, pixels->data, pixels->size)) ||
        overlap(pixels->data, pixels->size, info->samples_passed, info->samples_passed ? sizeof *info->samples_passed : 0))
        return 1;
    for (uint32_t i = 0; earlier_colors; ++i, earlier_colors >>= 1)
        if ((earlier_colors & 1u) &&
            overlap(pixels->data, pixels->size, info->colors[i].pixels.data, info->colors[i].pixels.size))
            return 1;
    return 0;
}

static VkImageAspectFlags changed_target_aspects(const struct dm_m4_draw *d, const struct dm_m4_draw_info *info,
                                                 const struct dm_m4_draw_attachment *src, struct draw_target *t,
                                                 uint32_t earlier_colors)
{
    const VkImageAspectFlags aspects = t->store;
    VkImageAspectFlags changed = 0;
    const VkImageAspectFlags first = t->copies[0].imageSubresource.aspectMask;
    const int depth24 = src->format == VK_FORMAT_X8_D24_UNORM_PACK32 || src->format == VK_FORMAT_D24_UNORM_S8_UINT;
    if (t->copy_count == 1) {
        if (!plan_target_range(d, &t->pixels, src->pixels.data, depth24, &t->partial)) return 0;
        if (t->partial.end && target_download_aliases(d, info, &src->pixels, earlier_colors)) t->partial.end = 0;
        return aspects;
    }
    if ((aspects & first) && target_plane_changed(d, &t->pixels, src->pixels.data, depth24))
        changed |= first;
    if ((aspects & VK_IMAGE_ASPECT_STENCIL_BIT) && target_plane_changed(d, &t->stencil, src->stencil.data, 0))
        changed |= VK_IMAGE_ASPECT_STENCIL_BIT;
    return changed;
}

static void download_target(struct dm_m4_draw *d, const struct dm_m4_draw_attachment *src,
                            const struct draw_target *t, VkImageAspectFlags aspects)
{
    if (t->partial.end) {
        const struct draw_transfer *p = &t->pixels;
        const uint8_t *from = d->mapped + p->offset;
        uint8_t *to = src->pixels.data;
        const size_t first = t->partial.first, end = t->partial.end;
        if (p->row == p->pitch) {
            memcpy(to + first, from + first, end - first);
        } else {
            for (size_t y = first; y < end; ++y) memcpy(to + y * p->pitch, from + y * p->row, p->row);
        }
        return;
    }
    if (aspects & t->copies[0].imageSubresource.aspectMask) download_plane(d, &t->pixels, src->pixels.data);
    if (t->copy_count == 2 && (aspects & VK_IMAGE_ASPECT_STENCIL_BIT))
        download_plane(d, &t->stencil, src->stencil.data);
}

static void complete_target(const struct dm_m4_draw_attachment *src, const struct draw_target *t)
{
    if (!t->retain) return;
    // Use the store mask, not the changed-byte mask: a STORE with identical
    // readback is valid, and STORE_OP_NONE never supplies a proof.
    t->image->target_stored = t->retain;
    t->image->target_token = src->output_token;
}

static VkResult prepare_rate_image(struct dm_m4_draw *d, const struct dm_m4_rate_image *src, VkDeviceSize *used,
                                   struct draw_transfer *texels, const char **step)
{
    if (!d->CmdBindShadingRateImageNV)
        return failure(step, "NV shading-rate image feature not enabled", VK_ERROR_FEATURE_NOT_PRESENT);
    if (!src->texels.data) {
        if (!src->width && !src->height && !src->texels.size && !src->texels.row_pitch)
            return VK_SUCCESS; // Null image still uses the active palette at index zero.
        return failure(step, "invalid NV shading-rate image texel rows", VK_ERROR_INITIALIZATION_FAILED);
    }
    if (src->width > d->rate_max_extent.width || src->height > d->rate_max_extent.height)
        return failure(step, "NV shading-rate image dimensions exceed format limits", VK_ERROR_FORMAT_NOT_SUPPORTED);
    if (!plan_plane(src->texels.data, src->texels.size, src->texels.row_pitch, src->width, src->height,
                    (struct draw_block){1, 1, 1}, used, texels))
        return failure(step, "invalid NV shading-rate image texel rows", VK_ERROR_INITIALIZATION_FAILED);
    const uint8_t *data = src->texels.data;
    for (size_t y = 0; y < texels->rows; ++y)
        for (size_t x = 0; x < texels->row; ++x)
            if (data[y * texels->pitch + x] >= DM_M4_RATE_PALETTE_SIZE)
                return failure(step, "NV shading-rate image index exceeds palette", VK_ERROR_INITIALIZATION_FAILED);
    return ensure_image(d, &d->rate_image, VK_FORMAT_R8_UINT, src->width, src->height, 1,
                        VK_IMAGE_ASPECT_COLOR_BIT, (VkComponentMapping){0}, rate_usage,
                        VK_FORMAT_FEATURE_TRANSFER_DST_BIT, step);
}

static void bind_rate_image(struct dm_m4_draw *d, const struct dm_m4_rate_image *src, const struct draw_transfer *texels)
{
    if (!src->texels.data) {
        d->CmdBindShadingRateImageNV(d->cmd, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED);
        return;
    }
    struct draw_image *image = &d->rate_image;
    upload_plane(d, texels, src->texels.data);
    // Each replay owns a fenced submission and uploads the full image again.
    image_barrier(d, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    const VkBufferImageCopy copy = image_copy_region(texels, src->width, src->height, 0, VK_IMAGE_ASPECT_COLOR_BIT);
    d->CmdCopyBufferToImage(d->cmd, d->buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    image_barrier(d, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADING_RATE_OPTIMAL_NV,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_SHADING_RATE_IMAGE_BIT_NV,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADING_RATE_IMAGE_READ_BIT_NV);
    d->CmdBindShadingRateImageNV(d->cmd, image->view, VK_IMAGE_LAYOUT_SHADING_RATE_OPTIMAL_NV);
}

static VkResult prepare_direct(struct dm_m4_draw *d, const struct dm_m4_shader_binding *recipe,
                               const struct dm_m4_draw_binding *src, int compute, VkDeviceSize *used,
                               struct draw_binding *plan, const char **step)
{
    const uint32_t count = src->descriptor_count;
    const int samplers = recipe->descriptor_type == VK_DESCRIPTOR_TYPE_SAMPLER;
    if (!d->sampled_nonuniform || (!samplers && recipe->descriptor_type != VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) ||
        count > recipe->count || count > UINT32_MAX - d->array_info_count ||
        (count && (samplers ? src->sampler_array == NULL : src->texture_array == NULL)))
        return failure(step, "invalid partially bound sampled descriptor array", VK_ERROR_FEATURE_NOT_PRESENT);
    const uint32_t first = d->array_info_count;
    void *infos = d->array_infos;
    VkResult r = grow_array(&infos, sizeof *d->array_infos, first + count, &d->array_info_capacity, 0);
    if (r != VK_SUCCESS) return failure(step, "allocate compact descriptor writes", r);
    d->array_infos = infos;
    for (uint32_t i = 0; i < count; ++i) {
        VkDescriptorImageInfo *out = &d->array_infos[first + i];
        if (samplers) {
            struct draw_binding local = {0};
            r = prepare_separate_sampler(d, &src->sampler_array[i], &local, step);
            if (r != VK_SUCCESS) return r;
            *out = local.image;
        } else {
            uint32_t index;
            if (compute && src->texture_array[i] && src->texture_array[i]->upload)
                return failure(step, "compute sampled image requires eager planes for alias checks",
                               VK_ERROR_FEATURE_NOT_PRESENT);
            r = prepare_sample(d, src->texture_array[i], NULL, used, &index, step);
            if (r != VK_SUCCESS) return r;
            *out = (VkDescriptorImageInfo){VK_NULL_HANDLE, d->samples[index].image.view,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        }
    }
    plan->first_descriptor = first;
    plan->descriptor_count = count;
    d->array_info_count += count;
    return VK_SUCCESS;
}

static VkResult prepare_bindings(struct dm_m4_draw *d, const struct dm_m4_draw_stage *stages,
                                 uint32_t stage_count, const struct dm_m4_shader_support *support,
                                 VkDeviceSize *used, uint32_t counts[8], uint32_t total,
                                 struct sampled_types *numeric, const char **step)
{
    void *scratch = d->bindings;
    VkResult r = grow_array(&scratch, sizeof *d->bindings, total, &d->binding_capacity, 0);
    if (r != VK_SUCCESS) return failure(step, "grow draw binding plans", r);
    d->bindings = scratch;
    scratch = d->writes;
    r = grow_array(&scratch, sizeof *d->writes, total, &d->write_capacity, 0);
    if (r != VK_SUCCESS) return failure(step, "grow draw descriptor writes", r);
    d->writes = scratch;
    d->sample_count = 0;
    d->storage_image_count = 0;
    d->sampler_count = 0;
    d->array_info_count = 0;
    uint32_t n = 0, numeric_count = 0;
    for (uint32_t s = 0; s < stage_count; ++s) {
        const struct dm_m4_draw_stage *stage = &stages[s];
        const struct dm_m4_shader *sh = stage->shader;
        struct sampled_types *types = &numeric[sh->set];
        types->first = numeric_count;
        const int compute = sh->stage == VK_SHADER_STAGE_COMPUTE_BIT;
        for (uint32_t i = 0; i < sh->binding_count; ++i, ++n) {
            const struct dm_m4_shader_binding *recipe = &sh->bindings[i];
            const struct dm_m4_draw_binding *src = &stage->bindings[i];
            struct draw_binding *plan = &d->bindings[n];
            *plan = (struct draw_binding){0};
            const int direct = dm_m4_shader_direct(recipe);
            if (recipe->count != 1 && !direct)
                return failure(step, "descriptor array has no compact replay recipe", VK_ERROR_FEATURE_NOT_PRESENT);
            const VkDescriptorType type = recipe->descriptor_type;
            if ((unsigned)type >= 8u || recipe->count > UINT32_MAX - counts[type])
                return failure(step, "unsupported staged descriptor type or count", VK_ERROR_FEATURE_NOT_PRESENT);
            counts[type] += recipe->count;
            if (direct) {
                r = prepare_direct(d, recipe, src, compute, used, plan, step);
                if (r != VK_SUCCESS) return r;
                continue;
            }
            if (recipe->source == DM_M4_SHADER_GMEM_MAP || recipe->source == DM_M4_SHADER_GMEM_LOG) {
                const int log = recipe->source == DM_M4_SHADER_GMEM_LOG;
                if (type != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || !(sh->gmem & (log ? 2u : 1u)))
                    return failure(step, "invalid shader global memory descriptor", VK_ERROR_INVALID_SHADER_NV);
                plan->buffer.offset = log ? d->gmem_log_offset : d->gmem_map_offset;
                plan->size = plan->buffer.range = log ? GMEM_LOG_BYTES : gmem_map_size(d->gmem_read_alias);
                continue;
            }
            const int texel = type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER ||
                              type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            const int bindless_map = recipe->source == DM_M4_SHADER_BINDLESS_MAP;
            const int storage = recipe->source == DM_M4_SHADER_SBUF || bindless_map;
            size_t fixed = 0;
            if (recipe->source == DM_M4_SHADER_TEXTURE || recipe->source == DM_M4_SHADER_IMAGE) {
                if (!compute && type != VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
                    return failure(step, "draw only supports combined sampled image descriptors", VK_ERROR_FEATURE_NOT_PRESENT);
                switch (type) {
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: {
                    enum {
                        RYUJINX_SAMPLER_2D = 3, RYUJINX_SAMPLER_3D = 4, RYUJINX_SAMPLER_CUBE = 5,
                        RYUJINX_SAMPLER_2D_ARRAY = 3 | 0x100, RYUJINX_SAMPLER_SHADOW = 0x400,
                    };
                    const uint32_t shape = recipe->image.type & ~(uint32_t)RYUJINX_SAMPLER_SHADOW;
                    const int shadow = (recipe->image.type & RYUJINX_SAMPLER_SHADOW) != 0;
                    const int combined = type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    const VkSamplerCreateInfo *sampler = src->sampler;
                    if ((shape != RYUJINX_SAMPLER_2D && shape != RYUJINX_SAMPLER_3D &&
                         shape != RYUJINX_SAMPLER_CUBE && shape != RYUJINX_SAMPLER_2D_ARRAY) ||
                        (shadow && shape == RYUJINX_SAMPLER_3D) || recipe->image.row_bias || recipe->image.tbx >= 0)
                        return failure(step, "unsupported sampled image dimensionality",
                                       VK_ERROR_FEATURE_NOT_PRESENT);
                    if (combined && !src->texture && src->sampler) {
                        if (!d->device->null_descriptor)
                            return failure(step, "guest null texture requires Vulkan nullDescriptor", VK_ERROR_FEATURE_NOT_PRESENT);
                        r = prepare_separate_sampler(d, src->sampler, plan, step);
                        if (r != VK_SUCCESS) return r;
                        plan->image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        continue;
                    }
                    if (!src->texture || (recipe->image.format != VK_FORMAT_UNDEFINED &&
                                          recipe->image.format != src->texture->format))
                        return failure(step, "unresolved sampled image or format mismatch", VK_ERROR_FORMAT_NOT_SUPPORTED);
                    if ((shape == RYUJINX_SAMPLER_2D_ARRAY) != (src->texture->array_layers != 0) ||
                        (shape == RYUJINX_SAMPLER_3D) != (src->texture->depth != 0) ||
                        (shape == RYUJINX_SAMPLER_CUBE) != (src->texture->cube != 0))
                        return failure(step, "sampled image view type does not match shader reflection",
                                       VK_ERROR_FORMAT_NOT_SUPPORTED);
                    if (combined && !sampler) sampler = &src->texture->sampler;
                    if (compute && src->texture->upload)
                        return failure(step, "compute sampled image requires eager planes for alias checks",
                                       VK_ERROR_FEATURE_NOT_PRESENT);
                    uint32_t sample_index;
                    if ((shadow && !depth_bytes(src->texture->format)) ||
                        (combined && shadow != (sampler->compareEnable != 0)))
                        return failure(step, "comparison sampler or depth aspect does not match shader reflection",
                                       VK_ERROR_FORMAT_NOT_SUPPORTED);
                    if (combined) {
                        r = prepare_separate_sampler(d, sampler, plan, step);
                        if (r != VK_SUCCESS) return r;
                    }
                    r = prepare_sample(d, src->texture, combined ? sampler : NULL, used, &sample_index, step);
                    if (r != VK_SUCCESS) return r;
                    const struct draw_sample *sample = &d->samples[sample_index];
                    const int32_t sign = combined ? sampled_format_sign(sample->image.format) : 0;
                    if (sign) {
                        r = append_sampled_type(d, sh, &numeric_count, recipe->binding, sign, step);
                        if (r != VK_SUCCESS) return r;
                    }
                    plan->image = (VkDescriptorImageInfo){plan->image.sampler,
                                                         sample->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                    continue;
                }
                case VK_DESCRIPTOR_TYPE_SAMPLER:
                    r = prepare_separate_sampler(d, src->sampler, plan, step);
                    if (r != VK_SUCCESS) return r;
                    continue;
                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                    r = prepare_storage_image(d, recipe, src->image, used, plan, step);
                    if (r != VK_SUCCESS) return r;
                    continue;
                case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                    r = plan_texel_buffer(d, recipe, src, plan, step);
                    if (r != VK_SUCCESS) return r;
                    break;
                default:
                    return failure(step, "reflected image descriptor type mismatch", VK_ERROR_INVALID_SHADER_NV);
                }
            } else {
                const VkDescriptorType expected = storage ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                if (type != expected)
                    return failure(step, "reflected buffer descriptor type mismatch", VK_ERROR_INVALID_SHADER_NV);
                switch (recipe->source) {
                case DM_M4_SHADER_SUPPORT:
                    plan->data = support;
                    plan->size = sizeof *support;
                    break;
                case DM_M4_SHADER_CBUF:
                    fixed = recipe->cbuf.fixed_size;
                    if (recipe->cbuf.slot == 1u) {
                        plan->data = sh->c1;
                        plan->size = sh->c1_size;
                    } else {
                        plan->data = src->buffer.data;
                        plan->size = src->buffer.size;
                    }
                    break;
                case DM_M4_SHADER_BINDLESS_MAP:
                    if ((sh->stage == VK_SHADER_STAGE_FRAGMENT_BIT && !d->features.fragmentStoresAndAtomics) ||
                        (!compute && sh->stage != VK_SHADER_STAGE_FRAGMENT_BIT && !d->features.vertexPipelineStoresAndAtomics) ||
                        src->buffer.size < DM_M4_BINDLESS_HEADER_WORDS * sizeof(uint32_t))
                        return failure(step, "bindless fault map requires shader writes and a staged table",
                                       VK_ERROR_FEATURE_NOT_PRESENT);
                    plan->bindless_map_binding = recipe->sbuf.cb_offset + 1u;
                    plan->data = src->buffer.data;
                    plan->size = src->buffer.size;
                    break;
                case DM_M4_SHADER_SBUF:
                    plan->data = src->buffer.data;
                    plan->size = src->buffer.size;
                    if (recipe->sbuf.flags & 1u) {
                        if ((sh->stage == VK_SHADER_STAGE_VERTEX_BIT && !d->features.vertexPipelineStoresAndAtomics) ||
                            (sh->stage == VK_SHADER_STAGE_FRAGMENT_BIT && !d->features.fragmentStoresAndAtomics))
                            return failure(step, "shader storage writes not enabled for draw stage", VK_ERROR_FEATURE_NOT_PRESENT);
                        if (!src->writeback || src->writeback != plan->data)
                            return failure(step, "missing writable shader storage span", VK_ERROR_INITIALIZATION_FAILED);
                        plan->writeback = src->writeback;
                    }
                    break;
                default:
                    return failure(step, "unsupported staged descriptor source", VK_ERROR_FEATURE_NOT_PRESENT);
                }
            }
            size_t range = plan->size > fixed ? plan->size : fixed;
            if (!range || (plan->size && !plan->data))
                return failure(step, "unresolved draw buffer span", VK_ERROR_INITIALIZATION_FAILED);
            if (!texel && range > (storage ? d->limits.maxStorageBufferRange : d->limits.maxUniformBufferRange))
                return failure(step, storage ? "draw storage buffer exceeds device range" : "draw uniform buffer exceeds device range",
                               VK_ERROR_FEATURE_NOT_PRESENT);
            plan->storage = storage;
            plan->buffer.range = range;
        }
        types->count = numeric_count - types->first;
    }
    trim_inactive_samples(d);
    return VK_SUCCESS;
}

static VkResult plan_buffer_bindings(struct dm_m4_draw *d, uint32_t total, int compute, VkDeviceSize *used,
                                     VkBufferUsageFlags *usage, const char **step)
{
    for (uint32_t n = 0; n < total; ++n) {
        struct draw_binding *plan = &d->bindings[n];
        const size_t range = (size_t)plan->buffer.range;
        // The checked raw map/log already have offsets and are uploaded separately.
        if (!range || (!plan->data && plan->size)) continue;
        if (plan->gmem_alias) {
            VkResult r = gmem_plan_buffer(d, plan, step);
            if (r != VK_SUCCESS) return r;
        }
        if (plan->texel_format != VK_FORMAT_UNDEFINED && !plan->buffer.buffer)
            *usage |= plan->storage_texel ? VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT :
                                           VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
        if (plan->gmem_alias) continue;

        struct draw_binding *alias = NULL;
        for (uint32_t before = 0; before < n; ++before) {
            struct draw_binding *other = &d->bindings[before];
            if (plan->size && (compute || plan->storage == other->storage) && other->data == plan->data &&
                other->size == plan->size && other->buffer.range == range)
                alias = other;
            else if ((plan->writeback || other->writeback) && overlap(plan->data, plan->size, other->data, other->size))
                return failure(step, "overlapping writable draw buffer spans", VK_ERROR_FEATURE_NOT_PRESENT);
        }
        if (alias) {
            plan->buffer = alias->buffer;
            if (plan->writeback) alias->writeback = plan->writeback;
            plan->data = NULL; // One upload and writeback for the shared arena bytes.
            plan->writeback = NULL;
            continue;
        }
        VkDeviceSize alignment = gmem_binding_alignment(d, plan);
        if (compute) {
            // Aliases of a span can use it as a UBO, SSBO or texel buffer.
            if (alignment < d->limits.minStorageBufferOffsetAlignment) alignment = d->limits.minStorageBufferOffsetAlignment;
            if (alignment < d->limits.minUniformBufferOffsetAlignment) alignment = d->limits.minUniformBufferOffsetAlignment;
            if (alignment < d->limits.minTexelBufferOffsetAlignment) alignment = d->limits.minTexelBufferOffsetAlignment;
        }
        if (!reserve(used, alignment, range, &plan->buffer.offset))
            return failure(step, "draw staging buffer size overflow", VK_ERROR_OUT_OF_HOST_MEMORY);
    }
    return VK_SUCCESS;
}

static VkResult prepare_descriptor_sets(struct dm_m4_draw *d, const struct dm_m4_draw_stage *stages,
                                        uint32_t stage_count, const uint32_t counts[8], VkDescriptorSet *sets,
                                        const char **step)
{
    const VkDevice vk = d->device->device;
    int grow = !d->descriptor_pool;
    for (uint32_t i = 0; i < 8; ++i) if (counts[i] > d->descriptor_counts[i]) grow = 1;
    VkResult r;
    if (grow) {
        VkDescriptorPoolSize sizes[8];
        uint32_t n = 0;
        for (uint32_t i = 0; i < 8; ++i) {
            uint32_t count = counts[i] > d->descriptor_counts[i] ? counts[i] : d->descriptor_counts[i];
            if (count) sizes[n++] = (VkDescriptorPoolSize){(VkDescriptorType)i, count};
        }
        const VkDescriptorPoolCreateInfo ci = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = d->task_shader ? 3u : 2u,
            .poolSizeCount = n, .pPoolSizes = sizes,
        };
        VkDescriptorPool next = VK_NULL_HANDLE;
        r = d->CreateDescriptorPool(vk, &ci, NULL, &next);
        if (r != VK_SUCCESS) return failure(step, "allocate staged draw descriptor pool", r);
        if (d->descriptor_pool) d->DestroyDescriptorPool(vk, d->descriptor_pool, NULL);
        d->descriptor_pool = next;
        for (uint32_t i = 0; i < n; ++i) d->descriptor_counts[sizes[i].type] = sizes[i].descriptorCount;
    } else {
        r = d->ResetDescriptorPool(vk, d->descriptor_pool, 0);
        if (r != VK_SUCCESS) return failure(step, "reset draw descriptor pool", r);
    }
    VkDescriptorSetLayout layouts[3];
    for (uint32_t s = 0; s < stage_count; ++s) layouts[s] = stages[s].shader->set_layout;
    const VkDescriptorSetAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = d->descriptor_pool,
        .descriptorSetCount = stage_count, .pSetLayouts = layouts,
    };
    r = d->AllocateDescriptorSets(vk, &ai, sets);
    if (r != VK_SUCCESS) return failure(step, "allocate staged descriptor sets", r);
    uint32_t n = 0, write = 0;
    for (uint32_t s = 0; s < stage_count; ++s) {
        const struct dm_m4_shader *sh = stages[s].shader;
        for (uint32_t i = 0; i < sh->binding_count; ++i, ++n) {
            const struct dm_m4_shader_binding *src = &sh->bindings[i];
            struct draw_binding *binding = &d->bindings[n];
            const int direct = dm_m4_shader_direct(src);
            if (direct && !binding->descriptor_count) continue;
            if (!binding->buffer.buffer) binding->buffer.buffer = d->buffer;
            d->writes[write++] = (VkWriteDescriptorSet){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[s], .dstBinding = src->binding,
                .descriptorCount = direct ? binding->descriptor_count : 1u, .descriptorType = src->descriptor_type,
                .pImageInfo = direct ? &d->array_infos[binding->first_descriptor] : &binding->image,
                .pBufferInfo = &binding->buffer, .pTexelBufferView = &binding->texel_view,
            };
        }
    }
    if (write) d->UpdateDescriptorSets(vk, write, d->writes, 0, NULL);
    return VK_SUCCESS;
}

static VkResult reset_recording(struct dm_m4_draw *d, const char **step)
{
    if (d->queue_error) return failure(step, "Vulkan replay queue stopped after a failed submission", d->queue_error);
    VkResult r = d->ResetCommandBuffer(d->cmd, 0);
    return r == VK_SUCCESS ? r : failure(step, "reset staged Vulkan command buffer", r);
}

static VkResult begin_recording(struct dm_m4_draw *d, const char **step)
{
    const VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult r = d->BeginCommandBuffer(d->cmd, &begin);
    return r == VK_SUCCESS ? r : failure(step, "begin staged Vulkan commands", r);
}

static VkResult finish_recording(struct dm_m4_draw *d, VkDeviceSize used,
                                 VkPipelineStageFlags write_stages, VkAccessFlags write_access, const char **step)
{
    if (write_access) {
        const VkMemoryBarrier host_read = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = write_access, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        };
        d->CmdPipelineBarrier(d->cmd, write_stages, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host_read, 0, NULL, 0, NULL);
    }
    VkResult r = d->EndCommandBuffer(d->cmd);
    if (r != VK_SUCCESS) return failure(step, "end staged Vulkan commands", r);
    const VkDevice vk = d->device->device;
    const VkMappedMemoryRange mapped = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = d->buffer_memory, .size = VK_WHOLE_SIZE,
    };
    if (used && !d->coherent) {
        r = d->FlushMappedMemoryRanges(vk, 1, &mapped);
        if (r != VK_SUCCESS) return failure(step, "flush staged Vulkan input", r);
    }
    r = d->ResetFences(vk, 1, &d->fence);
    if (r != VK_SUCCESS) return failure(step, "reset staged Vulkan fence", r);
    const VkSubmitInfo submission = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &d->cmd,
    };
    r = d->QueueSubmit(d->device->queue, 1, &submission, d->fence);
    if (r != VK_SUCCESS) {
        d->queue_error = r;
        return failure(step, "submit staged commands to Vulkan queue", r);
    }
    d->in_flight = 1;
    r = d->WaitForFences(vk, 1, &d->fence, VK_TRUE, UINT64_MAX);
    if (r != VK_SUCCESS) {
        d->queue_error = r;
        return failure(step, "wait for staged Vulkan fence", r);
    }
    d->in_flight = 0;
    if (write_access && !d->coherent) {
        r = d->InvalidateMappedMemoryRanges(vk, 1, &mapped);
        if (r != VK_SUCCESS) return failure(step, "invalidate staged Vulkan readback", r);
    }
    return VK_SUCCESS;
}

static int upload_buffers(struct dm_m4_draw *d, uint32_t count)
{
    int storage_write = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const struct draw_binding *binding = &d->bindings[i];
        storage_write |= binding->writeback != NULL || binding->bindless_map_binding != 0;
        if (binding->gmem_alias) continue;
        if (!binding->buffer.range || (!binding->data && binding->size)) continue;
        uint8_t *dst = d->mapped + binding->buffer.offset;
        if (binding->size) memcpy(dst, binding->data, binding->size);
        if (binding->buffer.range > binding->size) memset(dst + binding->size, 0, binding->buffer.range - binding->size);
    }
    return storage_write;
}

static void download_buffers(const struct dm_m4_draw *d, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        const struct draw_binding *binding = &d->bindings[i];
        if (!binding->writeback) continue;
        if (binding->gmem_alias) {
            const struct gmem_range *r = &d->gmem_ranges[binding->gmem_alias - 1];
            if (r->imported || r->dirty) continue;
        }
        memcpy(binding->writeback, d->mapped + binding->buffer.offset, binding->size);
    }
}

static VkResult bindless_faults(const struct dm_m4_draw *d, uint32_t count, const char **step)
{
    for (uint32_t i = 0; i < count; ++i) {
        const struct draw_binding *binding = &d->bindings[i];
        if (!binding->bindless_map_binding) continue;
        uint32_t fault;
        memcpy(&fault, d->mapped + binding->buffer.offset, sizeof fault);
        if (!fault) continue;
        fprintf(stderr, "[RED] NVN bindless descriptor binding %u accessed unmapped guest ID %u\n",
                binding->bindless_map_binding - 1u, fault - 1u);
        return failure(step, "shader accessed an unmapped bindless descriptor; staged writes were not published",
                       VK_ERROR_INITIALIZATION_FAILED);
    }
    return VK_SUCCESS;
}

static VkResult upload_samples(struct dm_m4_draw *d, VkPipelineStageFlags stages, const char **step)
{
    for (uint32_t i = 0; i < d->sample_count; ++i) {
        if (d->samples[i].resident) continue;
        VkResult r = upload_sample(d, &d->samples[i], stages, step);
        if (r != VK_SUCCESS) return r;
    }
    return VK_SUCCESS;
}

static void complete_samples(struct dm_m4_draw *d)
{
    for (uint32_t i = 0; i < d->sample_count; ++i)
        d->samples[i].resident = d->samples[i].snapshot != NULL;
}

static int within_scissor(const VkRect2D *s, VkExtent2D extent)
{
    return s->offset.x >= 0 && s->offset.y >= 0 && (uint32_t)s->offset.x <= extent.width &&
           (uint32_t)s->offset.y <= extent.height && s->extent.width <= extent.width - (uint32_t)s->offset.x &&
           s->extent.height <= extent.height - (uint32_t)s->offset.y;
}

struct draw_indices {
    VkDeviceSize offset;
    VkPrimitiveTopology topology;
    uint32_t count, source_size, staged_size;
    VkBool32 restart;
    int assemble;
};

static uint32_t index_restart_value(uint32_t bytes)
{
    return bytes == 1 ? UINT8_MAX : bytes == 2 ? UINT16_MAX : UINT32_MAX;
}

static uint32_t load_index(const uint8_t *p, uint32_t bytes)
{
    if (bytes == 1) return *p;
    if (bytes == 2) {
        uint16_t v;
        memcpy(&v, p, sizeof v);
        return v;
    }
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

static void store_index(uint8_t *p, uint32_t bytes, uint32_t index)
{
    if (bytes == 2) {
        const uint16_t v = (uint16_t)index;
        memcpy(p, &v, sizeof v);
    } else {
        memcpy(p, &index, sizeof index);
    }
}

static unsigned list_width(VkPrimitiveTopology topology)
{
    switch (topology) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return 1;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: return 2;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: return 3;
    default: return 0;
    }
}

// Assemble complete runs into one indexed list, keeping the draw's primitive
// numbering and provoking vertices. A NULL destination only measures it.
static uint64_t assemble_indices(const struct dm_m4_draw_info *info, const struct draw_indices *plan, uint8_t *dst)
{
    const uint8_t *src = info->indices.data;
    const uint32_t in = plan->source_size, out = plan->staged_size;
    const VkPrimitiveTopology topology = info->pipeline.topology;
    const unsigned width = list_width(topology);
    uint64_t used = 0;
    for (size_t first = 0; first < info->index_count;) {
        size_t end = first;
        while (end < info->index_count && load_index(src + end * in, in) != info->restart_index) ++end;
        const size_t count = end - first;
        if (width) {
            const size_t complete = count - count % width;
            if (dst && complete) {
                if (in == out) {
                    memcpy(dst + used * out, src + first * in, complete * in);
                } else {
                    for (size_t i = 0; i < complete; ++i)
                        store_index(dst + (used + i) * out, out, src[first + i]);
                }
            }
            used += complete;
        } else if (topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP && count >= 2) {
            if (dst) {
                uint32_t previous = load_index(src + first * in, in);
                for (size_t i = first + 1; i < end; ++i) {
                    const uint32_t v = load_index(src + i * in, in);
                    store_index(dst + used++ * out, out, previous);
                    store_index(dst + used++ * out, out, v);
                    previous = v;
                }
            } else {
                used += 2 * (uint64_t)(count - 1);
            }
        } else if (count >= 3) {
            if (dst) {
                const uint32_t head = load_index(src + first * in, in);
                uint32_t a = head, b = load_index(src + (first + 1) * in, in);
                for (size_t i = first + 2; i < end; ++i) {
                    const uint32_t c = load_index(src + i * in, in);
                    if (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN) {
                        store_index(dst + used++ * out, out, b);
                        store_index(dst + used++ * out, out, c);
                        store_index(dst + used++ * out, out, head);
                    } else {
                        const int odd = ((i - first) & 1u) != 0;
                        store_index(dst + used++ * out, out, a);
                        store_index(dst + used++ * out, out, odd ? c : b);
                        store_index(dst + used++ * out, out, odd ? b : c);
                    }
                    a = b;
                    b = c;
                }
            } else {
                used += 3 * (uint64_t)(count - 2);
            }
        }
        first = end + (end < info->index_count);
    }
    return used;
}

static VkResult prepare_indices(const struct dm_m4_draw *d, const struct dm_m4_draw_info *info,
                                struct draw_indices *plan, const char **step)
{
    uint32_t size;
    switch (info->index_type) {
    case VK_INDEX_TYPE_UINT8_EXT: size = 1; break;
    case VK_INDEX_TYPE_UINT16: size = 2; break;
    case VK_INDEX_TYPE_UINT32: size = 4; break;
    default: return failure(step, "unsupported draw index format", VK_ERROR_FEATURE_NOT_PRESENT);
    }
    if (!info->index_count || !info->instance_count || !info->indices.data || info->index_count > info->indices.size / size)
        return failure(step, "empty or truncated indexed geometry", VK_ERROR_INITIALIZATION_FAILED);
    plan->count = info->index_count;
    plan->source_size = size;
    plan->staged_size = size == 1 ? 2u : size;

    const uint32_t fixed = index_restart_value(size);
    const uint32_t marker = info->restart_index;
    const int restart = info->pipeline.primitive_restart_enable && marker <= fixed;
    const int list_fallback = list_width(plan->topology) && !d->device->primitive_topology_list_restart;
    // Byte indices already widen, so their real maximum cannot collide.
    const int inspect = restart && (list_fallback || (size != 1 && marker != fixed));
    const int check_range = size == 4 && !d->features.fullDrawIndexUint32;
    const uint32_t limit = d->limits.maxDrawIndexedIndexValue == UINT32_MAX ?
                           (UINT32_C(1) << 24) - 1u : d->limits.maxDrawIndexedIndexValue;
    int has_marker = restart && !inspect, has_fixed = 0;
    if (inspect || check_range) {
        const uint8_t *src = info->indices.data;
        for (size_t i = 0; i < info->index_count; ++i) {
            const uint32_t v = load_index(src + i * size, size);
            if (restart && v == marker) {
                has_marker = 1;
            } else {
                if (check_range && v > limit)
                    return failure(step, "non-restart 32-bit index exceeds the enabled device range",
                                   VK_ERROR_FEATURE_NOT_PRESENT);
                has_fixed |= v == fixed;
            }
            if (!check_range && has_marker && (list_fallback || has_fixed)) break;
        }
    }
    plan->restart = has_marker;
    if (!has_marker) return VK_SUCCESS;
    if (!list_fallback && !(size == 4 && has_fixed)) {
        if (size == 2 && has_fixed) plan->staged_size = 4;
        return VK_SUCCESS;
    }

    // Keeping the original indices and issuing one draw preserves VertexIndex,
    // BaseVertex, and PrimitiveId; splitting the runs into draws would not.
    if (!list_fallback) {
        switch (plan->topology) {
        case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
        case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
            break;
        case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
            plan->topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            break;
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
            plan->topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            break;
        default:
            return failure(step, "unsupported indexed primitive topology", VK_ERROR_FEATURE_NOT_PRESENT);
        }
    }
    plan->restart = VK_FALSE;
    plan->assemble = 1;
    const uint64_t count = assemble_indices(info, plan, NULL);
    if (count > UINT32_MAX)
        return failure(step, "restart fallback exceeds Vulkan's indexed draw count", VK_ERROR_FEATURE_NOT_PRESENT);
    plan->count = (uint32_t)count;
    return VK_SUCCESS;
}

static void bind_indices(struct dm_m4_draw *d, const struct dm_m4_draw_info *info, const struct draw_indices *plan)
{
    const uint8_t *src = info->indices.data;
    uint8_t *dst = d->mapped + plan->offset;
    const uint32_t in = plan->source_size, out = plan->staged_size;
    const uint32_t marker = info->restart_index, fixed = index_restart_value(out);
    if (plan->assemble) {
        assemble_indices(info, plan, dst);
    } else if (in == out && (!plan->restart || marker == fixed)) {
        memcpy(dst, src, (size_t)plan->count * in);
    } else if (in == 1 && !plan->restart) {
        uint16_t *words = (uint16_t *)dst;
        for (size_t i = 0; i < plan->count; ++i) words[i] = src[i];
    } else {
        for (size_t i = 0; i < plan->count; ++i) {
            const uint32_t v = load_index(src + i * in, in);
            store_index(dst + i * out, out, v == marker ? fixed : v);
        }
    }
    d->CmdBindIndexBuffer(d->cmd, d->buffer, plan->offset, out == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

VkResult dm_m4_draw_submit(struct dm_m4_draw *d, const struct dm_m4_draw_info *info, const char **step)
{
    if (step) *step = NULL;
    if (info && info->attachment_writes) *info->attachment_writes = (struct dm_m4_draw_attachment_writes){0};
    if (!d || !info) return failure(step, "draw submit arguments", VK_ERROR_INITIALIZATION_FAILED);
    VkResult r = reset_recording(d, step);
    if (r != VK_SUCCESS) return r;
    const int mesh = info->ms.shader != NULL;
    const int indexed = !mesh && !info->vertex_count;
    const struct dm_m4_draw_stage primary = mesh ? info->ms : info->vs;
    if (!stage_ok(&primary, mesh ? VK_SHADER_STAGE_MESH_BIT_EXT : VK_SHADER_STAGE_VERTEX_BIT, 0) ||
        (info->ps.shader && !stage_ok(&info->ps, VK_SHADER_STAGE_FRAGMENT_BIT, 1)) ||
        (info->ts.shader && (!mesh || !stage_ok(&info->ts, VK_SHADER_STAGE_TASK_BIT_EXT, 2))) ||
        (mesh && info->vs.shader))
        return failure(step, "draw stage or descriptor set does not match its pipeline", VK_ERROR_INVALID_SHADER_NV);
    uint32_t group_limit = 0;
    if (mesh) {
        if (!info->task_count) return VK_SUCCESS;
        if (info->task_count - 1u > UINT32_MAX - info->first_task)
            return failure(step, "mesh draw launch range overflow", VK_ERROR_INITIALIZATION_FAILED);
        group_limit = mesh_group_limit(d, info);
        if (!group_limit) return failure(step, "native mesh or task launch unavailable", VK_ERROR_FEATURE_NOT_PRESENT);
    }
    struct draw_indices index = {.topology = info->pipeline.topology};
    if (indexed) {
        r = prepare_indices(d, info, &index, step);
        if (r != VK_SUCCESS || !index.count) return r;
    }
    const uint32_t color_count = info->color_count;
    const int depth = info->depth.format != VK_FORMAT_UNDEFINED;
    const VkExtent2D extent = info->extent;
    if (color_count > DM_M4_NVN_COLOR_TARGETS || color_count > d->limits.maxColorAttachments ||
        (color_count && info->colors[color_count - 1].format == VK_FORMAT_UNDEFINED))
        return failure(step, "draw color slots exceed limits or end in an unbound slot", VK_ERROR_FEATURE_NOT_PRESENT);
    if (!extent.width || !extent.height || extent.width > d->limits.maxFramebufferWidth ||
        extent.height > d->limits.maxFramebufferHeight || !within_scissor(&info->scissor, extent))
        return failure(step, "draw render extent or scissor is outside device bounds", VK_ERROR_FEATURE_NOT_PRESENT);
    if (!color_count && !depth && !(d->limits.framebufferNoAttachmentsSampleCounts & VK_SAMPLE_COUNT_1_BIT))
        return failure(step, "attachmentless draws at one sample are not supported", VK_ERROR_FEATURE_NOT_PRESENT);
    VkDeviceSize used = 0;
    struct draw_transfer rate = {0};
    if (info->rate_image) {
        r = prepare_rate_image(d, info->rate_image, &used, &rate, step);
        if (r != VK_SUCCESS) return r;
    }
    struct dm_m4_draw_stage stages[3] = {primary};
    uint32_t stage_count = 1;
    if (info->ps.shader) stages[stage_count++] = info->ps;
    if (info->ts.shader) stages[stage_count++] = info->ts;
    uint32_t counts[8] = {0}, total = 0, gmem = 0;
    for (uint32_t i = 0; i < stage_count; ++i) {
        if (stages[i].shader->binding_count > UINT32_MAX - total)
            return failure(step, "draw descriptor count overflow", VK_ERROR_OUT_OF_HOST_MEMORY);
        total += stages[i].shader->binding_count;
        gmem |= stages[i].shader->gmem;
    }
    r = gmem_prepare(d, &info->gmem, gmem, 0, &used, step);
    if (r != VK_SUCCESS) return r;
    struct sampled_types numeric[3] = {0};
    r = prepare_bindings(d, stages, stage_count, &info->support, &used, counts, total, numeric, step);
    if (r != VK_SUCCESS) return r;
    // Validate aggregate limits before creating the graphics pipeline layout.
    if (counts[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER] > d->limits.maxDescriptorSetUniformBuffers ||
        counts[VK_DESCRIPTOR_TYPE_STORAGE_BUFFER] > d->limits.maxDescriptorSetStorageBuffers ||
        (uint64_t)counts[VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER] + counts[VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE] >
            d->limits.maxDescriptorSetSampledImages ||
        (uint64_t)counts[VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER] + counts[VK_DESCRIPTOR_TYPE_SAMPLER] >
            d->limits.maxDescriptorSetSamplers)
        return failure(step, "draw descriptor counts exceed device limits", VK_ERROR_FEATURE_NOT_PRESENT);
    VkBufferUsageFlags usage = 0;
    r = gmem_backing(d, &info->gmem, total, &used, &usage, step);
    if (r != VK_SUCCESS) return r;
    r = plan_buffer_bindings(d, total, 0, &used, &usage, step);
    if (r != VK_SUCCESS) return r;
    struct draw_pipeline *pipeline;
    r = get_pipeline(d, info, index.topology, index.restart, numeric, &pipeline, step);
    if (r != VK_SUCCESS) return r;
    // An empty scissor rejects samples before any depth/stencil operations.
    // Still issue the draw for pre-raster and shader storage side effects.
    const int rasterize = !info->pipeline.rasterization->rasterizerDiscardEnable &&
                          info->scissor.extent.width && info->scissor.extent.height;
    struct dm_m4_draw_attachment_writes writes = {0};
    if (rasterize) writes = target_writes(info);
    struct draw_target colors[DM_M4_NVN_COLOR_TARGETS], z;
    for (uint32_t i = 0; i < color_count; ++i) {
        const struct dm_m4_draw_attachment *src = &info->colors[i];
        if (src->format == VK_FORMAT_UNDEFINED) continue;
        if (src->width < extent.width || src->height < extent.height)
            return failure(step, "color attachment is smaller than render area", VK_ERROR_INITIALIZATION_FAILED);
        const int blend = info->pipeline.blend->pAttachments[i].blendEnable;
        const VkImageAspectFlags store = (writes.colors & (1u << i)) ? VK_IMAGE_ASPECT_COLOR_BIT : 0;
        r = prepare_target(d, &d->colors[i], src, 0, blend, 0, store, &used, &colors[i], step);
        if (r != VK_SUCCESS) return r;
    }
    if (depth) {
        if (info->depth.width < extent.width || info->depth.height < extent.height)
            return failure(step, "depth/stencil attachment is smaller than render area", VK_ERROR_INITIALIZATION_FAILED);
        const VkImageAspectFlags load = rasterize ? depth_stencil_load(info, writes.depth_stencil) : 0;
        r = prepare_target(d, &d->depth, &info->depth, 1, 0, load, writes.depth_stencil, &used, &z, step);
        if (r != VK_SUCCESS) return r;
    }
    VkDeviceSize vertex_offsets[DM_M4_NVN_VERTEX_SLOTS];
    const VkPipelineVertexInputStateCreateInfo *v = info->pipeline.vertex_input;
    if (indexed && !reserve(&used, index.staged_size, (VkDeviceSize)index.count * index.staged_size, &index.offset))
        return failure(step, "indexed geometry staging size overflow", VK_ERROR_OUT_OF_HOST_MEMORY);
    if (!mesh) {
        if (!info->instance_count || info->instance_count - 1u > UINT32_MAX - info->first_instance)
            return failure(step, "draw instance range overflow", VK_ERROR_INITIALIZATION_FAILED);
        for (uint32_t i = 0; i < v->vertexBindingDescriptionCount; ++i) {
            const uint32_t slot = v->pVertexBindingDescriptions[i].binding;
            const struct dm_m4_draw_span *src = &info->vertices[slot];
            const size_t tail = info->vertex_zero_tail[slot];
            if ((src->size && !src->data) || (!src->size && !tail))
                return failure(step, "missing draw vertex stream span", VK_ERROR_INITIALIZATION_FAILED);
            if (tail > SIZE_MAX - src->size || !reserve(&used, 16, src->size + tail, &vertex_offsets[i]))
                return failure(step, "vertex geometry staging size overflow", VK_ERROR_OUT_OF_HOST_MEMORY);
        }
    }
    r = ensure_buffer(d, used, usage, step);
    if (r != VK_SUCCESS) return r;
    VkDescriptorSet sets[3];
    r = prepare_descriptor_sets(d, stages, stage_count, counts, sets, step);
    if (r != VK_SUCCESS) return r;
    uint32_t gmem_entries;
    r = gmem_upload(d, &info->gmem, gmem, &gmem_entries, step);
    if (r != VK_SUCCESS) return r;
    if (info->samples_passed && !d->occlusion) {
        const VkQueryPoolCreateInfo query = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 1};
        r = d->CreateQueryPool(d->device->device, &query, NULL, &d->occlusion);
        if (r != VK_SUCCESS) return failure(step, "create samples-passed query", r);
    }
    r = begin_recording(d, step);
    if (r != VK_SUCCESS) return r;
    if (info->samples_passed) d->CmdResetQueryPool(d->cmd, d->occlusion, 0, 1);
    const int storage_write = upload_buffers(d, total) || d->gmem_writes;
    if (indexed) bind_indices(d, info, &index);
    if (!mesh) {
        for (uint32_t i = 0; i < v->vertexBindingDescriptionCount; ++i) {
            const uint32_t slot = v->pVertexBindingDescriptions[i].binding;
            const struct dm_m4_draw_span *src = &info->vertices[slot];
            uint8_t *dst = d->mapped + vertex_offsets[i];
            if (src->size) memcpy(dst, src->data, src->size);
            if (info->vertex_zero_tail[slot]) memset(dst + src->size, 0, info->vertex_zero_tail[slot]);
            d->CmdBindVertexBuffers(d->cmd, slot, 1, &d->buffer, &vertex_offsets[i]);
        }
    }
    const VkPipelineStageFlags shaders = (mesh ? VK_PIPELINE_STAGE_MESH_SHADER_BIT_EXT : VK_PIPELINE_STAGE_VERTEX_SHADER_BIT) |
        (info->ts.shader ? VK_PIPELINE_STAGE_TASK_SHADER_BIT_EXT : 0) |
        (info->ps.shader ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : 0);
    r = upload_samples(d, shaders, step);
    if (r != VK_SUCCESS) return r;
    if (info->rate_image) bind_rate_image(d, info->rate_image, &rate);
    VkRenderingAttachmentInfoKHR color_info[DM_M4_NVN_COLOR_TARGETS];
    for (uint32_t i = 0; i < color_count; ++i) {
        if (info->colors[i].format == VK_FORMAT_UNDEFINED) {
            color_info[i] = (VkRenderingAttachmentInfoKHR){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
            continue;
        }
        upload_target(d, &info->colors[i], &colors[i]);
        color_info[i] = render_target(&colors[i], VK_IMAGE_ASPECT_COLOR_BIT);
    }
    const int have_z = depth && info->depth.format != VK_FORMAT_S8_UINT;
    const int have_stencil = depth && has_stencil(info->depth.format);
    VkRenderingAttachmentInfoKHR depth_info, stencil_info;
    if (depth) upload_target(d, &info->depth, &z);
    if (have_z) depth_info = render_target(&z, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (have_stencil) stencil_info = render_target(&z, VK_IMAGE_ASPECT_STENCIL_BIT);
    const VkRenderingInfoKHR rendering = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO_KHR, .renderArea = {{0, 0}, extent},
        .layerCount = 1, .colorAttachmentCount = color_count, .pColorAttachments = color_count ? color_info : NULL,
        .pDepthAttachment = have_z ? &depth_info : NULL,
        .pStencilAttachment = have_stencil ? &stencil_info : NULL,
    };
    d->CmdBeginRenderingKHR(d->cmd, &rendering);
    if (info->samples_passed) d->CmdBeginQuery(d->cmd, d->occlusion, 0, VK_QUERY_CONTROL_PRECISE_BIT);
    d->CmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline);
    const int gap = info->ts.shader && !info->ps.shader;
    d->CmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout, 0,
                             gap ? 1u : stage_count, sets, 0, NULL);
    if (gap) d->CmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->layout, 2, 1, &sets[1], 0, NULL);
    const uint32_t viewport_count = info->pipeline.viewport_count;
    if (viewport_count == 1) {
        d->CmdSetViewport(d->cmd, 0, 1, &info->viewport);
        d->CmdSetScissor(d->cmd, 0, 1, &info->scissor);
    } else {
        VkViewport viewports[DM_M4_RATE_MAX_PALETTES];
        VkRect2D scissors[DM_M4_RATE_MAX_PALETTES];
        for (uint32_t i = 0; i < viewport_count; ++i) {
            viewports[i] = info->viewport;
            scissors[i] = info->scissor;
        }
        d->CmdSetViewport(d->cmd, 0, viewport_count, viewports);
        d->CmdSetScissor(d->cmd, 0, viewport_count, scissors);
    }
    if (mesh) {
        const VkShaderStageFlagBits launch = mesh_launch_stage(info);
        uint32_t remaining = info->task_count, first = info->first_task;
        // Vk has no firstTask parameter. Rebase only the launched stage's
        // WorkgroupId, including subsequent chunks above the device's X limit.
        do {
            const uint32_t n = remaining < group_limit ? remaining : group_limit;
            d->CmdPushConstants(d->cmd, pipeline->layout, launch, 0, sizeof(first), &first);
            d->CmdDrawMeshTasksEXT(d->cmd, n, 1, 1);
            first += n;
            remaining -= n;
        } while (remaining);
    } else if (info->vertex_count) {
        d->CmdDraw(d->cmd, info->vertex_count, info->instance_count, (uint32_t)info->base_vertex, info->first_instance);
    } else {
        d->CmdDrawIndexed(d->cmd, index.count, info->instance_count, 0, info->base_vertex, info->first_instance);
    }
    if (info->samples_passed) d->CmdEndQuery(d->cmd, d->occlusion, 0);
    d->CmdEndRenderingKHR(d->cmd);
    for (uint32_t i = 0; i < color_count; ++i)
        if (writes.colors & (1u << i)) readback_target(d, &colors[i]);
    if (writes.depth_stencil) readback_target(d, &z);
    const int image_write = writes.colors || writes.depth_stencil;
    r = finish_recording(d, used, (image_write ? VK_PIPELINE_STAGE_TRANSFER_BIT : 0) | (storage_write ? shaders : 0),
                         (image_write ? VK_ACCESS_TRANSFER_WRITE_BIT : 0) | (storage_write ? VK_ACCESS_SHADER_WRITE_BIT : 0), step);
    if (r != VK_SUCCESS) return r;
    r = bindless_faults(d, total, step);
    if (r != VK_SUCCESS) return r;
    if (info->attachment_writes) {
        // Compare the fenced readback against the original inputs before publishing any CPU downloads.
        for (uint32_t i = 0; i < color_count; ++i) {
            const uint32_t bit = 1u << i;
            if ((writes.colors & bit) && !changed_target_aspects(d, info, &info->colors[i], &colors[i],
                                                                 writes.colors & (bit - 1u)))
                writes.colors &= ~bit;
        }
        if (writes.depth_stencil)
            writes.depth_stencil = changed_target_aspects(d, info, &info->depth, &z, writes.colors);
    }
    r = gmem_download(d, &info->gmem, gmem_entries, step);
    if (r != VK_SUCCESS) return r;
    if (info->samples_passed) {
        uint64_t samples;
        r = d->GetQueryPoolResults(d->device->device, d->occlusion, 0, 1, sizeof(samples),
                                  &samples, sizeof(samples), VK_QUERY_RESULT_64_BIT);
        if (r != VK_SUCCESS) return failure(step, "read fenced samples-passed query", r);
        *info->samples_passed += samples;
    }
    for (uint32_t i = 0; i < color_count; ++i)
        if (writes.colors & (1u << i)) download_target(d, &info->colors[i], &colors[i], VK_IMAGE_ASPECT_COLOR_BIT);
    if (writes.depth_stencil) download_target(d, &info->depth, &z, writes.depth_stencil);
    if (storage_write) download_buffers(d, total);
    for (uint32_t i = 0; i < color_count; ++i)
        if (info->colors[i].format != VK_FORMAT_UNDEFINED) complete_target(&info->colors[i], &colors[i]);
    if (depth) complete_target(&info->depth, &z);
    if (info->attachment_writes) *info->attachment_writes = writes;
    complete_samples(d);
    return VK_SUCCESS;
}
