"""Wrap the translated Ampere task/mesh body in a native Vulkan shader entry.

The body and its resource bindings still come from dmsh_xlate. The wrapper
implements the same shared-memory interface as the Darwin adapter (dmsh_meshstage)
without going through MSL or changing NVN's clip-space convention.
"""

import re
import subprocess

ID = r'%\d+'
DEFINITION = re.compile(rf'^ *({ID}) = (Op\w+)\s*(.*)$')
MESH_EXTENSION = '"SPV_EXT_mesh_shader"'
TOPOLOGY = {1: ('OutputPoints', 'PrimitivePointIndicesEXT'),
            2: ('OutputLinesEXT', 'PrimitiveLineIndicesEXT'),
            3: ('OutputTrianglesEXT', 'PrimitiveTriangleIndicesEXT')}
PRIMITIVE_BUILTINS = {8: 'PrimitiveId', 9: 'Layer', 10: 'ViewportIndex'}


def definitions(lines):
    return {m[1]: (m[2], *m[3].split()) for line in lines if (m := DEFINITION.match(line))}


def before(lines, prefix, new):
    at = next(i for i, line in enumerate(lines) if line.lstrip().startswith(prefix))
    lines[at:at] = new


def enable_mesh(header, extra=()):
    before(header, ('OpExtension ', 'OpMemoryModel ', '%'), ['OpCapability MeshShadingEXT', *(
        f'OpCapability {cap}' for cap in extra if not any(
            line.strip() == f'OpCapability {cap}' for line in header))])
    before(header, ('OpMemoryModel ', '%'), [f'OpExtension {MESH_EXTENSION}'])


def storage_buffer_class(header, body, defs):
    """Promote Ryujinx's legacy BufferBlock SSBOs for SPIR-V 1.4 or later."""
    blocks = {m[1] for line in header if (m := re.fullmatch(rf'\s*OpDecorate ({ID}) BufferBlock', line))}
    pointers = {id_: (storage, pointee) for id_, (op, *args) in defs.items()
                if op == 'OpTypePointer' for storage, pointee in (args,)}
    roots = {id_ for id_, (op, *args) in defs.items() if op == 'OpVariable' and
             pointers[args[0]][0] == 'Uniform' and pointers[args[0]][1] in blocks}
    promoted = {}
    known = {pointee: id_ for id_, (storage, pointee) in pointers.items() if storage == 'StorageBuffer'}

    def pointer(original):
        storage, pointee = pointers[original]
        if storage != 'Uniform':
            raise ValueError(f'SSBO pointer {original} was not Uniform')
        if pointee in known:
            return known[pointee]
        return promoted.setdefault(original, f'%dm_ssbo_{original[1:]}')

    for i, line in enumerate(header):
        if line.strip().endswith(' BufferBlock'):
            header[i] = line.removesuffix('BufferBlock') + 'Block'
        elif (m := DEFINITION.match(line)) and m[1] in roots:
            header[i] = f'{m[1]} = OpVariable {pointer(defs[m[1]][1])} StorageBuffer'
    # The translator emits individual storage operations via access chains; it
    # does not pass Uniform pointers through its function ABI.
    for i, line in enumerate(body):
        if (m := DEFINITION.match(line)) and m[2] in ('OpAccessChain', 'OpInBoundsAccessChain',
                                                     'OpPtrAccessChain', 'OpInBoundsPtrAccessChain'):
            ty, base, *indices = m[3].split()
            if base in roots:
                roots.add(m[1])
                body[i] = f'{m[1]} = {m[2]} {pointer(ty)} {base} ' + ' '.join(indices)
    # Reuse the pointee's original declaration order, particularly for structs.
    result = []
    for line in header:
        result.append(line)
        if (m := DEFINITION.match(line)) and m[1] in promoted:
            result.append(f'{promoted[m[1]]} = OpTypePointer StorageBuffer {pointers[m[1]][1]}')
    return result


