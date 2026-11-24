"""Convert NVN fragment register bits to the numeric type in a recorded PSO."""
from functools import cache
import struct

# MTLPixelFormat values used in the ordinary NVN Metal pipeline keys. The
# consecutive Uint/Sint pairs match the format mappings in nvn_fmt_mtl.c.
MTL_UINT = {13, 23, 33, 53, 63, 73, 91, 103, 113, 123}
MTL_SINT = {n + 1 for n in MTL_UINT if n != 91}


@cache
def recorded_colors(path):
    # nvn_pso_skey.h: k starts at byte 88 (after the aligned stencil-fold
    # descriptor); pso_key's two pointers precede the eight color formats.
    colors = {}
    with path.open(encoding='utf-8') as rows:
        for line in rows:
            if not line.startswith(('R\t', 'M\t')):
                continue
            code = bytes.fromhex(line.rsplit('\t', 1)[-1])
            if not code[24] or code[50] or code[51] or not code[24:48].rstrip(b'\0').endswith(b'_b'):
                continue  # absent fragment or a Metal-specific tile program
            key = code[24:40].decode('ascii')
            masks = colors.setdefault(key, [0] * 8)
            for i, fmt in enumerate(struct.unpack_from('<8I', code, 104)):
                if fmt:
                    masks[i] |= 1 if fmt in MTL_UINT else 2 if fmt in MTL_SINT else 4
    return colors


def output_masks(colors, key, writes):
    unsigned = signed = 0
    for i, types in enumerate(colors.get(key, ())):
        if not (writes >> (4 * i) & 15) or not (types & 3):
            continue
        if types & (types - 1):
            raise ValueError(f'color location {i} needs distinct numeric variants across recorded PSOs')
        if types == 1:
            unsigned |= 1 << i
        else:
            signed |= 1 << i
    return unsigned, signed


# SPIR-V 1.x opcodes and enum operands used by the translator's scalar and
# vector fragment outputs. Leave their float computations (and inputs) alone.
TYPE_INT, TYPE_FLOAT, TYPE_VECTOR, TYPE_POINTER = 21, 22, 23, 32
FUNCTION, VARIABLE, LOAD, STORE = 54, 59, 61, 62
ACCESS_CHAIN, IN_BOUNDS_ACCESS_CHAIN, DECORATE, BITCAST = 65, 66, 71, 124
OUTPUT, LOCATION = 3, 30


def lower_output(raw, unsigned, signed):
    if not (unsigned or signed):
        return raw
    words = struct.unpack(f'<{len(raw) // 4}I', raw)
    header = list(words[:5])
    instructions = []
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xffff
        if not count or count > len(words) - i:
            raise ValueError('malformed fragment SPIR-V instruction')
        instructions.append((op, words[i + 1:i + count]))
        i += count

    def fresh():
        ident = header[3]
        header[3] += 1
        return ident

    type_ops = {TYPE_INT, TYPE_FLOAT, TYPE_VECTOR, TYPE_POINTER}
    types = {args[0]: (op, args[1:]) for op, args in instructions if op in type_ops}
    by_shape = {shape: ident for ident, shape in types.items()}
    new_types = []

    def type_id(op, *shape):
        spec = op, shape
        if spec not in by_shape:
            by_shape[spec] = new = fresh()
            new_types.append((op, (new, *shape)))
        return by_shape[spec]

    locations = {p[0]: p[2] for op, p in instructions if op == DECORATE and p[1] == LOCATION}
    pointers = {}
    globals_ = []
    found = 0
    for op, args in instructions:
        if op != VARIABLE or args[2] != OUTPUT or args[1] not in locations:
            continue
        pointer, ident, _ = args
        bit = 1 << locations[ident]
        if not ((unsigned | signed) & bit):
            continue
        found |= bit
        ptr_op, (storage, value) = types[pointer]
        if ptr_op != TYPE_POINTER or storage != OUTPUT:
            raise ValueError('fragment output variable is not an Output pointer')
        val_op, val_shape = types[value]
        scalar, components = val_shape if val_op == TYPE_VECTOR else (value, 1)
        if types[scalar] != (TYPE_FLOAT, (32,)):
            raise ValueError('expected a float32 scalar or vector fragment output')
        integer = type_id(TYPE_INT, 32, int(bool(signed & bit)))
        converted = type_id(TYPE_VECTOR, integer, components) if components > 1 else integer
        pointers[ident] = converted, integer if components > 1 else None
        globals_.append((VARIABLE, (type_id(TYPE_POINTER, OUTPUT, converted), ident, OUTPUT)))
    if found != (unsigned | signed):
        raise ValueError('recorded integer attachment has no matching fragment output variable')

    out = []
    variables = set(pointers)
    have_function = False
    for op, args in instructions:
        if op == VARIABLE and args[1] in variables:
            continue
        if op == FUNCTION and not have_function:
            have_function = True
            # Insert just before code, so reused types already exist.
            globals_at = len(out)
        if op in (ACCESS_CHAIN, IN_BOUNDS_ACCESS_CHAIN) and args[2] in pointers:
            integer = pointers[args[2]][1]
            if integer is None or len(args) != 4:
                raise ValueError('unsupported access chain into a typed fragment output')
            pointers[args[1]] = integer, None
            args = (type_id(TYPE_POINTER, OUTPUT, integer), *args[1:])
        if op == STORE and args[0] in pointers:
            converted = fresh()
            out.append((BITCAST, (pointers[args[0]][0], converted, args[1])))
            args = (args[0], converted, *args[2:])
        if op == LOAD and args[2] in pointers:
            converted = fresh()
            out.append((LOAD, (pointers[args[2]][0], converted, *args[2:])))
            op, args = BITCAST, (args[0], args[1], converted)
        out.append((op, args))
    if not have_function:
        raise ValueError('fragment SPIR-V has no function')
    out[globals_at:globals_at] = new_types + globals_
    for op, args in out:
        header.extend(((len(args) + 1) << 16 | op, *args))
    return struct.pack(f'<{len(header)}I', *header)
