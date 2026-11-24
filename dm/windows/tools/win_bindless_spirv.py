"""Vulkan descriptor arrays addressed by real NVN handle IDs through a compact map."""

from collections import defaultdict
import re
import subprocess

from win_mesh_spirv import DEFINITION, ID, Wrapper, before, definitions, native_header

DIRECT = 0x40000000
MAP_CB_SLOT = 254
MISSING = 0xffffffff
IMAGE_CAPACITY = 65536  # Ocarina's 33,025-entry texture table, rounded to a power of two.
SAMPLER_CAPACITY = 4096  # NVN sampler ID is 12 bits.
STORAGE = 'StorageBuffer'
NONUNIFORM = ('ShaderNonUniform', 'SampledImageArrayNonUniformIndexing')
IMAGE_OPERATION = re.compile(r'OpImage(?:Sample\w+|Gather|DrefGather|Fetch|Query\w+|Read)$')
RESOURCE_OPERATION = {'OpSampledImage', 'OpImage', 'OpLoad', 'OpAccessChain', 'OpInBoundsAccessChain'}


def direct_pool(binding):
    """The translator's full guest-indexed pools, not its small slot arrays."""
    return (binding['source'] == 'texture' and binding['cbuf_slot'] == 0 and binding['handle'] == DIRECT and
            binding['count'] == (1 << (12 if binding['type'] == 0 else 20)))


def needs_lowering(bindings):
    # The compact sampler array still has 4096 entries. Its map, not its
    # length, distinguishes a compact sampler from a raw guest sampler.
    mapped = [b['cb_offset'] for b in bindings if b['source'] == 'sbuf' and b['cb_slot'] == MAP_CB_SLOT]
    if len(set(mapped)) != len(mapped):
        raise ValueError('duplicate compact descriptor map; rebuild from an earlier base')
    raw = any(direct_pool(b) and b['binding'] not in mapped for b in bindings)
    if raw and mapped:
        raise ValueError('mixed raw and compact descriptor pools; rebuild from an earlier base')
    return raw