def native_header(text):
    lines = [line for line in text.splitlines() if not line.startswith(';')]
    start = next(i for i, line in enumerate(lines) if re.match(r'^ *%\d+ = OpFunction ', line))
    header, body = lines[:start], lines[start:]
    defs = definitions(header)
    header = storage_buffer_class(header, body, defs)
    entry = next(i for i, line in enumerate(header) if line.lstrip().startswith('OpEntryPoint '))
    model, original, name, *_ = header[entry].split()[1:]
    if name != '"main"':
        raise ValueError(f'expected main SPIR-V entry point, got {name}')
    # SPIR-V >=1.4 lists all (not just Input/Output) globals used by the entry.
    used = set(re.findall(ID + r'\b', '\n'.join(body)))
    globals_ = [id_ for id_, (op, *_) in defs.items() if op == 'OpVariable' and id_ in used]
    return header, body, entry, model, original, globals_


class Wrapper:
    def __init__(self, header):
        self.by_instruction = {args: id_ for id_, args in definitions(header).items()
                               if args[0] != 'OpVariable'}
        self.decl, self.annotations, self.interface, self.code = [], [], [], []
        self.serial = 0
        self.u32 = self.type('OpTypeInt', 32, 0)
        self.boolean = self.type('OpTypeBool')
        self.f32 = self.type('OpTypeFloat', 32)

    def id(self):
        self.serial += 1
        return f'%dm_{self.serial}'

    def emit(self, *instruction):
        self.code.append(' '.join(map(str, instruction)))

    def result(self, op, *args):
        id_ = self.id()
        self.emit(id_, '=', op, *args)
        return id_

    def type(self, op, *args):
        inst = (op, *map(str, args))
        if inst not in self.by_instruction:
            self.by_instruction[inst] = id_ = self.id()
            self.decl.append(f'{id_} = ' + ' '.join(inst))
        return self.by_instruction[inst]

    def const(self, value, ty=None):
        return self.type('OpConstant', ty or self.u32, value)

    def annotate(self, target, annotation):
        self.annotations.append(f'OpDecorate {target} {annotation}')

    def global_(self, storage, ty, *annotations):
        ptr = self.type('OpTypePointer', storage, ty)
        id_ = self.id()
        self.decl.append(f'{id_} = OpVariable {ptr} {storage}')
        self.interface.append(id_)
        for annotation in annotations:
            self.annotate(id_, annotation)
        return id_

    def access(self, base, storage, ty, *indices):
        return self.result('OpAccessChain', self.type('OpTypePointer', storage, ty), base, *indices)

    def load(self, base, storage, ty, *indices):
        return self.result('OpLoad', ty, self.access(base, storage, ty, *indices))

    def store(self, base, storage, ty, value, *indices):
        self.emit('OpStore', self.access(base, storage, ty, *indices), value)

    def uint(self, op, x, y):
        return self.result(op, self.u32, x, self.const(y) if isinstance(y, int) else y)

    def barrier(self):
        self.emit('OpControlBarrier', self.const(2), self.const(2), self.const(0x108))

    def loop(self, preceding, first, limit, step, process):
        head, work, advance, end, index, next_ = (self.id() for _ in range(6))
        self.emit('OpBranch', head)
        self.emit(head, '= OpLabel')
        self.emit(index, '= OpPhi', self.u32, first, preceding, next_, advance)
        check = self.result('OpULessThan', self.boolean, index, limit)
        self.emit('OpLoopMerge', end, advance, 'None')
        self.emit('OpBranchConditional', check, work, end)
        self.emit(work, '= OpLabel')
        process(index)
        self.emit('OpBranch', advance)
        self.emit(advance, '= OpLabel')
        self.emit(next_, '= OpIAdd', self.u32, index, self.const(step))
        self.emit('OpBranch', head)
        self.emit(end, '= OpLabel')
        return end


