// Included by m4_shader.c. Only fixed UniformConstant combined samplers are
// specialized; SSA IDs seen by the rest of the translated shader keep their
// original types, except for the sampler/image flow itself.
enum {
    NUM_EXT_INST = 12, NUM_LINE = 8, NUM_NO_LINE = 317,
    NUM_TYPE_INT = 21, NUM_TYPE_FLOAT = 22, NUM_TYPE_VECTOR = 23,
    NUM_TYPE_IMAGE = 25, NUM_TYPE_SAMPLED = 27, NUM_TYPE_POINTER = 32,
    NUM_FUNCTION = 54, NUM_FUNCTION_END = 56, NUM_VARIABLE = 59,
    NUM_LOAD = 61, NUM_STORE = 62, NUM_COPY_MEMORY = 63, NUM_COPY_MEMORY_SIZED = 64,
    NUM_ARRAY_LENGTH = 68, NUM_DECORATE = 71, NUM_VECTOR_SHUFFLE = 79,
    NUM_COMPOSITE_EXTRACT = 81, NUM_COMPOSITE_INSERT = 82, NUM_COPY_OBJECT = 83,
    NUM_SAMPLE = 87, NUM_SAMPLE_LOD = 88, NUM_SAMPLE_DREF = 89, NUM_SAMPLE_DREF_LOD = 90,
    NUM_SAMPLE_PROJ = 91, NUM_SAMPLE_PROJ_LOD = 92,
    NUM_SAMPLE_PROJ_DREF = 93, NUM_SAMPLE_PROJ_DREF_LOD = 94,
    NUM_FETCH = 95, NUM_GATHER = 96, NUM_GATHER_DREF = 97,
    NUM_IMAGE_READ = 98, NUM_IMAGE_WRITE = 99, NUM_IMAGE = 100,
    NUM_QUERY_SIZE_LOD = 103, NUM_QUERY_SIZE = 104, NUM_QUERY_LOD = 105,
    NUM_QUERY_LEVELS = 106, NUM_QUERY_SAMPLES = 107, NUM_BITCAST = 124,
    NUM_LOOP_MERGE = 246, NUM_SELECTION_MERGE = 247, NUM_BRANCH_COND = 250, NUM_SWITCH = 251,
    NUM_SPARSE_SAMPLE = 305, NUM_SPARSE_GATHER_DREF = 315, NUM_SPARSE_READ = 320,
    NUM_BALLOT_BIT_COUNT = 342, NUM_GROUP_IADD = 349, NUM_GROUP_XOR = 364,
    NUM_UNTYPED_VARIABLE = 4418, NUM_EXT_INST_FORWARD = 4433,
    NUM_BINDING = 33, NUM_DESCRIPTOR_SET = 34, NUM_UNIFORM_CONSTANT = 0,
    NUM_MAX_BOUND = 0x3fffff,
    NUM_IMAGE_EXTEND = 0x3000, // SignExtend or ZeroExtend
    NUM_POINTER = 1, NUM_SAMPLED = 2, NUM_PLAIN_IMAGE = 3,
    // 1D through Buffer, three Depth values, Arrayed and MS. Sampled=1,
    // format=Unknown, and no access qualifier for these Vulkan descriptors.
    NUM_IMAGE_SHAPES = 6 * 3 * 2 * 2,
};

struct numeric_id {
    uint32_t decl, set_plus_one;
    uint8_t kind, shape, sign;
};

struct numeric_types {
    uint32_t scalar, vec4;
    uint32_t image[NUM_IMAGE_SHAPES][3]; // indexed by NUM_POINTER/SAMPLED/PLAIN_IMAGE - 1
};

struct numeric_module {
    const uint32_t *src;
    size_t words, size, first_function;
    uint32_t bound, next_id, texels;
    struct numeric_id *ids;
    struct numeric_types types[2]; // OpTypeInt Signedness operand
};

