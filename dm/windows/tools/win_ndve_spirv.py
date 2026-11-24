"""Vulkan lowerings for the first Rouleau movie network's translated SPIR-V."""

import re
import subprocess

# Actual image formats bound by nn::ndve for this model (the SUST is untyped).
OUTPUT_FORMATS = {
    '49dff010b9832251': (72, 'Rgba8', 'R8G8B8A8Unorm'),
    '5a7d69b067a7f011': (72, 'Rgb10A2', 'R10G10B10A2Unorm'),
    '4f41ccf24f163e0d': (74, 'Rgba8', 'R8G8B8A8Unorm'),
}
# The original convolutions write the next tensor through the c0 SSBO slot 0
# address. The up-convolutions also read the skip tensor through slot 4.
RAW_TENSORS = {
    '0a56002cdc7e1fce': False, '7f7ff0f36b54d90f': True, '9442573ee4ace011': False,
    'a8b4fb48fee0ccf8': False, 'ba079f68ae288dba': False, 'd15370a382630d4e': True,
    'e2eebd7ba18fe35f': False, 'e6be82d835fbe4d3': False,
}

ID = r'%\d+'
DEFINITION = re.compile(rf'^ *({ID}) = (Op\w+) ([^\n]+)', re.M)
FUNCTION = re.compile(rf'^ *({ID}) = OpFunction .*?^ *OpFunctionEnd$', re.M | re.S)


def one(matches):
    values = list(matches)
    if len(values) != 1:
        raise ValueError(f'expected one NDVE SPIR-V match, found {len(values)}')
    return values[0]


def round_hmma(body, u32):
    """Keep the translator's four real scalar chains; round after product 8 of 16."""
    fmas = list(re.finditer(rf'^ *({ID}) = OpExtInst ({ID}) ({ID}) Fma ({ID}) ({ID}) ({ID})$', body, re.M))
    vec2 = one(set(re.findall(rf'OpExtInst ({ID}) {fmas[0][3]} UnpackHalf2x16', body))) if fmas else None
    if len(fmas) != 64 or any(fmas[i][6] != fmas[i - 1][1] for i in range(64) if i % 16):
        raise ValueError('unexpected accumulator chains in dmsh_hmma16816_f16')
    # Use the same pack/unpack as the ends of the chains, including half denormals.
    for m in reversed(fmas[7::16]):
        out, f32, glsl, a, b, c = m.groups()
        temp = '%half_mid_' + out[1:]
        rounded = (f'{temp} = OpExtInst {f32} {glsl} Fma {a} {b} {c}\n'
                   f'{temp}_v = OpCompositeConstruct {vec2} {temp} {temp}\n'
                   f'{temp}_p = OpExtInst {u32} {glsl} PackHalf2x16 {temp}_v\n'
                   f'{temp}_u = OpExtInst {vec2} {glsl} UnpackHalf2x16 {temp}_p\n'
                   f'{out} = OpCompositeExtract {f32} {temp}_u 0')
        body = body[:m.start()] + rounded + body[m.end():]
    return body


def tensor_access(header, tensor, c0, word, types, write):
    # Raw helpers have (signed-int function pointers for) the address low/high
    # words, and, for the store, the value word. Keep that call ABI unchanged.
    u32, i32, boolean, uniform, elem_type, storage = types
    params = re.findall(rf'^ *({ID}) = OpFunctionParameter {ID}$', header, re.M)
    lo, hi = params[:2]
    lines = [header.rstrip()]
    stem = '%tensor_' + ('st' if write else 'ld')

    def emit(name, op):
        lines.append(f'{stem}_{name} = {op}')
        return f'{stem}_{name}'

    emit('entry', 'OpLabel')
    address = [emit(n, f'OpBitcast {u32} {emit(n + "_i", f"OpLoad {i32} {p}")}')
               for n, p in [('lo', lo), ('hi', hi)]]
    base = []
    for j, n in enumerate(('base_lo', 'base_hi', 'size')):
        ptr = emit(n + '_p', f'OpAccessChain {uniform} {c0} %ndve_i0 %ndve_i{word // 4} %ndve_i{j}')
        value = emit(n + '_v', f'OpLoad {elem_type} {ptr}')
        base.append(emit(n, f'OpBitcast {u32} {value}') if elem_type != u32 else value)
    low, high = address
    base_lo, base_hi, size = base
    # Compare the full 64-bit guest range with two 32-bit words. A tensor may
    # cross a 4 GiB boundary; comparing the high words directly loses that case.
    byte = emit('byte', f'OpISub {u32} {low} {base_lo}')
    borrow = emit('borrow', f'OpULessThan {boolean} {low} {base_lo}')
    carry = emit('carry', f'OpSelect {u32} {borrow} %ndve_u1 %ndve_u0')
    high_delta = emit('high_delta', f'OpISub {u32} {high} {base_hi}')
    high_ok = emit('high_ok', f'OpIEqual {boolean} {high_delta} {carry}')
    remaining = emit('remaining', f'OpISub {u32} {size} {byte}')
    start_ok = emit('start_ok', f'OpULessThan {boolean} {byte} {size}')
    end_ok = emit('end_ok', f'OpUGreaterThanEqual {boolean} {remaining} %ndve_u4')
    range_ok = emit('range_ok', f'OpLogicalAnd {boolean} {start_ok} {end_ok}')
    ok = emit('ok', f'OpLogicalAnd {boolean} {high_ok} {range_ok}')
    lines += [f'OpSelectionMerge {stem}_end None', f'OpBranchConditional {ok} {stem}_op {stem}_end']
    emit('op', 'OpLabel')
    index = emit('index', f'OpShiftRightLogical {u32} {byte} %ndve_u2')
    pointer = emit('p', f'OpAccessChain {storage} {tensor} %ndve_i0 {index}')
    if write:
        value = emit('val', f'OpLoad {i32} {params[2]}')
        value = emit('word', f'OpBitcast {u32} {value}')
        lines += [f'OpStore {pointer} {value}', 'OpReturn']
    else:
        value = emit('word', f'OpLoad {u32} {pointer}')
        value = emit('val', f'OpBitcast {i32} {value}')
        lines.append(f'OpReturnValue {value}')
    emit('end', 'OpLabel')
    lines += ['OpReturn' if write else 'OpReturnValue %ndve_i0', 'OpFunctionEnd']
    return '\n'.join(lines)