def mesh_outputs(w, i, vertex, primitive, block, load, index_load):
    """Build declarations and the per-vertex/per-primitive export loops."""
    max_v, max_p, width = i['max_v'], i['max_p'], i['verts']
    output, pu = 'Output', 'PerPrimitiveEXT'
    mode, indices = TOPOLOGY[width]
    i32 = w.type('OpTypeInt', 32, 1)
    idx_ty = w.type('OpTypeVector', w.u32, width) if width != 1 else w.u32

    def export(ty, count, *annotations):
        return w.global_(output, w.type('OpTypeArray', ty, w.const(count)), *annotations)

    def output_locations(comps, default, count, offset=0, qualifiers=()):
        result = []
        vec4 = w.type('OpTypeVector', w.f32, 4)
        for n in sorted({n for n, _ in comps if isinstance(n, int)} | set(default)):
            channels = [offset + comps.index((n, c)) if (n, c) in comps else None for c in range(4)]
            result.append((export(vec4, count, f'Location {n}', *qualifiers), channels))
        return vec4, result

    vlocations = {n for n, _ in vertex if isinstance(n, int)}
    plocations = {n for n, _ in primitive}
    if vlocations & plocations:
        raise ValueError('a generic mesh location is both per-vertex and per-primitive')
    vec4, vfields = output_locations(vertex, i['fragment_locations'] - vlocations - plocations, max_v)
    _, pfields = output_locations(primitive, (), max_p, len(i['primitive_builtins']), (pu,))
    position = [vertex.index((None, c)) if (None, c) in vertex else None for c in range(4)]
    if all(c is None for c in position):
        raise ValueError('mesh SPH enables no position component')
    vfields.insert(0, (export(vec4, max_v, 'BuiltIn Position', 'Invariant'), position))
    point = export(w.f32, max_v, 'BuiltIn PointSize') if width == 1 or ('psiz', 0) in vertex else None
    p_builtin = [(export(i32, max_p, f'BuiltIn {PRIMITIVE_BUILTINS[b]}', pu), k)
                 for k, b in enumerate(i['primitive_builtins'])]
    indices = export(idx_ty, max_p, f'BuiltIn {indices}', pu)

    def write(attribute, ty, n, values):
        value = w.result('OpCompositeConstruct', ty, *values) if len(values) > 1 else values[0]
        w.store(attribute, output, ty, value, n)

    def fields(n, base, attributes):
        for attr, components in attributes:
            values = [w.const(int(c == 3), w.f32) if v is None else
                      w.result('OpBitcast', w.f32, load(w.uint('OpIAdd', base, v * block)))
                      for c, v in enumerate(components)]
            write(attr, vec4, n, values)

    def vertex_at(n, base):
        fields(n, base, vfields)
        if point:
            value = (w.result('OpBitcast', w.f32, load(w.uint('OpIAdd', base, vertex.index(('psiz', 0)) * block)))
                     if ('psiz', 0) in vertex else w.const(1, w.f32))
            write(point, w.f32, n, [value])

    def primitive_at(n, base):
        fields(n, base, pfields)
        for attr, k in p_builtin:
            write(attr, i32, n, [w.result('OpBitcast', i32, load(w.uint('OpIAdd', base, k * block)))])
        start = w.uint('OpIMul', n, width)
        write(indices, idx_ty, n, [index_load(w.uint('OpIAdd', start, k)) for k in range(width)])

    return mode, vertex_at, primitive_at