static uint32_t numeric_op(const uint32_t *p) { return p[0] & 0xffffu; }
static uint32_t numeric_len(const uint32_t *p) { return p[0] >> 16; }
static uint32_t numeric_inst(uint32_t op, uint32_t len) { return (len << 16) | op; }

static int numeric_valid_id(const struct numeric_module *m, uint32_t id)
{
    return id != 0 && id < m->bound;
}

static const uint32_t *numeric_decl(const struct numeric_module *m, uint32_t id, uint32_t op)
{
    if (!numeric_valid_id(m, id) || !m->ids[id].decl)
        return NULL;
    const uint32_t *p = m->src + m->ids[id].decl;
    return numeric_op(p) == op ? p : NULL;
}
static const struct numeric_id *numeric_flow(const struct numeric_module *m, uint32_t id)
{
    return numeric_valid_id(m, id) && m->ids[id].kind ? &m->ids[id] : NULL;
}

static int numeric_shape(const uint32_t *image)
{
    if (!image || numeric_len(image) != 9 || image[3] >= 6 || image[4] > 2 ||
        image[5] > 1 || image[6] > 1 || image[7] != 1 || image[8] != 0)
        return -1;
    return (int)(((image[3] * 3 + image[4]) * 2 + image[5]) * 2 + image[6]);
}

static uint32_t numeric_find_binding(const struct dm_m4_sample_type *types, uint32_t count,
                                     uint32_t binding)
{
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (types[mid].binding < binding)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < count && types[lo].binding == binding ? lo : count;
}

// Index just the declarations needed for the rewrites and intern structurally
// identical types already present anywhere in the original global declarations.
static int numeric_index(struct numeric_module *m, const struct dm_m4_sample_type *types,
                         uint32_t count, uint32_t *targets)
{
    for (size_t at = 5; at < m->first_function; ) {
        const uint32_t *p = m->src + at;
        const uint32_t op = numeric_op(p), len = numeric_len(p);
        uint32_t id;
        switch (op) {
        case NUM_DECORATE:
            if (len < 3 || !numeric_valid_id(m, p[1]))
                return 0;
            if (p[2] == NUM_BINDING || p[2] == NUM_DESCRIPTOR_SET) {
                if (len != 4)
                    return 0;
                if (p[2] == NUM_BINDING) {
                    const uint32_t i = numeric_find_binding(types, count, p[3]);
                    if (i < count) {
                        if (targets[i])
                            return 0;
                        targets[i] = p[1];
                    }
                } else {
                    if (p[3] == UINT32_MAX || m->ids[p[1]].set_plus_one)
                        return 0;
                    m->ids[p[1]].set_plus_one = p[3] + 1;
                }
            }
            at += len;
            continue;
        case NUM_TYPE_INT: if (len != 4 || p[3] > 1) return 0; break;
        case NUM_TYPE_FLOAT: if (len < 3 || len > 4) return 0; break;
        case NUM_TYPE_VECTOR: case NUM_TYPE_POINTER: if (len != 4) return 0; break;
        case NUM_TYPE_IMAGE: if (len != 9 && len != 10) return 0; break;
        case NUM_TYPE_SAMPLED: if (len != 3) return 0; break;
        case NUM_VARIABLE: if (len < 4 || len > 5) return 0; break;
        default: at += len; continue;
        }
        id = op == NUM_VARIABLE ? p[2] : p[1];
        if (!numeric_valid_id(m, id) || m->ids[id].decl)
            return 0;
        m->ids[id].decl = (uint32_t)at;
        if (op == NUM_TYPE_INT && p[2] == 32) {
            if (m->types[p[3]].scalar)
                return 0;
            m->types[p[3]].scalar = id;
        } else if (op == NUM_TYPE_VECTOR && p[3] == 4) {
            for (uint32_t sign = 0; sign < 2; ++sign) {
                if (m->types[sign].scalar && p[2] == m->types[sign].scalar) {
                    if (m->types[sign].vec4)
                        return 0;
                    m->types[sign].vec4 = id;
                }
            }
        } else if (op == NUM_TYPE_IMAGE || op == NUM_TYPE_SAMPLED ||
                   (op == NUM_TYPE_POINTER && p[2] == NUM_UNIFORM_CONSTANT)) {
            const uint32_t *sampled = op == NUM_TYPE_POINTER ? numeric_decl(m, p[3], NUM_TYPE_SAMPLED) : p;
            const uint32_t *image = op == NUM_TYPE_IMAGE ? p :
                (sampled ? numeric_decl(m, sampled[2], NUM_TYPE_IMAGE) : NULL);
            const int shape = numeric_shape(image);
            if (shape >= 0) {
                const uint32_t kind = op == NUM_TYPE_IMAGE ? NUM_PLAIN_IMAGE :
                                      op == NUM_TYPE_SAMPLED ? NUM_SAMPLED : NUM_POINTER;
                for (uint32_t sign = 0; sign < 2; ++sign) {
                    if (m->types[sign].scalar && image[2] == m->types[sign].scalar) {
                        uint32_t *slot = &m->types[sign].image[shape][kind - 1];
                        if (*slot)
                            return 0;
                        *slot = id;
                    }
                }
            }
        }
        at += len;
    }
    return 1;
}