def material_records(defs, body, var_bindings, bindings):
    """Recognize full-width handles loaded from aligned storage records.

    This is only an optimization of map preparation: an unproven index still
    uses the real active pool. A local load is unwrapped only for a single-store
    variable, and all possible offsets through a select must be in the result.
    """
    stores = defaultdict(list)
    for line in body:
        if (m := re.match(rf'^\s*OpStore ({ID}) ({ID})(?:\s|$)', line)):
            stores[m[1]].append(m[2])

    def constant(id_):
        if id_ in defs and defs[id_][0] == 'OpConstant' and defs[defs[id_][1]][0] == 'OpTypeInt':
            return int(defs[id_][2], 0) & 0xffffffff
        return None

    def peel(id_):
        seen = set()
        while id_ not in seen and id_ in defs:
            seen.add(id_)
            op, *args = defs[id_]
            if op in ('OpBitcast', 'OpCopyObject'):
                id_ = args[1]
            elif op == 'OpLoad' and defs[args[1]][0] == 'OpVariable' and defs[args[1]][2] == 'Function' and len(stores[args[1]]) == 1:
                id_ = stores[args[1]][0]
            else:
                break
        return id_

    def address(id_, depth=0):
        id_ = peel(id_)
        if depth > 16 or id_ not in defs:
            return None
        if (value := constant(id_)) is not None:
            return {0}, {value}
        op, *args = defs[id_]
        # Work modulo 32 even for larger records: 32-bit addresses can
        # wrap. (i * 96 + 96) % 32 is still zero after wrapping, unlike % 96.
        if op == 'OpShiftLeftLogical' and (shift := constant(args[2])) is not None and 5 <= shift < 32:
            return {32}, {0}
        if op == 'OpIMul' and any((k := constant(arg)) is not None and k % 32 == 0 for arg in args[1:]):
            return {32}, {0}
        if op == 'OpIAdd':
            left, right = address(args[1], depth + 1), address(args[2], depth + 1)
            if left and right and (left[0] == {0} or right[0] == {0}):
                return (left[0] | right[0]) - {0} or {0}, {x + y & 0xffffffff for x in left[1] for y in right[1]}
        if op == 'OpSelect':
            v, u = address(args[2], depth + 1), address(args[3], depth + 1)
            if v and u:
                return v[0] | u[0], v[1] | u[1]
        return None

    def trace(id_, sampler):
        op, *args = defs[peel(id_)]
        if op == 'OpExtInst' and args[2] == 'UMin' and constant(args[4]) == ((1 << (12 if sampler else 20)) - 1):
            op, *args = defs[peel(args[3])]
        want = ('OpShiftRightLogical', 20) if sampler else ('OpBitwiseAnd', (1 << 20) - 1)
        if op != want[0] or constant(args[2]) != want[1]:
            return None
        op, *args = defs[peel(args[1])]
        if op != 'OpLoad':
            return None
        op, *args = defs[args[1]]
        if op != 'OpAccessChain' or len(args) != 4 or constant(args[2]) != 0:
            return None
        binding = var_bindings.get(args[1])
        recipe = bindings.get(binding)
        if not recipe or recipe['source'] != 'sbuf' or recipe['flags'] or recipe['cb_slot'] != 0:
            return None
        op, *args = defs[peel(args[3])]
        if op != 'OpShiftRightLogical' or constant(args[2]) != 2:
            return None
        span = address(args[1])
        if not span or any(offset % 4 for offset in span[1]):
            return None
        return binding, span[0], {offset % 32 // 4 for offset in span[1]}

    return trace


def lookup_function(w, map_, capacity):
    """Return the dense index or latch raw + 1 into the map's first word."""
    ty, bol = w.u32, w.boolean
    ident, raw = w.id(), w.id()
    zero, one = w.const(0), w.const(1)
    missing = w.const(MISSING)
    w.emit(ident, '= OpFunction', ty, 'None', w.type('OpTypeFunction', ty, ty))
    w.emit(raw, '= OpFunctionParameter', ty)
    first = w.result('OpLabel')
    guest_count = w.load(map_, STORAGE, ty, zero, one)
    in_range = w.result('OpULessThan', bol, raw, guest_count)
    read, check = w.id(), w.id()
    w.emit('OpSelectionMerge', check, 'None')
    w.emit('OpBranchConditional', in_range, read, check)
    w.emit(read, '= OpLabel')
    dense = w.load(map_, STORAGE, ty, zero, w.uint('OpIAdd', raw, 3))
    w.emit('OpBranch', check)
    w.emit(check, '= OpLabel')
    dense = w.result('OpPhi', ty, missing, first, dense, read)
    count = w.load(map_, STORAGE, ty, zero, w.const(2))
    inside = w.result('OpULessThan', bol, dense, count)
    capacity_ok = w.result('OpULessThan', bol, dense, w.const(capacity))
    good = w.result('OpLogicalAnd', bol, inside, capacity_ok)
    fault, done = w.id(), w.id()
    w.emit('OpSelectionMerge', done, 'None')
    w.emit('OpBranchConditional', good, done, fault)
    w.emit(fault, '= OpLabel')
    status = w.access(map_, STORAGE, ty, zero, zero)
    w.result('OpAtomicCompareExchange', ty, status, one, zero, zero, w.uint('OpIAdd', raw, 1), zero)
    w.emit('OpBranch', done)
    w.emit(done, '= OpLabel')
    result = w.result('OpSelect', ty, good, dense, missing)
    w.emit('OpReturnValue', result)
    w.emit('OpFunctionEnd')
    return ident


def lower_assembly(text, bindings, descriptor_set):
    header, body, entry, model, original, globals_ = native_header(text)
    defs = definitions(header + body)
    old_recipes = {b['binding']: b for b in bindings}
    var_bindings = {m[1]: int(m[2]) for line in header
                    if (m := re.fullmatch(rf'\s*OpDecorate ({ID}) Binding (\d+)', line))}
    if set(var_bindings.values()) != old_recipes.keys():
        raise ValueError('SPIR-V and bindless descriptor recipes disagree')
    renumber = {i: n for n, i in enumerate(sorted(old_recipes))}
    pools = {var: old_recipes[b] for var, b in var_bindings.items() if direct_pool(old_recipes[b])}
    if not pools:
        raise ValueError('missing full guest descriptor arrays')
    traces = material_records(defs, body, var_bindings, old_recipes)
    w = Wrapper(header)
    array = w.id()
    w.decl.append(f'{array} = OpTypeRuntimeArray {w.u32}')
    w.annotate(array, 'ArrayStride 4')
    map_ty = w.id()
    w.decl.append(f'{map_ty} = OpTypeStruct {array}')
    w.annotate(map_ty, 'Block')
    w.annotations.append(f'OpMemberDecorate {map_ty} 0 Offset 0')
    new_maps, lookup, known_sources = {}, {}, defaultdict(list)
    for var, recipe in pools.items():
        n = len(renumber) + len(new_maps)
        map_var = w.global_(STORAGE, map_ty, f'DescriptorSet {descriptor_set}', f'Binding {n}')
        cap = SAMPLER_CAPACITY if recipe['type'] == 0 else IMAGE_CAPACITY
        new_maps[var] = (n, cap)
        lookup[var] = lookup_function(w, map_var, cap)

    lowered, moved, pool_uses = {}, set(), set()
    for ident, (op, *args) in defs.items():
        if not IMAGE_OPERATION.fullmatch(op):
            continue
        ret_ty, image, *operands = args
        dependencies, accesses = {}, {}

        def resource(id_):
            if id_ in dependencies or defs[id_][0] == 'OpVariable':
                return
            kind, *parts = defs[id_]
            if kind not in RESOURCE_OPERATION:
                raise ValueError(f'unsupported translated descriptor producer {kind}')
            if kind in ('OpAccessChain', 'OpInBoundsAccessChain'):
                base = parts[1]
                if base in pools:
                    if len(parts) != 3:
                        raise ValueError(f'{kind} of a direct descriptor must take one index')
                    accesses[id_] = (base, parts[2])
                elif defs[base][0] != 'OpVariable':
                    raise ValueError('unsupported nested descriptor access chain')
            else:
                for src in parts[1:]:
                    if src in defs and defs[src][0] != 'OpConstant':
                        resource(src)
            dependencies[id_] = (kind, *parts)

        resource(image)
        if not accesses:
            continue
        params = {}

        def value(arg):
            if arg not in defs or defs[arg][0].startswith(('OpConstant', 'OpSpecConstant')):
                return arg
            if arg not in params:
                params[arg] = w.id()
            return params[arg]

        for _, (base, raw) in accesses.items():
            value(raw)
            known_sources[base].append(traces(raw, pools[base]['type'] == 0))
        for id_, (kind, _, _, *tail) in dependencies.items():
            if kind in ('OpAccessChain', 'OpInBoundsAccessChain') and id_ not in accesses:
                for arg in tail:
                    value(arg)
        operands = [value(arg) for arg in operands]
        func = w.id()
        types = [defs[x][1] for x in params]
        w.emit(func, '= OpFunction', ret_ty, 'None', w.type('OpTypeFunction', ret_ty, *types))
        for ty, arg in zip(types, params.values()):
            w.emit(arg, '= OpFunctionParameter', ty)
        w.result('OpLabel')
        dense, valid = {}, None
        for id_, (base, raw) in accesses.items():
            index = params.get(raw, raw)
            if defs[raw][1] != w.u32:
                index = w.result('OpBitcast', w.u32, index)
            idx = w.result('OpFunctionCall', w.u32, lookup[base], index)
            good = w.result('OpINotEqual', w.boolean, idx, w.const(MISSING))
            valid = good if valid is None else w.result('OpLogicalAnd', w.boolean, valid, good)
            dense[id_] = idx
            pool_uses.add(base)
        read, missing, done = w.id(), w.id(), w.id()
        w.emit('OpSelectionMerge', done, 'None')
        w.emit('OpBranchConditional', valid, read, missing)
        w.emit(read, '= OpLabel')
        values, divergent = {}, set()
        for id_, (kind, ty, *args) in dependencies.items():
            if id_ in dense:
                output = w.result(kind, ty, args[0], dense[id_])
            elif kind in ('OpAccessChain', 'OpInBoundsAccessChain'):
                output = w.result(kind, ty, args[0], *(params.get(p, p) for p in args[1:]))
            else:
                output = w.result(kind, ty, *(values.get(p, p) for p in args))
            if id_ in dense or any(p in divergent for p in args):
                w.annotate(output, 'NonUniform')
                divergent.add(id_)
            values[id_] = output
        result = w.result(op, ret_ty, values[image], *operands)
        w.emit('OpBranch', done)
        w.emit(missing, '= OpLabel')
        # The runtime rejects the draw on a GPU-reported fault. Do not issue a
        # descriptor access (including to a dummy) on that execution path.
        w.emit('OpBranch', done)
        w.emit(done, '= OpLabel')
        ret = w.result('OpPhi', ret_ty, result, read, w.type('OpConstantNull', ret_ty), missing)
        w.emit('OpReturnValue', ret)
        w.emit('OpFunctionEnd')
        lowered[ident] = f'{ident} = OpFunctionCall {ret_ty} {func} ' + ' '.join(params)
        moved.update(dependencies)

    if pool_uses != pools.keys():
        raise ValueError('a direct descriptor binding has no supported image access')
    removed = moved.copy()
    # Shared (including static) descriptor producers remain if some other real
    # operation still reads them. Any surviving direct producer fails closed.
    while True:
        keep = set()
        for ident, (op, *args) in defs.items():
            if ident not in removed and ident not in lowered:
                keep.update(set(args) & removed)
        if not keep:
            break
        removed.difference_update(keep)
    for ident, (op, *args) in defs.items():
        if ident not in removed and op in RESOURCE_OPERATION and any(arg in pools for arg in args):
            raise ValueError(f'unhandled genuine direct descriptor access {op} {ident}')
    for i, line in enumerate(body):
        if (m := DEFINITION.match(line)):
            if m[1] in removed:
                body[i] = ''
            elif m[1] in lowered:
                body[i] = lowered[m[1]]

    def assign_array(var, cap):
        _, ptr, storage = defs[var]
        _, _, array_type = defs[ptr]
        op, elem, _ = defs[array_type]
        if storage != 'UniformConstant' or op != 'OpTypeArray':
            raise ValueError('unexpected direct descriptor declaration')
        compact = w.type('OpTypeArray', elem, w.const(cap))
        return w.type('OpTypePointer', storage, compact)

    types = {var: assign_array(var, cap) for var, (_, cap) in new_maps.items()}
    moved_globals = []
    for i, line in enumerate(header):
        if (m := re.fullmatch(rf'\s*OpDecorate ({ID}) Binding (\d+)', line)):
            header[i] = f'OpDecorate {m[1]} Binding {renumber[int(m[2])]}'
        elif (m := re.match(rf'^\s*Op(?:Name|Decorate) ({ID})\s', line)) and m[1] in removed:
            header[i] = ''
        elif (m := DEFINITION.match(line)) and m[1] in types:
            header[i] = ''
            moved_globals.append(f'{m[1]} = OpVariable {types[m[1]]} UniformConstant')
    header[entry] = f'OpEntryPoint {model} {original} "main" ' + ' '.join(globals_ + w.interface)
    capabilities = {line.strip() for line in header if line.strip().startswith('OpCapability ')}
    before(header, ('OpExtension ', 'OpMemoryModel ', '%'), [f'OpCapability {cap}' for cap in NONUNIFORM
                                                              if f'OpCapability {cap}' not in capabilities])
    first_type = next(i for i, line in enumerate(header) if (m := DEFINITION.match(line)) and m[2].startswith('OpType'))
    header[first_type:first_type] = w.annotations
    result = [dict(old_recipes[b], binding=renumber[b]) for b in sorted(old_recipes)]
    for var, (binding, cap) in new_maps.items():
        associated = renumber[var_bindings[var]]
        recipe = result[associated]
        recipe.update(array=cap, count=cap)
        trace = known_sources[var]
        slot, stride, words = MISSING, 0, 0
        if trace and all(trace) and len({x[0] for x in trace}) == 1 and set.union(*(x[1] for x in trace)) in ({32}, {0, 32}):
            slot, stride = renumber[trace[0][0]], 32
            words = sum(1 << n for n in set.union(*(x[2] for x in trace)))
        result.append({'binding': binding, 'source': 'sbuf', 'descriptor_type': 'VK_DESCRIPTOR_TYPE_STORAGE_BUFFER',
                       'count': 1, 'slot': slot, 'cb_slot': MAP_CB_SLOT, 'cb_offset': associated, 'flags': 1,
                       'record_stride': stride, 'record_words': words})
    return '\n'.join(header + w.decl + moved_globals + body + w.code) + '\n', result


def lower_module(raw, bindings, descriptor_set, root, tools, run):
    source, assembly, output = (root.with_suffix(ext) for ext in ('.pool-input.spv', '.pool.spvasm', '.pool.spv'))
    source.write_bytes(raw)
    text = subprocess.check_output([tools / 'spirv-dis.exe', '--raw-id', source], text=True)
    lowered, recipes = lower_assembly(text, bindings, descriptor_set)
    assembly.write_text(lowered, encoding='utf-8')
    run(tools / 'spirv-as.exe', '--target-env', 'spv1.5', assembly, '-o', output)
    return output.read_bytes(), recipes