def lower_ndve(text, res, pool_slot, local):
    """Return Vulkan assembly and workgroup bytes, updating changed binding recipes."""
    key = res['key']
    definitions = {name: (op, args.split()) for name, op, args in DEFINITION.findall(text)}

    def dtype(identifier, opcode):
        op, args = definitions[identifier]
        if op != opcode:
            raise ValueError(f'expected {opcode} for {identifier}, found {op}')
        return args

    if tuple(map(int, one(re.findall(rf'OpExecutionMode {ID} LocalSize (\d+) (\d+) (\d+)', text)))) != tuple(local):
        raise ValueError('SPIR-V local size disagrees with NVN2 control section')
    shared = 0
    for op, args in definitions.values():
        if op == 'OpVariable' and args[1] == 'Workgroup':
            _, array = dtype(args[0], 'OpTypePointer')
            elem, length = dtype(array, 'OpTypeArray')
            width, _ = dtype(elem, 'OpTypeInt')
            _, count = dtype(length, 'OpConstant')
            shared += (int(width) // 8) * int(count)

    names = {name: identifier for identifier, name in re.findall(rf'OpName ({ID}) "([^"]+)"', text)}
    bindings = {int(number): identifier for identifier, number in re.findall(rf'OpDecorate ({ID}) Binding (\d+)', text)}
    u32 = one(k for k, d in definitions.items() if d == ('OpTypeInt', ['32', '0']))
    replacements = {}
    functions = {m[1]: m[0] for m in FUNCTION.finditer(text)}
    hmma = names.get('dmsh_hmma16816_f16')
    if key != '49dff010b9832251':
        if not hmma:
            raise ValueError('NDVE HMMA helper missing')
        replacements[hmma] = round_hmma(functions[hmma], u32)

    if key in OUTPUT_FORMATS:
        handle, spv_format, vk_format = OUTPUT_FORMATS[key]
        image = one(res['images'])
        if (image['cbuf_slot'], image['handle'], image['type'], image['format']) != (-1, handle, 3, 'Unknown'):
            raise ValueError('unexpected NDVE storage image binding')
        op, (pointer, storage) = definitions[bindings[image['binding']]]
        ptr_op, (ptr_storage, image_type) = definitions[pointer]
        img_op, (_, *shape) = definitions[image_type]
        if (op, storage, ptr_op, ptr_storage, img_op, shape) != (
                'OpVariable', 'UniformConstant', 'OpTypePointer', 'UniformConstant', 'OpTypeImage',
                ['2D', '0', '0', '0', '2', 'Unknown']):
            raise ValueError('unexpected NDVE SPIR-V storage image type')
        text = re.sub(rf'(^ *{image_type} = OpTypeImage [^\n]+) Unknown$', rf'\1 {spv_format}', text, flags=re.M)
        if spv_format == 'Rgb10A2':
            text = text.replace('OpCapability Shader\n', 'OpCapability Shader\n               OpCapability StorageImageExtendedFormats\n', 1)
        image['format'] = vk_format

    raw_find = names.get('dmsh_raw_find')
    pool = {b['cb_offset']: b for b in res['sbufs'] if b['cb_slot'] == pool_slot}
    constants = ''
    if key in RAW_TENSORS:
        if not raw_find or set(pool) != {0, 1} or (pool[0]['flags'], pool[1]['flags']) != (0, 1):
            raise ValueError('unexpected NDVE raw memory bindings')
        c0 = bindings[one(b for b in res['cbufs'] if b['slot'] == 0)['binding']]
        if c0 != names['cp_c0']:
            raise ValueError('unexpected NDVE driver c0 binding')

        _, block = dtype(dtype(c0, 'OpVariable')[0], 'OpTypePointer')
        array, = dtype(block, 'OpTypeStruct')
        vector, _ = dtype(array, 'OpTypeArray')
        elem_type, width = dtype(vector, 'OpTypeVector')
        if width != '4':
            raise ValueError('unexpected NDVE driver c0 row width')
        i32 = one(k for k, d in definitions.items() if d == ('OpTypeInt', ['32', '1']))
        boolean = one(re.findall(rf'^ *({ID}) = OpTypeBool$', text, re.M))
        storage = one(k for k, d in definitions.items() if d == ('OpTypePointer', ['Uniform', u32]))
        uniform = one(k for k, d in definitions.items() if d == ('OpTypePointer', ['Uniform', elem_type]))
        types = u32, i32, boolean, uniform, elem_type, storage
        has_load = RAW_TENSORS[key]
        seen = set()
        for identifier, body in functions.items():
            if not re.search(rf'OpFunctionCall {ID} {raw_find}\b', body):
                continue
            ptr = one(re.findall(rf'^ *({ID}) = OpConvertUToPtr {ID} {ID}$', body, re.M))
            if re.search(rf'OpStore {ptr} {ID} Aligned 4$', body, re.M):
                write, recipe, word = True, pool[1], 0x310 // 4
            elif re.search(rf'OpLoad {u32} {ptr} Aligned 4$', body, re.M) and has_load:
                write, recipe, word = False, pool[0], 0x350 // 4
            else:
                raise ValueError('unknown NDVE physical memory operation')
            header = body.split(' = OpLabel', 1)[0].rsplit('\n', 1)[0]
            if len(re.findall(r'OpFunctionParameter', header)) != (3 if write else 2) or write in seen:
                raise ValueError('unexpected NDVE raw tensor call ABI')
            target = bindings[recipe['binding']]
            replacements[identifier] = tensor_access(header, target, c0, word, types, write)
            recipe.update(cb_slot=0, cb_offset=word)
            seen.add(write)
        if seen != ({False, True} if has_load else {True}):
            raise ValueError('NDVE raw load/store helpers do not match the network')
        if not has_load:
            res['sbufs'].remove(pool[0])
        replacements[raw_find] = ''
        text = re.sub(rf'^ *OpName {raw_find} [^\n]+\n', '', text, flags=re.M)
        text = re.sub(r'^ *(?:OpCapability (?:PhysicalStorageBufferAddresses|Int64)|'
                      r'OpExtension "SPV_KHR_physical_storage_buffer"|%\d+ = OpTypeInt 64 0|'
                      r'%\d+ = OpTypePointer PhysicalStorageBuffer [^\n]+)\n', '', text, flags=re.M)
        text = text.replace('OpMemoryModel PhysicalStorageBuffer64 GLSL450', 'OpMemoryModel Logical GLSL450')
        constants = ''.join(f'%ndve_i{n} = OpConstant {i32} {n}\n' for n in (0, 1, 2, 49, 53))
        constants += ''.join(f'%ndve_u{n} = OpConstant {u32} {n}\n' for n in (0, 1, 2, 4))
        res['notes'] = [note for note in res['notes'] if 'goes through the memory pool table' not in note]
        res['notes'].append('Vulkan: NDVE raw tensor accesses lowered to c0 SSBO ranges')
    elif raw_find or pool:
        raise ValueError('unexpected NDVE raw memory helper')

    text = FUNCTION.sub(lambda m: replacements.get(m[1], m[0]), text)
    if constants:
        pos = next(FUNCTION.finditer(text)).start()
        text = text[:pos] + constants + text[pos:]
    if re.search(r'OpConvertUToPtr|PhysicalStorageBuffer', text):
        raise ValueError('unhandled NDVE physical storage access')
    return text, shared


def lower_module(source, res, pool_slot, tools, local):
    text = subprocess.check_output([tools / 'spirv-dis.exe', '--raw-id', source], text=True)
    version = one(re.findall(r'^; Version: (\d+\.\d+)$', text, re.M))
    text, shared = lower_ndve(text, res, pool_slot, local)
    output = source.with_suffix('.vulkan.spv')
    subprocess.run([tools / 'spirv-as.exe', '--target-env', 'spv' + version,
                    '--preserve-numeric-ids', '-o', output, '-'], input=text, text=True, check=True)
    return output, shared