static int numeric_reserve(struct numeric_module *m, uint32_t *id, uint32_t words)
{
    if (*id)
        return 1;
    if (m->next_id == NUM_MAX_BOUND || m->size > SIZE_MAX / sizeof(uint32_t) - words)
        return 0;
    *id = m->next_id++;
    m->size += words;
    return 1;
}

static uint32_t numeric_texel_operands(uint32_t op)
{
    switch (op) {
    case NUM_SAMPLE: case NUM_SAMPLE_LOD: case NUM_SAMPLE_PROJ: case NUM_SAMPLE_PROJ_LOD:
    case NUM_FETCH: case NUM_IMAGE_READ: case NUM_SPARSE_READ:
        return 5;
    case NUM_SAMPLE_DREF: case NUM_SAMPLE_DREF_LOD:
    case NUM_SAMPLE_PROJ_DREF: case NUM_SAMPLE_PROJ_DREF_LOD:
    case NUM_GATHER: case NUM_GATHER_DREF:
        return 6;
    case NUM_IMAGE_WRITE:
        return 4;
    default:
        if (op >= NUM_SPARSE_SAMPLE && op <= NUM_SPARSE_GATHER_DREF)
            return numeric_texel_operands(op - (NUM_SPARSE_SAMPLE - NUM_SAMPLE));
        return 0;
    }
}

static int numeric_texel_extent(const uint32_t *p, uint32_t len, uint32_t mask_at)
{
    if (len <= mask_at)
        return len == mask_at;
    const uint32_t mask = p[mask_at];
    if (mask & ~0x17fffu)
        return 0;
    uint32_t operands = (mask & 4u) != 0; // Grad contributes a second ID.
    for (uint32_t ids = mask & 0x103ffu; ids; ids &= ids - 1)
        ++operands;
    return len - mask_at - 1 == operands;
}

static uint32_t numeric_texel_kind(uint32_t op)
{
    switch (op) {
    case NUM_SAMPLE: case NUM_SAMPLE_LOD: case NUM_SAMPLE_PROJ: case NUM_SAMPLE_PROJ_LOD:
    case NUM_GATHER: return NUM_SAMPLED;
    case NUM_FETCH: return NUM_PLAIN_IMAGE;
    default: return 0;
    }
}