def lower_stage(text, i, guest):
    header, body, entry, model, original, globals_ = native_header(text)
    if model != 'GLCompute':
        raise ValueError(f'expected an Ampere compute body, got {model}')
    w = Wrapper(header)
    defs = definitions(header)
    wg, inp, out, prim, *_ = guest.memory(i)[0]
    wg, inp, out = wg // 4, inp // 4, out // 4
    words = i['shared'] // 4
    stage_memory = []
    for variable in globals_:
        _, ptr, storage, *_ = defs[variable]
        if storage == 'Workgroup':
            _, _, item = defs[ptr]
            if defs[item][0] == 'OpTypeArray':
                _, element, count = defs[item]
                if element == w.u32 and defs[count] == ('OpConstant', w.u32, str(words)):
                    stage_memory.append(variable)
    if len(stage_memory) != 1:
        raise ValueError(f'expected one {words}-word Ampere stage array, found {len(stage_memory)}')
    mem = stage_memory[0]
    task, taskful = i['kind'] == 'ts', i.get('task', False)
    payload = 'TaskPayloadWorkgroupEXT'
    threads, pay = i['threads'], (i['payload'] + 3) // 4
    if (task or taskful) and pay < 2:
        raise ValueError('Ampere task payload is missing its launch header')
    if bad := set(i.get('primitive_builtins', ())) - PRIMITIVE_BUILTINS.keys():
        raise ValueError(f'unsupported mesh per-primitive system components: {sorted(bad)}')
    extra = []
    if 9 in i.get('primitive_builtins', ()):
        extra.append('ShaderLayer')
    if 10 in i.get('primitive_builtins', ()):
        extra.append('ShaderViewportIndex')
    uv3 = w.type('OpTypeVector', w.u32, 3)

    def builtin(name, ty):
        pattern = re.compile(rf'\s*OpDecorate ({ID}) BuiltIn {name}')
        return next((m[1] for line in header if (m := pattern.fullmatch(line))), None) or w.global_(
            'Input', ty, f'BuiltIn {name}')

    lid_var = builtin('LocalInvocationIndex', w.u32)
    group_var = builtin('WorkgroupId', uv3)
    if task or taskful:
        payload_array = w.type('OpTypeArray', w.u32, w.const(pay))
        data = w.global_(payload, w.type('OpTypeStruct', payload_array))
    if not taskful:
        pc_type = w.id()
        w.decl.append(f'{pc_type} = OpTypeStruct {w.u32}')
        w.annotate(pc_type, 'Block')
        w.annotations.append(f'OpMemberDecorate {pc_type} 0 Offset 0')
        push = w.global_('PushConstant', pc_type)

    def load(index):
        return w.load(mem, 'Workgroup', w.u32, index)

    def store(index, value):
        w.store(mem, 'Workgroup', w.u32, value, index)

    def index_load(index):
        addr = w.uint('OpIAdd', index, prim + 4)
        word = load(w.uint('OpShiftRightLogical', addr, 2))
        shift = w.uint('OpIMul', w.uint('OpBitwiseAnd', addr, 3), 8)
        return w.uint('OpBitwiseAnd', w.uint('OpShiftRightLogical', word, shift), 255)

    void, _, fn_ty = next(m[3].split() for line in body if (m := DEFINITION.match(line))
                               and m[1] == original and m[2] == 'OpFunction')
    # The translator's main has no arguments and is called once per lane. Its
    # early returns are now returns to the wrapper, not to the mesh pipeline.
    if defs[fn_ty] != ('OpTypeFunction', void) or defs[void] != ('OpTypeVoid',):
        raise ValueError('expected void Ampere shader body with no parameters')
    w.emit('%dm_main = OpFunction', void, 'None', fn_ty)
    label = w.result('OpLabel')
    zero, one = w.const(0), w.const(1)
    lid = w.result('OpLoad', w.u32, lid_var)
    group = w.load(group_var, 'Input', w.u32, zero)
    if not taskful:
        group = w.uint('OpIAdd', group, w.load(push, 'PushConstant', w.u32, zero))
    label = w.loop(label, lid, w.const(words), threads, lambda n: store(n, zero))
    w.barrier()
    if taskful:
        # Word 0 is set by NVN hardware per child, not inherited from the task.
        label = w.loop(label, w.uint('OpIAdd', lid, 1), w.const(pay), threads,
                       lambda n: store(w.uint('OpIAdd', n, inp), w.load(data, payload, w.u32, zero, n)))
    leader, ready = w.id(), w.id()
    first = w.result('OpIEqual', w.boolean, lid, zero)
    w.emit('OpSelectionMerge', ready, 'None')
    w.emit('OpBranchConditional', first, leader, ready)
    w.emit(leader, '= OpLabel')
    store(w.const(wg), group)
    if taskful:
        store(w.const(inp), group)
    w.emit('OpBranch', ready)
    w.emit(ready, '= OpLabel')
    w.barrier()
    w.result('OpFunctionCall', void, original)
    w.barrier()
    if task:
        def copy(n):
            w.store(data, payload, w.u32, load(w.uint('OpIAdd', n, out)), zero, n)

        w.loop(ready, lid, w.const(pay), threads, copy)
        launches = load(w.const(out + 1))
        # This terminator also synchronizes writes to the task payload.
        w.emit('OpEmitMeshTasksEXT', launches, one, one, data)
    else:
        nv, np = i['max_v'], i['max_p']
        count = w.uint('OpShiftRightLogical', load(w.const(prim // 4)), 24)
        below = w.result('OpULessThan', w.boolean, count, w.const(np))
        count = w.result('OpSelect', w.u32, below, count, w.const(np))
        # NVN supplies a primitive count and byte indices, not a vertex count.
        w.emit('OpSetMeshOutputsEXT', w.const(nv), count)
        block = guest.BLOCK
        mode, vertex_at, primitive_at = mesh_outputs(w, i, i['vertex'], i['primitive'], block, load, index_load)

        def at(n, start, stride, export):
            row = w.uint('OpIMul', w.uint('OpUDiv', n, block), stride * block)
            base = w.uint('OpIAdd', w.uint('OpIAdd', row, start), w.uint('OpUMod', n, block))
            export(n, base)

        nvertex = len(i['vertex'])
        p_start = out + guest.blocks_of(nv) * nvertex * block
        end = w.loop(ready, lid, w.const(nv), threads, lambda n: at(n, out, nvertex, vertex_at))
        w.loop(end, lid, count, threads, lambda n: at(n, p_start, guest.per_primitive(i), primitive_at))
        w.emit('OpReturn')
    w.emit('OpFunctionEnd')
    stage = 'TaskEXT' if task else 'MeshEXT'
    header[entry] = f'OpEntryPoint {stage} %dm_main "main" ' + ' '.join(globals_ + w.interface)
    mode_at = entry + 1
    while mode_at < len(header) and header[mode_at].lstrip().startswith('OpExecutionMode '):
        header[mode_at] = header[mode_at].replace(original + ' ', '%dm_main ')
        mode_at += 1
    if not task:
        header[mode_at:mode_at] = [f'OpExecutionMode %dm_main {mode}', f'OpExecutionMode %dm_main OutputVertices {nv}',
                                     f'OpExecutionMode %dm_main OutputPrimitivesEXT {np}']
    enable_mesh(header, extra)
    first_type = next(n for n, line in enumerate(header) if (m := DEFINITION.match(line)) and m[2].startswith('OpType'))
    header[first_type:first_type] = w.annotations
    return '\n'.join(header + w.decl + body + w.code) + '\n'


def fragment_input_ids(lines, defs):
    for line in lines:
        if (m := re.fullmatch(rf'\s*OpDecorate ({ID}) Location (\d+)', line)) and defs[m[1]][0] == 'OpVariable':
            var, location = m.groups()
            if defs[var][2] == 'Input':
                yield var, int(location)


def fragment_locations(source, tools):
    lines = subprocess.check_output([tools / 'spirv-dis.exe', '--raw-id', source], text=True).splitlines()
    defs = definitions(lines)
    components = {m[1] for line in lines if (m := re.fullmatch(rf'\s*OpDecorate ({ID}) Component \d+', line))}
    result = set()
    for var, loc in fragment_input_ids(lines, defs):
        _, _, ty = defs[defs[var][1]]
        op, *args = defs[ty]
        if (op != 'OpTypeVector' or args[1] != '4' or defs[args[0]] != ('OpTypeFloat', '32') or
                var in components or loc in result):
            raise ValueError(f'{source.stem}: unsupported translated fragment input at Location {loc}')
        result.add(loc)
    return result


def lower_fragment(text, locations):
    """Mark inputs matched to per-primitive user attributes of a selected mesh."""
    header, body, entry, model, original, globals_ = native_header(text)
    if model != 'Fragment':
        raise ValueError(f'expected a fragment stage, got {model}')
    decorations = [f'OpDecorate {var} PerPrimitiveEXT' for var, loc in
                   fragment_input_ids(header, definitions(header)) if loc in locations]
    if not decorations:
        return None
    header[entry] = f'OpEntryPoint Fragment {original} "main" ' + ' '.join(globals_)
    enable_mesh(header)
    before(header, 'OpDecorate ', decorations)
    return '\n'.join(header + body) + '\n'


def lower_module(source, tools, run, *, stage=None, guest=None, primitive=()):
    text = subprocess.check_output([tools / 'spirv-dis.exe', '--raw-id', source], text=True)
    wrapped = lower_stage(text, stage, guest) if stage else lower_fragment(text, primitive)
    if wrapped is None:
        return source
    assembly = source.with_suffix('.mesh.spvasm')
    output = source.with_suffix('.mesh.spv')
    assembly.write_text(wrapped, encoding='utf-8')
    run(tools / 'spirv-as.exe', '--target-env', 'spv1.5', assembly, '-o', output)
    return output
