static int32_t sampled_format_sign(VkFormat format)
{
    // These packed and BGR variants can be sampled as well as the NVN color formats.
    switch (format) {
    case VK_FORMAT_B8G8R8_UINT: case VK_FORMAT_B8G8R8A8_UINT:
    case VK_FORMAT_A8B8G8R8_UINT_PACK32: case VK_FORMAT_A2R10G10B10_UINT_PACK32:
        return 1;
    case VK_FORMAT_B8G8R8_SINT: case VK_FORMAT_B8G8R8A8_SINT:
    case VK_FORMAT_A8B8G8R8_SINT_PACK32: case VK_FORMAT_A2R10G10B10_SINT_PACK32:
        return -1;
    default:
        break;
    }
    const enum dm_m4_color_type type = dm_m4_attachment_color_type(format);
    return (type == DM_M4_COLOR_UINT) - (type == DM_M4_COLOR_SINT);
}

static void destroy_numeric_variant(struct dm_m4_draw *d, struct numeric_variant *variant)
{
    d->DestroyShaderModule(d->device->device, variant->module, NULL);
    free(variant);
}

static void evict_numeric_variant(struct dm_m4_draw *d, struct numeric_variant *variant)
{
    const VkShaderModule module = variant->module;
    // Submits are fenced. Remove pipeline keys before Vulkan can reuse this handle.
    for (size_t i = 0; i < PIPELINE_CACHE_SIZE; ++i) {
        struct draw_pipeline *p = &d->pipelines[i];
        if (p->pipeline && (p->key.primary == module || p->key.ps == module || p->key.task == module))
            destroy_pipeline(d, p);
    }
    for (size_t i = 0; i < COMPUTE_PIPELINE_CACHE_SIZE; ++i) {
        struct compute_pipeline *p = &d->compute_pipelines[i];
        if (p->shader == module)
            destroy_compute_pipeline(d, p);
    }
    destroy_numeric_variant(d, variant);
}

static VkResult append_sampled_type(struct dm_m4_draw *d, const struct dm_m4_shader *shader,
                                    uint32_t *count, uint32_t binding, int32_t sign, const char **step)
{
    if (!shader->numeric_code || !shader->numeric_words)
        return failure(step, "integer sampled image requires retained shader code", VK_ERROR_FEATURE_NOT_PRESENT);
    if (*count == d->numeric_type_capacity) {
        void *scratch = d->numeric_types;
        VkResult r = grow_array(&scratch, sizeof *d->numeric_types, *count + 1, &d->numeric_type_capacity, 0);
        if (r != VK_SUCCESS) return failure(step, "allocate sampled shader type signature", r);
        d->numeric_types = scratch;
    }
    d->numeric_types[(*count)++] = (struct dm_m4_sample_type){binding, sign};
    return VK_SUCCESS;
}

static VkResult sampled_module(struct dm_m4_draw *d, const struct dm_m4_shader *shader,
                               struct sampled_types types, VkShaderModule *out, const char **step)
{
    *out = shader ? shader->module : VK_NULL_HANDLE;
    const uint32_t count = types.count;
    if (!count) return VK_SUCCESS;
    const struct dm_m4_sample_type *signature = d->numeric_types + types.first;

    const size_t bytes = (size_t)count * sizeof *d->numeric_types;
    uint32_t oldest = 0;
    for (uint32_t i = 0; i < d->numeric_variant_count; ++i) {
        struct numeric_variant *variant = d->numeric_variants[i];
        if (variant->base == shader->module && variant->set == shader->set_layout && variant->count == count &&
            !memcmp(variant + 1, signature, bytes)) {
            variant->used = ++d->serial;
            *out = variant->module;
            return VK_SUCCESS;
        }
        if (variant->used < d->numeric_variants[oldest]->used) oldest = i;
    }

    uint32_t *code;
    size_t words;
    VkResult r = dm_m4_shader_numeric_code(shader->numeric_code, shader->numeric_words,
                                           signature, count, &code, &words);
    if (r != VK_SUCCESS) return failure(step, "specialize fixed sampled image numeric type", r);
    struct numeric_variant *next = bytes <= SIZE_MAX - sizeof *next ? malloc(sizeof *next + bytes) : NULL;
    if (!next) {
        free(code);
        return failure(step, "allocate sampled shader variant", VK_ERROR_OUT_OF_HOST_MEMORY);
    }
    const VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words * sizeof *code, .pCode = code,
    };
    r = d->CreateShaderModule(d->device->device, &ci, NULL, &next->module);
    free(code);
    if (r != VK_SUCCESS) {
        free(next);
        return failure(step, "create fixed sampled shader variant", r);
    }
    next->base = shader->module;
    next->set = shader->set_layout;
    next->used = ++d->serial;
    next->count = count;
    memcpy(next + 1, signature, bytes);
    if (d->numeric_variant_count == NUMERIC_VARIANT_CACHE_SIZE)
        evict_numeric_variant(d, d->numeric_variants[oldest]);
    else
        oldest = d->numeric_variant_count++;
    d->numeric_variants[oldest] = next;
    *out = next->module;
    return VK_SUCCESS;
}