static VkResult numeric_plan_body(struct numeric_module *m)
{
    for (size_t at = m->first_function; at < m->words; ) {
        const uint32_t *p = m->src + at;
        const uint32_t op = numeric_op(p), len = numeric_len(p), texel = numeric_texel_operands(op);
        const uint32_t wanted = numeric_texel_kind(op);
        if ((texel && len < texel) ||
            ((op == NUM_SAMPLE_LOD || op == NUM_SAMPLE_PROJ_LOD) && len == texel) ||
            (op == NUM_LOAD && len < 4) ||
            ((op == NUM_COPY_OBJECT || op == NUM_IMAGE || op == NUM_QUERY_SIZE ||
              op == NUM_QUERY_LEVELS || op == NUM_QUERY_SAMPLES) && len != 4) ||
            ((op == NUM_QUERY_SIZE_LOD || op == NUM_QUERY_LOD) && len != 5))
            return VK_ERROR_INITIALIZATION_FAILED;
        if (op == NUM_LOAD || op == NUM_COPY_OBJECT || op == NUM_IMAGE || wanted) {
            if (!numeric_valid_id(m, p[1]) || !numeric_valid_id(m, p[2]) ||
                !numeric_valid_id(m, p[3]))
                return VK_ERROR_INITIALIZATION_FAILED;
            const struct numeric_id *in = numeric_flow(m, p[3]);
            if (in) {
                const uint32_t *def = m->src + in->decl;
                const uint32_t old_type = def[1];
                struct numeric_types *types = &m->types[in->sign];
                uint32_t expected = old_type;
                uint8_t kind = in->kind;
                if (op == NUM_LOAD || op == NUM_IMAGE) {
                    const uint32_t *type = numeric_decl(m, old_type, op == NUM_LOAD ?
                                                        NUM_TYPE_POINTER : NUM_TYPE_SAMPLED);
                    if (!type || in->kind != (op == NUM_LOAD ? NUM_POINTER : NUM_SAMPLED))
                        return VK_ERROR_FEATURE_NOT_PRESENT;
                    expected = type[op == NUM_LOAD ? 3 : 2];
                    kind = op == NUM_LOAD ? NUM_SAMPLED : NUM_PLAIN_IMAGE;
                }
                if (wanted) {
                    const uint32_t *image = numeric_decl(m, old_type,
                        wanted == NUM_SAMPLED ? NUM_TYPE_SAMPLED : NUM_TYPE_IMAGE);
                    if (image && wanted == NUM_SAMPLED)
                        image = numeric_decl(m, image[2], NUM_TYPE_IMAGE);
                    const uint32_t *vec = numeric_decl(m, p[1], NUM_TYPE_VECTOR);
                    if (!numeric_texel_extent(p, len, texel))
                        return VK_ERROR_INITIALIZATION_FAILED;
                    if (!image || in->kind != wanted || !vec || vec[3] != 4 || vec[2] != image[2] ||
                        (len > texel && (p[texel] & NUM_IMAGE_EXTEND)))
                        return VK_ERROR_FEATURE_NOT_PRESENT;
                    if (!numeric_reserve(m, &types->vec4, 4) ||
                        m->texels >= NUM_MAX_BOUND - m->next_id || m->size > SIZE_MAX / sizeof(uint32_t) - 4)
                        return VK_ERROR_OUT_OF_HOST_MEMORY;
                    ++m->texels;
                    m->size += 4;
                } else {
                    struct numeric_id *out = &m->ids[p[2]];
                    if (p[1] != expected || out->decl)
                        return VK_ERROR_INITIALIZATION_FAILED;
                    out->decl = (uint32_t)at;
                    out->kind = kind;
                    out->shape = in->shape;
                    out->sign = in->sign;
                }
            }
        }
        at += len;
    }
    return VK_SUCCESS;
}

