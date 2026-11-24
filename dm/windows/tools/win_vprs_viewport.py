"""Windows lowering of an SM86 vertex VPRS table index to Vulkan ViewportIndex."""

import re
import struct
import subprocess

from win_mesh_spirv import DEFINITION, ID, before, definitions, native_header

SPEC_ID = 0x4e565250  # NVRP; DM_M4_VPRS_VIEWPORT_SPEC_ID
MARKER = 'nxnative.vprs.viewport.v1'

SPH_BYTES = 0x80
VPRS_BIT, VIEWPORT_BIT = 423, 426
VPRS_ATTR, VIEWPORT_ATTR, VIEWPORT_MASK_ATTR = 0x5c, 0x68, 0x3a0
ALD, AST, STAGE_OUT = 0x321, 0x322, 0x927
ADDRESS_SHIFT, ADDRESS_MASK = 40, 0x3ff


class UnsupportedVprs(ValueError):
    pass


def vertex_input(instructions):
    """Return (translation bytes, conflict note); never modify the keyed source."""
    if len(instructions) < SPH_BYTES or len(instructions) % 16:
        raise UnsupportedVprs('invalid SM86 vertex instruction range')
    if not instructions[VPRS_BIT // 8] & (1 << (VPRS_BIT % 8)):
        return instructions, None

    native_viewport = bool(instructions[VIEWPORT_BIT // 8] & (1 << (VIEWPORT_BIT % 8)))
    remap = []
    unsupported = None
    for at in range(SPH_BYTES, len(instructions), 16):
        lo, hi = struct.unpack_from('<QQ', instructions, at)
        op = lo & 0xfff
        if op == STAGE_OUT:
            unsupported = unsupported or f'stage-buffer AST at 0x{at:x}'
        if op not in (ALD, AST):
            continue
        # InstEmitAmpereMem.AAst/AAld: Ra, 10-bit byte address, count-1,
        # patch/physical modes, and (for ALD) whether it reads output storage.
        address = (lo >> ADDRESS_SHIFT) & ADDRESS_MASK
        count = ((hi >> 10) & 3) + 1
        patch, physical, output_read = (hi & (1 << bit) for bit in (12, 13, 15))
        indirect = physical or ((lo >> 24) & 0xff) != 0xff
        if indirect or patch:
            if op == AST or output_read:
                unsupported = unsupported or f'indirect or patch attribute access at 0x{at:x}'
            continue

        end = address + count * 4
        native_viewport |= any(address < attr + 4 and attr < end for attr in (VIEWPORT_ATTR, VIEWPORT_MASK_ATTR))
        if address < VPRS_ATTR + 4 and VPRS_ATTR < end:
            if op == AST and address == VPRS_ATTR and count == 1:
                remap.append((at, (lo & ~(ADDRESS_MASK << ADDRESS_SHIFT)) | (VIEWPORT_ATTR << ADDRESS_SHIFT)))
            else:
                unsupported = unsupported or f'non-scalar store or read of VPRS at 0x{at:x}'

    if native_viewport:
        # Preserve native viewport semantics in the unmodified module. Replay
        # must still reject a primitive palette draw that needs this lowering.
        return instructions, 'VPRS viewport lowering unavailable: guest also accesses its native viewport index or mask'
    if unsupported or not remap:
        reason = unsupported or 'SPH declares VPRS but no direct scalar AST was found'
        raise UnsupportedVprs(f'VPRS output is unsupported: {reason}')

    translated = bytearray(instructions)
    translated[VPRS_BIT // 8] &= ~(1 << (VPRS_BIT % 8))
    translated[VIEWPORT_BIT // 8] |= 1 << (VIEWPORT_BIT % 8)
    for at, lo in remap:
        struct.pack_into('<Q', translated, at, lo)
    return translated, None


def lower_assembly(text):
    header, body, entry, model, main, interface = native_header(text)
    if model != 'Vertex':
        raise ValueError('VPRS lowering requires a vertex entry point')
    defs = definitions(header + body)
    builtin = re.compile(rf'\s*OpDecorate ({ID}) BuiltIn (\w+)')
    builtins = [(m[1], m[2]) for line in header if (m := builtin.fullmatch(line))]
    outputs = [var for var, name in builtins if name == 'ViewportIndex']
    if len(outputs) != 1 or outputs[0] not in header[entry].split()[4:]:
        raise ValueError('expected one translated ViewportIndex interface output')
    viewport = outputs[0]
    op, pointer, *args = defs[viewport]
    ptr_op, storage, integer = defs[pointer]
    if (op != 'OpVariable' or args != ['Output'] or ptr_op != 'OpTypePointer' or storage != 'Output'
            or defs[integer] != ('OpTypeInt', '32', '1')):
        raise ValueError('expected a signed 32-bit scalar ViewportIndex output')
    if any(re.fullmatch(rf'\s*OpDecorate {ID} SpecId {SPEC_ID}', line) for line in header):
        raise ValueError('translated vertex shader already uses the VPRS SpecId')

    boolean = next((var for var, shape in defs.items() if shape == ('OpTypeBool',)), None)
    zero = next((var for var, shape in defs.items() if shape == ('OpConstant', integer, '0')), None)
    decl = []
    if boolean is None:
        boolean = '%dm_vprs_bool'
        decl.append(f'{boolean} = OpTypeBool')
    if zero is None:
        zero = '%dm_vprs_zero'
        decl.append(f'{zero} = OpConstant {integer} 0')
    spec = '%dm_vprs_enable'
    decl.append(f'{spec} = OpSpecConstantFalse {boolean}')

    # The source preflight rules out real viewport reads or writes. Verify that
    # the translator kept the remapped scalar value and that the pointer does
    # not escape; rewrite all stores without touching their original operands.
    out = []
    stores = 0
    viewport_use = re.compile(rf'{viewport}\b')
    for line in body:
        if viewport_use.search(line):
            tokens = line.split()
            if (tokens[:2] != ['OpStore', viewport] or len(tokens) < 3 or viewport in tokens[2:]
                    or defs.get(tokens[2], ())[1:2] != (integer,)):
                raise ValueError('unsupported translated ViewportIndex pointer use or store type')
            stores += 1
            selected = f'%dm_vprs_value_{stores}'
            out.append(f'{selected} = OpSelect {integer} {spec} {tokens[2]} {zero}')
            tokens[2] = selected
            line = ' '.join(tokens)
        out.append(line)
    if not stores:
        raise ValueError('the translator dropped all VPRS stores')

    # With the feature disabled the native default (viewport zero) also applies
    # to invocations that return without executing a predicated guest store.
    main_at = next(i for i, line in enumerate(out) if (m := DEFINITION.match(line)) and
                   m[1] == main and m[2] == 'OpFunction')
    first_block = DEFINITION.match(out[main_at + 1])
    if not first_block or first_block[2] != 'OpLabel':
        raise ValueError('unexpected vertex entry-point prologue')
    at = main_at + 2
    while (m := DEFINITION.match(out[at])) and m[2] == 'OpVariable':
        at += 1
    out.insert(at, f'OpStore {viewport} {zero}')

    # Ryujinx writes SPIR-V 1.3 with the combined EXT capability. Vulkan 1.2
    # has independent core features for these built-ins; do not require Layer
    # for ViewportIndex alone. native_header promotes legacy SSBOs and computes
    # the storage interface required by SPIR-V 1.4+.
    header[entry] = f'OpEntryPoint Vertex {main} "main" ' + ' '.join(interface)
    combined = {'OpCapability ShaderViewportIndexLayerEXT', 'OpCapability ShaderViewportIndexLayerNV',
                'OpExtension "SPV_EXT_shader_viewport_index_layer"'}
    header = [line for line in header if line.strip() not in combined]
    caps = ['ShaderViewportIndex']
    if any(name == 'Layer' for _, name in builtins):
        caps.append('ShaderLayer')
    existing = {line.strip() for line in header}
    before(header, ('OpExtension ', 'OpMemoryModel ', '%'),
           [f'OpCapability {cap}' for cap in caps if f'OpCapability {cap}' not in existing])
    before(header, ('OpDecorat', 'OpMemberDecorat', 'OpGroupDecorat', 'OpGroupMemberDecorat'),
           [f'OpModuleProcessed "{MARKER}"', f'OpDecorate {spec} SpecId {SPEC_ID}'])
    return '\n'.join(header + decl + out) + '\n'


def lower_module(source, tools, run):
    text = subprocess.check_output([tools / 'spirv-dis.exe', '--raw-id', source], text=True)
    assembly = source.with_suffix('.vprs.spvasm')
    output = source.with_suffix('.vprs.spv')
    assembly.write_text(lower_assembly(text), encoding='utf-8')
    run(tools / 'spirv-as.exe', '--target-env', 'spv1.5', assembly, '-o', output)
    return output