// Check every executable use, including forward references (e.g. OpPhi).
// Ignore literal operands, not unknown image consumers: an unhandled
// instruction using a specialized descriptor/value must fail closed.
static int numeric_uses(const struct numeric_module *m)
{
    for (size_t at = 5; at < m->words; ) {
        const uint32_t *p = m->src + at;
        const uint32_t op = numeric_op(p), len = numeric_len(p);
        if (at < m->first_function && op != NUM_VARIABLE && op != NUM_UNTYPED_VARIABLE &&
            op != NUM_EXT_INST && op != NUM_EXT_INST_FORWARD) {
            at += len; // OpEntryPoint/annotations may reference the unchanged IDs.
            continue;
        }
        uint32_t end = len, skip = numeric_texel_operands(op);
        switch (op) {
        case NUM_VARIABLE: case NUM_UNTYPED_VARIABLE:
            skip = 3; break; // storage class (initializer, if any, is an ID)
        case NUM_LOAD: end = 4; break; // optional memory access mask, alignment, and scopes
        case NUM_STORE: case NUM_COPY_MEMORY: end = 3; break;
        case NUM_COPY_MEMORY_SIZED: end = 4; break;
        case NUM_ARRAY_LENGTH: case NUM_COMPOSITE_EXTRACT: end = 4; break;
        case NUM_VECTOR_SHUFFLE: case NUM_COMPOSITE_INSERT: end = 5; break;
        case NUM_LOOP_MERGE: case NUM_SWITCH: end = 3; break; // switch cases interleave literals and labels
        case NUM_SELECTION_MERGE: end = 2; break;
        case NUM_BRANCH_COND: end = 4; break;
        case NUM_FUNCTION: skip = 3; break;
        case NUM_EXT_INST: case NUM_EXT_INST_FORWARD: skip = 4; break;
        case NUM_LINE: case NUM_NO_LINE: end = 1; break;
        default:
            if (op == NUM_BALLOT_BIT_COUNT || (op >= NUM_GROUP_IADD && op <= NUM_GROUP_XOR))
                skip = 4; // GroupOperation
            break;
        }
        for (uint32_t j = 1; j < end && j < len; ++j) {
            if (j == skip)
                continue;
            const struct numeric_id *id = numeric_flow(m, p[j]);
            if (!id || (j == 2 && id->decl == at))
                continue;
            if (j == 3 && id->decl < at) {
                if (op == NUM_LOAD || op == NUM_COPY_OBJECT || op == NUM_IMAGE || numeric_texel_kind(op))
                    continue; // checked by numeric_plan_body
                if (op >= NUM_QUERY_SIZE_LOD && op <= NUM_QUERY_SAMPLES &&
                    id->kind == (op == NUM_QUERY_LOD ? NUM_SAMPLED : NUM_PLAIN_IMAGE))
                    continue;
            }
            return 0;
        }
        at += len;
    }
    return 1;
}

static uint32_t *numeric_emit_types(const struct numeric_module *m, uint32_t *dst)
{
    for (uint32_t sign = 0; sign < 2; ++sign) {
        const struct numeric_types *types = &m->types[sign];
        if (types->scalar >= m->bound) {
            *dst++ = numeric_inst(NUM_TYPE_INT, 4);
            *dst++ = types->scalar;
            *dst++ = 32;
            *dst++ = sign;
        }
        if (types->vec4 >= m->bound) {
            *dst++ = numeric_inst(NUM_TYPE_VECTOR, 4);
            *dst++ = types->vec4;
            *dst++ = types->scalar;
            *dst++ = 4;
        }
        for (uint32_t shape = 0; shape < NUM_IMAGE_SHAPES; ++shape) {
            const uint32_t *id = types->image[shape];
            if (id[NUM_PLAIN_IMAGE - 1] >= m->bound) {
                *dst++ = numeric_inst(NUM_TYPE_IMAGE, 9);
                *dst++ = id[NUM_PLAIN_IMAGE - 1];
                *dst++ = types->scalar;
                *dst++ = shape / 12;     // Dim
                *dst++ = shape / 4 % 3;  // Depth
                *dst++ = shape / 2 % 2;  // Arrayed
                *dst++ = shape % 2;      // MS
                *dst++ = 1;              // Sampled
                *dst++ = 0;              // Unknown image format
            }
            if (id[NUM_SAMPLED - 1] >= m->bound) {
                *dst++ = numeric_inst(NUM_TYPE_SAMPLED, 3);
                *dst++ = id[NUM_SAMPLED - 1];
                *dst++ = id[NUM_PLAIN_IMAGE - 1];
            }
            if (id[NUM_POINTER - 1] >= m->bound) {
                *dst++ = numeric_inst(NUM_TYPE_POINTER, 4);
                *dst++ = id[NUM_POINTER - 1];
                *dst++ = NUM_UNIFORM_CONSTANT;
                *dst++ = id[NUM_SAMPLED - 1];
            }
        }
    }
    return dst;
}

static uint32_t numeric_replacement(const struct numeric_module *m, const struct numeric_id *id)
{
    return m->types[id->sign].image[id->shape][id->kind - 1];
}

static void numeric_emit(const struct numeric_module *m, const uint32_t *targets, uint32_t count,
                         uint32_t *dst)
{
    memcpy(dst, m->src, 5 * sizeof(*dst));
    dst[3] = m->next_id + m->texels;
    dst += 5;
    for (size_t at = 5; at < m->first_function; ) {
        const uint32_t *p = m->src + at;
        const uint32_t len = numeric_len(p);
        if (numeric_op(p) != NUM_VARIABLE || !numeric_flow(m, p[2])) {
            memcpy(dst, p, len * sizeof(*dst));
            dst += len;
        }
        at += len;
    }
    // A matching integer type can already be declared *after* a selected
    // variable. Insert new types after all existing types, and place just the
    // changed variables here, before their first executable use.
    dst = numeric_emit_types(m, dst);
    for (uint32_t i = 0; i < count; ++i) {
        const struct numeric_id *id = &m->ids[targets[i]];
        *dst++ = numeric_inst(NUM_VARIABLE, 4);
        *dst++ = numeric_replacement(m, id);
        *dst++ = targets[i];
        *dst++ = NUM_UNIFORM_CONSTANT;
    }
    uint32_t temporary = m->next_id;
    for (size_t at = m->first_function; at < m->words; ) {
        const uint32_t *p = m->src + at;
        const uint32_t op = numeric_op(p), len = numeric_len(p);
        memcpy(dst, p, len * sizeof(*dst));
        if (op == NUM_LOAD || op == NUM_IMAGE || op == NUM_COPY_OBJECT) {
            const struct numeric_id *id = numeric_flow(m, p[2]);
            if (id)
                dst[1] = numeric_replacement(m, id);
        } else if (numeric_texel_kind(op)) {
            const struct numeric_id *in = numeric_flow(m, p[3]);
            if (in) {
                // The integer texel gets a fresh ID; subsequent code still
                // sees the original float-vector ID, now produced by Bitcast.
                const uint32_t raw = temporary++;
                dst[1] = m->types[in->sign].vec4;
                dst[2] = raw;
                dst += len;
                *dst++ = numeric_inst(NUM_BITCAST, 4);
                *dst++ = p[1];
                *dst++ = p[2];
                *dst++ = raw;
                at += len;
                continue;
            }
        }
        dst += len;
        at += len;
    }
}

VkResult dm_m4_shader_numeric_code(const uint32_t *src, size_t words,
                                   const struct dm_m4_sample_type *types, uint32_t count,
                                   uint32_t **out, size_t *out_words)
{
    if (!out || !out_words)
        return VK_ERROR_INITIALIZATION_FAILED;
    *out = NULL;
    *out_words = 0;
    if (!src || words < 5 || words > UINT32_MAX || words > SIZE_MAX / sizeof(*src) ||
        src[0] != 0x07230203u || src[1] < 0x10000u || src[1] > 0x10600u || (src[1] & 255u) ||
        !src[3] || src[3] > NUM_MAX_BOUND || src[4] ||
        (count && !types) || count >= src[3] || count > (words - 5) / 4)
        return VK_ERROR_INITIALIZATION_FAILED;
    for (uint32_t i = 0; i < count; ++i)
        if ((types[i].sign != 1 && types[i].sign != -1) || (i && types[i - 1].binding >= types[i].binding))
            return VK_ERROR_INITIALIZATION_FAILED;

    struct numeric_module m = { .src = src, .words = words, .size = words, .bound = src[3], .next_id = src[3] };
    int in_function = 0;
    for (size_t at = 5; at < words; ) {
        const uint32_t len = numeric_len(src + at), op = numeric_op(src + at);
        if (!len || len > words - at)
            return VK_ERROR_INITIALIZATION_FAILED;
        if (op == NUM_FUNCTION) {
            if (in_function || len != 5)
                return VK_ERROR_INITIALIZATION_FAILED;
            if (!m.first_function)
                m.first_function = at;
            in_function = 1;
        } else if (op == NUM_FUNCTION_END) {
            if (!in_function || len != 1)
                return VK_ERROR_INITIALIZATION_FAILED;
            in_function = 0;
        }
        at += len;
    }
    if (in_function || !m.first_function)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!count) {
        *out = malloc(words * sizeof(*src));
        if (!*out)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        memcpy(*out, src, words * sizeof(*src));
        *out_words = words;
        return VK_SUCCESS;
    }

    // Both counts are capped by the SPIR-V ID bound.
    const size_t id_bytes = m.bound * sizeof(*m.ids);
    m.ids = calloc(1, id_bytes + count * sizeof(uint32_t));
    if (!m.ids)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t *targets = (uint32_t *)((unsigned char *)m.ids + id_bytes);
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    if (!numeric_index(&m, types, count, targets))
        goto done;
    uint32_t set = 0;
    for (uint32_t i = 0; i < count; ++i) {
        result = VK_ERROR_FEATURE_NOT_PRESENT;
        const uint32_t *var = numeric_decl(&m, targets[i], NUM_VARIABLE);
        if (!var || numeric_len(var) != 4 || var[3] != NUM_UNIFORM_CONSTANT)
            goto done;
        struct numeric_id *id = &m.ids[targets[i]];
        if (!id->set_plus_one || (set && set != id->set_plus_one) || id->kind)
            goto done;
        set = id->set_plus_one;
        const uint32_t *ptr = numeric_decl(&m, var[1], NUM_TYPE_POINTER);
        const uint32_t *sampled = ptr && ptr[2] == NUM_UNIFORM_CONSTANT ?
            numeric_decl(&m, ptr[3], NUM_TYPE_SAMPLED) : NULL;
        const uint32_t *image = sampled ? numeric_decl(&m, sampled[2], NUM_TYPE_IMAGE) : NULL;
        const int shape = numeric_shape(image);
        const uint32_t *scalar = shape >= 0 ? numeric_decl(&m, image[2], NUM_TYPE_FLOAT) : NULL;
        if (!scalar || numeric_len(scalar) != 3 || scalar[2] != 32)
            goto done;
        id->kind = NUM_POINTER;
        id->shape = (uint8_t)shape;
        id->sign = (uint8_t)(types[i].sign < 0);
        struct numeric_types *variant = &m.types[id->sign];
        uint32_t *ids = variant->image[shape];
        result = VK_ERROR_OUT_OF_HOST_MEMORY;
        if (!numeric_reserve(&m, &variant->scalar, 4) ||
            !numeric_reserve(&m, &ids[NUM_PLAIN_IMAGE - 1], 9) ||
            !numeric_reserve(&m, &ids[NUM_SAMPLED - 1], 3) ||
            !numeric_reserve(&m, &ids[NUM_POINTER - 1], 4))
            goto done;
    }
    result = numeric_plan_body(&m);
    if (result != VK_SUCCESS)
        goto done;
    result = VK_ERROR_FEATURE_NOT_PRESENT;
    if (!numeric_uses(&m))
        goto done;
    result = VK_ERROR_OUT_OF_HOST_MEMORY;
    *out = malloc(m.size * sizeof(**out));
    if (!*out)
        goto done;
    numeric_emit(&m, targets, count, *out);
    *out_words = m.size;
    result = VK_SUCCESS;
done:
    free(m.ids);
    return result;
}
