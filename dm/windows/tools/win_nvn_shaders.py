#!/usr/bin/env python3
"""Build a key-selected Windows Vulkan shader library from Ocarina v131072.

Inputs: the packed DMROMFS1 image, the in-repo Ocarina shader map/facts, a
win-arm64 build of dmsh_xlate, and Khronos SPIRV-Tools (Vulkan 1.3).
Python needs zstandard. No ExeFS, Metal tools, or loose RomFS are needed.

Output: <key>.spv, <key>.json (diagnostics, ABI v1) and <key>.vkr (runtime).
`key` is the bytes of the NVN2 program's 8-byte hash printed as 16 lowercase
hex digits. The companion carries stage-local descriptor recipes, c1 and
SPIR-V capabilities and is stamped against the exact module bytes. Load it
through m4_shader.h; the entry point is `main`. Descriptors are stage-local
even when the underlying NVN resource is shared.
The module does not remap clip depth; use negativeOneToOne for NVN near=-w.
`frag_write_mask` has the NVN RGBA write-mask nibble per color attachment.

Use --base to copy a validated library into a new output without rebuilding
its modules. --manifest accepts key lists (such as shader_seen.tsv) or Mac
PSO records; use a relevant capture slice rather than the full prewarm seed.
Only modules actually in the output contribute to its device requirements.
--compute-gmem permits checked compute device addresses. Replay must supply
the live pool map and enforce 16-byte pool endpoints and address deltas.
--graphics-gmem also permits the guarded read-only map for vertex/fragment
programs. Graphics global-memory stores are not supported.
Task/mesh keys use MeshEXT with the guest SPH outputs and TaskEXT with the
guest payload. They add mesh.vkc (mesh then task capabilities). The launch
stage takes a uint32 firstTask push constant at offset zero; meshes with a
task stage use the child WorkgroupId instead. Mesh GMEM supports read-only
checked pool access.
--compact-bindless lowers full guest sampled pools to bounded Vulkan arrays
with checked per-binding remaps; it also upgrades reused modules.
Classic SM86 vertex VPRS table-index exports are lowered to ViewportIndex
where the guest does not use the native viewport index or mask. A default-off
NVRP specialization selects the original rate index instead of viewport zero.
This requires the VPRS index to agree across the vertices of each primitive,
as required by Vulkan's ViewportIndex selection.
Use --base with --retranslate-vertices and a key list to upgrade an existing
library from the original RomFS, leaving other base modules in place.

Binding source fields are the translator's NVN metadata:
* support: std140 SupportBuffer (Ryujinx.Graphics.Shader/SupportBuffer.cs).
* cbuf: slot 0/2 = NVN driver bank, 1 = the stage's immutable `c1` hex bytes,
  3..16 = NVN uniform slot slot-3 (zero-pad short ranges to `fixed` bytes),
  24..27 = push blocks, uniform sizes, texel biases, storage lengths in
  dmsh_abi.h. These are buffers, not Vulkan push constants.
* sbuf: address source c[cb_slot] at byte cb_offset*4; if flags & 2, `slot`
  indexes the storage length table. The runtime must resolve NVN addresses to
  Vulkan buffer ranges (or use translated device pointers where required).
* texture/image: `cbuf_slot` and `handle` use Ryujinx TextureHandle packing;
  use NVN's c2 tables in dmsh_abi.h or the named guest cbuf. `tbx` indexes
  the texel bias table. Direct pool arrays index the NVN descriptor IDs.

This tool validates individual modules, not pipelines or device limits. The
runtime must enable the listed SPIR-V features/extensions and check the
actual graphics stage interfaces and task/mesh device limits before pipeline creation.
"""

import argparse
from collections import defaultdict
from functools import cache
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

import win_bindless_spirv
from win_fragment_output import lower_output, output_masks, recorded_colors
import win_vprs_viewport
from win_spirv_legalize import upgrade_read_alias

REPO = Path(__file__).resolve().parents[3]
GFX_TOOLS = REPO / 'dm/gfx/tools'
sys.path.insert(0, str(GFX_TOOLS))
import dmsh_abi as abi
GAME = '0400486024DEC000'
VERSION = 'v131072'
PROFILE = REPO / 'dm/games' / GAME
COLOR_CAPTURE = PROFILE / 'pso_manifest.tsv'
XLATE_SOURCE = GFX_TOOLS / 'dmsh_xlate'
# NVN package prefix -> (SPIR-V execution model, descriptor set).
STAGES = {'vs': (0, 0), 'ps': (4, 1), 'cs': (5, 0), 'ts': (5364, 2), 'ms': (5365, 0)}
MESH_STAGES = ('ts', 'ms')
STAGE_NAMES = {'vs': 'vertex', 'ps': 'fragment', 'hs': 'tessellation control',
               'ds': 'tessellation evaluation', 'gs': 'geometry', 'cs': 'compute',
               'ts': 'task', 'ms': 'mesh'}
DICTIONARY_PATH = 'Pack/ZsDic.pack.zs'
VULKAN_ENV = 'vulkan1.3'
PHYSICAL_STORAGE_BUFFER_ADDRESSES = 5347
NDVE_SOURCE = REPO / 'dm/gfx/nvn/nvn_ndve.inc'
NDVE_ARCHIVE = '/Movie/Ndve/Ndve-Nvn2-vid_seq5.dat.zs#'
VULKAN_CORE = REPO / 'dm/third_party/vulkan-headers/include/vulkan/vulkan_core.h'
SOURCES = {'support': 0, 'cbuf': 1, 'sbuf': 2, 'texture': 3, 'image': 4}
DESCRIPTOR_TYPES = {f'VK_DESCRIPTOR_TYPE_{name}': i for i, name in enumerate((
    'SAMPLER', 'COMBINED_IMAGE_SAMPLER', 'SAMPLED_IMAGE', 'STORAGE_IMAGE',
    'UNIFORM_TEXEL_BUFFER', 'STORAGE_TEXEL_BUFFER', 'UNIFORM_BUFFER', 'STORAGE_BUFFER'))}

# Little-endian disk ABI shared with m4_shader.c; then uint32 capabilities
# followed by the raw c1 bytes. Never serialize native struct padding.
REFLECTION_HEADER = struct.Struct('<8s8sQ14I')
REFLECTION_BINDING = struct.Struct('<12I')
REFLECTION_GMEM_CHECKED = 1


@cache
def title_compute_keys():
    """Read the first model's ordered dispatch list, not all networks in the archive."""
    table = re.search(r'\bk_ndve_keys\[NDVE_N\]\[17\] = \{([^}]+)\}', NDVE_SOURCE.read_text(encoding='utf-8'))
    if not table:
        raise ValueError(f'{NDVE_SOURCE}: missing NDVE dispatch list')
    sequence = re.findall(r'"([0-9a-f]{16})"', table[1])
    if len(sequence) != 14 or len(set(sequence)) != 11:
        raise ValueError(f'{NDVE_SOURCE}: unrecognized first Rouleau model')
    return frozenset(sequence)


def shader_rows(keys):
    rows = {}
    with (PROFILE / 'shader_map.tsv').open(encoding='utf-8') as table:
        for row in table:
            if not row.strip() or row.startswith('#'):
                continue
            package, _, _, key, source = row.rstrip('\n').split('\t', 4)
            if key in keys:
                rows[key] = package[:2], source
    if missing := keys - rows.keys():
        raise ValueError(f'keys absent from the {GAME} {VERSION} shader map: {", ".join(sorted(missing))}')
    for key in keys & title_compute_keys():
        if rows[key][0] != 'cs' or not rows[key][1].startswith(NDVE_ARCHIVE):
            raise ValueError(f'{key}: first Rouleau shader is not mapped to {NDVE_ARCHIVE[:-1]}')
    return rows


def read_shaders(rows, romfs, split, index_reader, zstd):
    """Read only registered source archives for requested keys, once each."""

    index = index_reader(romfs)
    groups = defaultdict(set)
    for key, (_, source) in rows.items():
        # In a SARC source label, the guest member follows the actual RomFS path.
        path = source.split('#', 1)[0].lstrip('/')
        while path and (path not in index or index[path][0]):
            path = path.rpartition('/')[0]
        if not path:
            raise ValueError(f'{key}: {source} is not in {romfs}')
        groups[path].add(key)

    with romfs.open('rb') as image:
        def read(path):
            is_dir, offset, size = index[path]
            image.seek(offset)
            data = image.read(size)
            if is_dir or len(data) != size:
                raise ValueError(f'{romfs}: cannot read {path}')
            return data

        plain = zstd.ZstdDecompressor()
        dictionaries = {}
        for path, wanted in sorted(groups.items()):
            raw = read(path)
            if raw[:4] == split.ZSTD_MAGIC:
                dict_id = split.zs_dict_id(raw)
                if dict_id and not dictionaries:
                    archive = plain.decompress(read(DICTIONARY_PATH))
                    for _, data in split.sarc_members(archive, '/' + DICTIONARY_PATH):
                        if data[:4] == b'\x37\xa4\x30\xec':
                            dictionary = zstd.ZstdCompressionDict(data)
                            dictionaries[dictionary.dict_id()] = zstd.ZstdDecompressor(dict_data=dictionary)
                if dict_id and dict_id not in dictionaries:
                    raise ValueError(f'{path}: missing zstd dictionary {dict_id}')
                raw = (dictionaries[dict_id] if dict_id else plain).decompress(raw)
            found = []
            split.scan_blob(raw, '/' + path, found)
            # Match both content hash and stage. A label can move to a different
            # variation without changing the code or key the game will supply.
            chosen = {}
            for where, stage, ctl, code in found:
                key = split.skey(ctl, code)
                if key in wanted and stage == rows[key][0]:
                    previous = chosen.setdefault(key, (where, stage, ctl, code))
                    if not split.same_shader(previous[2:], (ctl, code)):
                        raise ValueError(f'{key}: conflicting records in {path}')
                    if where == rows[key][1]:
                        chosen[key] = where, stage, ctl, code
            if missing := wanted - chosen.keys():
                raise ValueError(f'{path}: does not contain the registered NVN2 shader(s) {", ".join(sorted(missing))}')
            yield from sorted(chosen.items())


def translator_job(key, shader, facts, work, ampere, abi, meshstage):
    """The ordinary NVN2 job layout from dmsh_build.job_for, without Metal imports."""
    _, stage, ctl, code = shader
    if not ampere.is_nvn2(ctl) or ampere.code_len(ctl) != len(code):
        raise ValueError(f'{key}: expected an NVN2 control section and its full code area')
    inst = ampere.inst_len(ctl)
    c1_start = struct.unpack_from('<I', ctl, 0x40)[0]
    if not 0 < inst <= c1_start <= len(code) or inst % 16:
        raise ValueError(f'{key}: invalid NVN2 instruction or c1 range')
    cp = work / (key + '.bin')
    instructions = code[:inst]
    lower_vprs, vprs_note = False, None
    if stage == 'vs':
        original = instructions
        instructions, vprs_note = win_vprs_viewport.vertex_input(original)
        lower_vprs = instructions is not original
    job = {'tex': {}, 'img': {}, 'attr': {}, 'fout': {}, **facts.get(key, {}),
           'key': key, 'stage': stage, 'arch': 'sm86', 'code': str(cp),
           'c1': ampere.c1_segment(ctl, code).hex(), 'debug': False}
    if stage == 'cs':
        if len(ctl) < 0xbc:
            raise ValueError(f'{key}: missing NVN2 compute layout')
        x, y, z, shared = struct.unpack_from('<4I', ctl, 0x58)
        if not x * y * z:
            raise ValueError(f'{key}: zero NVN2 compute group dimension')
        job.update(local=[x, y, z], shared=(shared + 15) & ~15,
                   nvn2_cs=[ctl[0xb1], ctl[0xb2], ctl[0xb3], struct.unpack_from('<H', ctl, 0xba)[0],
                            ctl[0xa9] * 4, ctl[0xb4], ctl[0xb5], ctl[0xaa] * 4])
    elif stage in MESH_STAGES:
        if len(ctl) < (0x69 if stage == 'ms' else 0x5c) or meshstage.is_stage(ctl) != stage or inst <= 0x80:
            raise ValueError(f'{key}: missing NVN2 {STAGE_NAMES[stage]} layout')
        mesh = meshstage.info(ctl, code)
        if not mesh['threads']:
            raise ValueError(f'{key}: zero NVN2 {STAGE_NAMES[stage]} group dimension')
        if stage == 'ms' and (mesh['verts'] not in meshstage.TOPOLOGY or not mesh['max_v'] or not mesh['max_p']):
            raise ValueError(f'{key}: invalid NVN2 mesh output limits or topology')
        instructions = instructions[0x80:]
        if meshstage.own_shared(instructions):
            raise ValueError(f'{key}: shader shared memory overlaps Ampere stage storage')
        layout, size = meshstage.memory(mesh)
        job.update(stage='cs', local=[mesh['threads'], 1, 1], shared=size,
                   stage_memory=layout, mesh_stage=mesh)
        if stage == 'ms':
            job['stage_bank'] = abi.NVN2_STAGE_BANK + 1
    else:
        job['stage_bank'] = abi.NVN2_STAGE_BANK + (4 if stage == 'ps' else 0)
        if stage == 'ps' and ampere.early_fragment_tests(ctl):
            job['early_fragment_tests'] = not (struct.unpack_from('<I', code, 19 * 4)[0] & 2)
    cp.write_bytes(instructions)
    return job, lower_vprs, vprs_note


def spirv_string(words):
    return struct.pack(f'<{len(words)}I', *words).partition(b'\0')[0].decode('utf-8')


def remap_spirv(raw, stage, *, already_remapped=False):
    """Rebase DescriptorSet decorations; return exact declared bindings and capabilities."""
    if len(raw) < 20 or len(raw) % 4:
        raise ValueError('truncated SPIR-V module')
    words = list(struct.unpack(f'<{len(raw) // 4}I', raw))
    if words[0] != 0x07230203:
        raise ValueError('missing SPIR-V module magic')
    model, stage_set = STAGES[stage]
    expected_set = stage_set if already_remapped else 0
    caps, extensions, entry, bindings, sets = set(), set(), [], {}, set()
    i = 5
    while i < len(words):
        op, count = words[i] & 0xffff, words[i] >> 16
        if not count or i + count > len(words):
            raise ValueError('invalid SPIR-V instruction extent')
        if op == 10:  # OpExtension
            extensions.add(spirv_string(words[i + 1:i + count]))
        elif op == 15:  # OpEntryPoint
            entry.append((words[i + 1], spirv_string(words[i + 3:i + count])))
        elif op == 17:  # OpCapability
            caps.add(words[i + 1])
        elif op == 71 and count == 4:  # OpDecorate
            target, decoration, value = words[i + 1:i + 4]
            if decoration == 33:  # Binding
                bindings[target] = value
            elif decoration == 34:  # DescriptorSet
                if value != expected_set:
                    raise ValueError(f'unexpected descriptor set {value}, expected {expected_set}')
                words[i + 3] = stage_set
                sets.add(target)
        i += count
    if entry != [(model, 'main')]:
        raise ValueError(f'expected a single {stage} entry point named main, found {entry}')
    if sets != bindings.keys() or len(set(bindings.values())) != len(bindings):
        raise ValueError('conflicting SPIR-V descriptor decorations')
    return (raw if already_remapped else struct.pack(f'<{len(words)}I', *words),
            set(bindings.values()), sorted(caps), sorted(extensions))


def descriptor_type(kind, meta):
    if kind in ('support', 'cbuf'):
        return 'VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER'
    if kind == 'sbuf':
        return 'VK_DESCRIPTOR_TYPE_STORAGE_BUFFER'
    tex_type = meta['type'] & 0xff  # Ryujinx SamplerType.Mask
    if kind == 'image':
        return 'VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER' if tex_type == 2 else 'VK_DESCRIPTOR_TYPE_STORAGE_IMAGE'
    if tex_type == 0:
        return 'VK_DESCRIPTOR_TYPE_SAMPLER'
    if tex_type == 2:
        return 'VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER'
    return 'VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE' if meta['separate'] else 'VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER'


def runtime_bindings(res, declared):
    by_binding = {}
    for group, kind in [('cbufs', 'cbuf'), ('sbufs', 'sbuf'), ('textures', 'texture'), ('images', 'image')]:
        for meta in res[group]:
            binding = meta['binding']
            if binding in by_binding:
                raise ValueError(f'duplicate reflection for descriptor binding {binding}')
            by_binding[binding] = dict(meta, source=kind,
                                       descriptor_type=descriptor_type(kind, meta), count=meta.get('array', 1))
    # The translator declares its built-in support UBO separately from CBuffers.
    if 0 in declared:
        by_binding[0] = {'binding': 0, 'source': 'support', 'count': 1,
                         'descriptor_type': descriptor_type('support', {})}
    if declared - by_binding.keys():
        raise ValueError(f'SPIR-V bindings lack reflection: {sorted(declared - by_binding.keys())}')
    return [by_binding[n] for n in sorted(declared)]


@cache
def vk_formats():
    return {name: int(value) for name, value in re.findall(
        r'^\s*(VK_FORMAT_\w+) = (\d+),', VULKAN_CORE.read_text(encoding='utf-8'), flags=re.M)}


def vk_format(fmt):
    if fmt == 'Unknown':
        return 0  # VK_FORMAT_UNDEFINED
    if fmt == 'R11G11B10Float':
        name = 'B10G11R11_UFLOAT_PACK32'
    elif fmt in ('R10G10B10A2Unorm', 'R10G10B10A2Uint'):
        name = f'A2B10G10R10_{fmt.removeprefix("R10G10B10A2").upper()}_PACK32'
    else:
        suffix = {'Unorm': '_UNORM', 'Snorm': '_SNORM', 'Uint': '_UINT',
                  'Sint': '_SINT', 'Float': '_SFLOAT'}
        match = re.fullmatch(r'((?:[RGBA]\d+)+)(Unorm|Snorm|Uint|Sint|Float)', fmt)
        if not match:
            raise ValueError(f'unsupported image format {fmt}')
        name = match[1] + suffix[match[2]]
    return vk_formats()['VK_FORMAT_' + name]


def pack_binding(b):
    source = b['source']
    if b['descriptor_type'] != descriptor_type(source, b) or b['count'] != b.get('array', 1):
        raise ValueError(f'inconsistent descriptor recipe at binding {b["binding"]}')
    if source == 'support':
        data = ()
    elif source == 'cbuf':
        data = (b['slot'], b['fixed'])
    elif source == 'sbuf':
        data = (b['slot'], b['cb_slot'], b['cb_offset'], b['flags'])
        if b['cb_slot'] == win_bindless_spirv.MAP_CB_SLOT:
            data += (b['record_stride'], b['record_words'])
    elif source in ('texture', 'image'):
        data = (b['cbuf_slot'] & 0xffffffff, b['handle'] & 0xffffffff,
                b['type'], vk_format(b['format']), b['flags'],
                b.get('tbx', -1) & 0xffffffff, b['separate'], b.get('row_bias', False))
    else:
        raise ValueError(f'unsupported descriptor source {source}')
    return REFLECTION_BINDING.pack(b['binding'], DESCRIPTOR_TYPES[b['descriptor_type']],
                                   b['count'], SOURCES[source], *data, *([0] * (8 - len(data))))


def spirv_hash(raw):
    digest = 0xcbf29ce484222325  # FNV-1a: detect accidental stale/mixed sidecars
    for byte in raw:
        digest = ((digest ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return digest


def reflection_blob(info, raw):
    stage = info['stage']
    model, stage_set = STAGES[stage]
    c1 = bytes.fromhex(info['c1'])
    caps = info['capabilities']
    gmem_checked = info.get('gmem_checked') is True
    if gmem_checked != (PHYSICAL_STORAGE_BUFFER_ADDRESSES in caps):
        raise ValueError('inconsistent checked global-memory metadata; rebuild with --compute-gmem')
    uint, sint = info.get('frag_uint_mask', 0), info.get('frag_sint_mask', 0)
    if not (0 <= uint <= 0xff and 0 <= sint <= 0xff) or uint & sint or ((uint or sint) and stage != 'ps'):
        raise ValueError('invalid integer fragment output metadata')
    flags = (REFLECTION_GMEM_CHECKED if gmem_checked else 0) | (uint << 8) | (sint << 16)
    return b''.join((
        REFLECTION_HEADER.pack(b'DMVKREF1', bytes.fromhex(info['key']), spirv_hash(raw), len(raw),
                               model, stage_set, len(info['bindings']), len(c1), len(caps),
                               info.get('frag_write_mask', 0), info.get('frag_out_map', 0),
                               info.get('uses_frag_coord', False), *info.get('local', (0, 0, 0)),
                               info.get('shared', 0), flags),
        *(pack_binding(b) for b in info['bindings']),
        struct.pack(f'<{len(caps)}I', *caps),
        c1,
    ))


def checked_key(key):
    if not re.fullmatch(r'[0-9a-f]{16}', key):
        raise ValueError(f'{key!r}: expected a 16-digit lowercase hex shader key')
    return key


def manifest_keys(path):
    """Read a key list or the shader identities from Mac PSO records."""
    def capture_key(value):
        return checked_key(value.removesuffix('_b'))

    with path.open(encoding='utf-8') as table:
        for line, row in enumerate(table, 1):
            row = row.strip()
            if not row or row.startswith('#'):
                continue
            fields = row.split('\t')
            try:
                kind = fields[0]
                if kind == 'S' and len(fields) == 3 and fields[2].isdigit():
                    yield capture_key(fields[1]), None
                elif kind == 'C' and len(fields) == 2:
                    yield capture_key(fields[1]), 'cs'
                elif (kind, len(fields)) in (('R', 2), ('M', 3)):
                    # nvn_pso_skey.h: two 24-byte NUL-padded stage names.
                    raw = bytes.fromhex(fields[-1])
                    if len(raw) < 50:
                        raise ValueError('truncated PSO record')
                    first = raw[:24].rstrip(b'\0').decode('ascii')
                    second = raw[24:48].rstrip(b'\0').decode('ascii')
                    if kind == 'M' and fields[1] != '-':
                        yield capture_key(fields[1]), 'ts'
                    yield capture_key(first), 'vs' if kind == 'R' else 'ms'
                    if second:
                        yield capture_key(second), 'ps'
                else:
                    yield checked_key(kind), None
            except ValueError as exc:
                raise ValueError(f'{path}:{line}: {exc}') from exc


def selected_keys(args):
    keys = {checked_key(k): None for k in args.keys}
    if args.ndve_title:
        keys.update(dict.fromkeys(title_compute_keys(), 'cs'))
    for manifest in args.manifest:
        for key, stage in manifest_keys(manifest):
            expected = keys.setdefault(key, stage)
            if stage:
                if expected and stage != expected:
                    raise ValueError(f'{manifest}: conflicting stages {expected} and {stage} for {key}')
                keys[key] = stage
    return keys


def mesh_partners(rows, reused):
    """Relevant producer/fragment pairs, including fragments from the base library."""
    stages = {key: info['stage'] for key, info in reused.items()}
    stages.update({key: stage for key, (stage, _) in rows.items()})
    meshes, ordinary = defaultdict(list), set()
    with (PROFILE / 'shader_pairs.tsv').open(encoding='utf-8') as table:
        for line in table:
            if not line.strip() or line.startswith('#'):
                continue
            producer, fragment = line.split()
            if stages.get(fragment) != 'ps':
                continue
            if stages.get(producer) == 'ms' and producer in rows:
                meshes[producer].append(fragment)
            elif stages.get(producer) == 'vs':
                ordinary.add(fragment)
    return meshes, ordinary


def write_capabilities(directory, shaders):
    """Only shaders available to replay may contribute device requirements."""
    caps = {stage: set() for stage in STAGES}
    for info in shaders:
        caps[info['stage']].update(info['capabilities'])
    vs, ps = sorted(caps['vs']), sorted(caps['ps'])
    ms, ts = sorted(caps['ms']), sorted(caps['ts'])
    if len(vs) + len(ps) > 256 or len(caps['cs']) > 256 or len(ms) + len(ts) > 256:
        raise ValueError('capability manifest exceeds replay limits')
    graphics = vs + ps
    (directory / 'graphics.vkc').write_bytes(
        struct.pack(f'<8sII{len(graphics)}I', b'DMVKCAP1', len(vs), len(ps), *graphics))
    compute = directory / 'compute.vkc'
    if cs := sorted(caps['cs']):
        compute.write_bytes(struct.pack(f'<8sI{len(cs)}I', b'DMVKCSC1', len(cs), *cs))
    else:
        compute.unlink(missing_ok=True)
    mesh = directory / 'mesh.vkc'
    if ms or ts:
        mesh.write_bytes(struct.pack(f'<8sII{len(ms) + len(ts)}I', b'DMVKMSC1', len(ms), len(ts), *ms, *ts))
    else:
        mesh.unlink(missing_ok=True)


def load_reflection(directory, key):
    info = json.loads((directory / (key + '.json')).read_text(encoding='utf-8'))
    raw = (directory / (key + '.spv')).read_bytes()
    stage = info['stage']
    if (info['abi'], info['game'], info['version'], info['key'], info['entry'], info['set']) != (
            1, GAME, VERSION, key, 'main', STAGES[stage][1]):
        raise ValueError('not matching Windows shader reflection v1')
    _, declared, caps, exts = remap_spirv(raw, stage, already_remapped=True)
    if ([b['binding'] for b in info['bindings']] != sorted(declared) or
            info['capabilities'] != caps or info['extensions'] != exts):
        raise ValueError('reflection does not match the SPIR-V module')
    return info, reflection_blob(info, raw)


def read_library(directory, *, reflect=()):
    """Validate every loadable triplet, plus any modules selected for reflection."""
    if not directory.is_dir() or directory.is_relative_to(REPO):
        raise ValueError(f'{directory}: use a shader library directory outside the checkout')
    keys = {p.stem for p in directory.glob('*.vkr')}
    shaders, updated = {}, {}
    for key in sorted(keys | set(reflect)):
        try:
            checked_key(key)
            info, reflection = load_reflection(directory, key)
            if key in reflect:
                updated[key] = reflection
            elif (directory / (key + '.vkr')).read_bytes() != reflection:
                raise ValueError('runtime sidecar does not match the shader and diagnostic metadata')
            shaders[key] = info
        except (KeyError, OSError, TypeError, ValueError, struct.error) as exc:
            raise ValueError(f'{directory}: {key}: {exc}') from exc
    if not shaders:
        raise ValueError(f'{directory}: no runtime shaders found')
    return shaders, updated


def reflect_existing(directory, keys):
    """Add runtime reflection to an existing, validated v1 Windows shader library."""
    keys = set(keys) or {path.stem for path in directory.glob('*.json')}
    shaders, updated = read_library(directory, reflect=keys)
    for key, reflection in updated.items():
        (directory / (key + '.vkr')).write_bytes(reflection)
    write_capabilities(directory, shaders.values())
    print(f'added runtime reflection for {len(updated)} NVN2 shaders: {directory}')


def verify_gmem(bindings, stage, caps, allow, map_slot):
    gmem = [b for b in bindings if b['source'] == 'sbuf' and b['cb_slot'] == map_slot]
    if not gmem:
        if PHYSICAL_STORAGE_BUFFER_ADDRESSES in caps:
            raise ValueError('physical addresses without a reflected global-memory pool binding')
        return False
    desc = ', '.join(f'{b["binding"]} (offset {b["cb_offset"]}, flags {b["flags"]})' for b in gmem)
    if not allow:
        option = '--compute-gmem' if stage == 'cs' else '--graphics-gmem'
        raise ValueError(f'{stage} global-memory pool/log storage bindings {desc} require {option}')
    # Compute can also reflect a write log; graphics stages support reads only.
    recipes = sorted((b['cb_offset'], b['flags'], b['count']) for b in gmem)
    variants = ([(0, 0, 1)], [(0, 0, 1), (1, 1, 1)]) if stage == 'cs' else ([(0, 0, 1)],)
    if PHYSICAL_STORAGE_BUFFER_ADDRESSES not in caps or recipes not in variants:
        raise ValueError(f'unsupported {stage} global-memory binding ABI: {desc}; capabilities {caps}')
    return True


def run_spirv_tool(*command):
    result = subprocess.run(command, capture_output=True, text=True, errors='replace')
    if result.returncode:
        diagnostic = result.stderr.strip() or result.stdout.strip()
        raise ValueError(f'{Path(command[0]).name} exited {result.returncode}: {diagnostic}')


def compact_shader(args, raw, info, work):
    if not args.spirv_val:
        raise ValueError('install Khronos spirv-val or pass --spirv-val PATH')
    tools = Path(args.spirv_val).resolve().parent
    raw, bindings = win_bindless_spirv.lower_module(raw, info['bindings'], info['set'],
                                                   work / info['key'], tools, run_spirv_tool)
    _, declared, caps, exts = remap_spirv(raw, info['stage'], already_remapped=True)
    if declared != {b['binding'] for b in bindings}:
        raise ValueError('compact descriptor map and SPIR-V bindings differ')
    info.update(bindings=bindings, capabilities=caps, extensions=exts)
    return raw


def read_only_gmem_compute(info):
    return (info['stage'] == 'cs' and info.get('gmem_checked') and not any(
        b['source'] == 'sbuf' and b['cb_slot'] == abi.GMEM_MAP_CB_SLOT and b['cb_offset'] == 1
        for b in info['bindings']))


def build_shaders(args, rows, work, output, reused):
    if not (args.romfs and args.xlate):
        raise ValueError('translating new keys requires --romfs and --xlate')
    if not args.spirv_val:
        raise ValueError('install Khronos spirv-val or pass --spirv-val PATH')
    tools = Path(args.spirv_val).resolve().parent
    spirv_opt = tools / 'spirv-opt.exe'
    failures = []
    source_sha = hashlib.sha256((XLATE_SOURCE / 'Program.cs').read_bytes() +
                                (XLATE_SOURCE / 'dmsh_xlate.csproj').read_bytes()).hexdigest()[:16]
    actual_sha = subprocess.check_output([args.xlate, '--source-sha'], text=True).strip()
    if actual_sha != source_sha:
        raise ValueError(f'{args.xlate}: expected translator source {source_sha}, got {actual_sha}')

    # These shared tools use DM_GAME to locate the exact profile at import time.
    os.environ['DM_GAME'] = GAME
    sys.path.insert(0, str(REPO / 'dm/tools'))
    import dmsh_ampere as ampere
    import dmsh_facts
    import dmsh_meshstage
    from romfs_image import read_index
    import shader_split as split
    import zstandard
    from win_ndve_spirv import lower_module
    import win_mesh_spirv
    from win_spirv_legalize import legalize

    jobs, frag_masks, vprs_vertices, vprs_notes = [], {}, set(), {}
    facts = dmsh_facts.load()
    sources = {}
    for key, shader in read_shaders(rows, args.romfs, split, read_index, zstandard):
        sources[key] = shader[0]
        try:
            job, lower_vprs, vprs_note = translator_job(key, shader, facts, work, ampere, abi, dmsh_meshstage)
        except win_vprs_viewport.UnsupportedVprs as exc:
            failures.append(f'{key} ({STAGE_NAMES[shader[1]]}, {sources[key]}): {exc}')
            continue
        if lower_vprs:
            vprs_vertices.add(key)
        if vprs_note:
            vprs_notes[key] = vprs_note
        jobs.append(job)
        if shader[1] == 'ps':
            frag_masks[key] = struct.unpack_from('<I', shader[3], 18 * 4)[0]
    if not jobs:
        return {}, failures
    spec = work / 'jobs.json'
    spec.write_text(json.dumps(jobs), encoding='utf-8')
    translated = work / 'translated'
    result = subprocess.run([args.xlate, spec, translated])
    if result.returncode not in (0, 1):
        result.check_returncode()

    partners, ordinary = mesh_partners(rows, reused) if any('mesh_stage' in job for job in jobs) else ({}, set())
    fragment_inputs, per_primitive = {}, {}
    for job in jobs:
        mesh = job.get('mesh_stage')
        if not mesh or mesh['kind'] != 'ms':
            continue
        needed = set()
        primitive_locations = {n for n, _ in mesh['primitive']}
        for fs in partners.get(job['key'], ()):
            if fs not in fragment_inputs:
                directory = output if fs in reused else translated
                fragment_inputs[fs] = win_mesh_spirv.fragment_locations(directory / (fs + '.spv'), tools)
            inputs = fragment_inputs[fs]
            needed.update(inputs)
            primitive = primitive_locations & inputs
            prior = per_primitive.setdefault(fs, primitive)
            if prior != primitive or (primitive and (fs in ordinary or fs in reused)):
                raise ValueError(f'{job["key"]}/{fs}: fragment has incompatible per-primitive interfaces in this library')
        mesh['fragment_locations'] = needed
    shaders = {}
    for job in jobs:
        key, body_stage = job['key'], job['stage']
        mesh = job.get('mesh_stage')
        stage = mesh['kind'] if mesh else body_stage
        try:
            metadata = translated / (key + '.res.json')
            if result.returncode and not metadata.is_file():
                raise ValueError('translation failed (see translator diagnostic)')
            res = json.loads(metadata.read_text(encoding='utf-8'))
            if res['key'] != key or res['stage'] != body_stage:
                raise ValueError('translator reflection key or stage does not match the job')
            if lost := next((n for n in res['notes'] if 'Failed to find handle source for bindless access' in n), None):
                raise ValueError(lost)
            source = translated / (key + '.spv')
            compute = {}
            if key in title_compute_keys():
                source, shared = lower_module(source, res, abi.GMEM_MAP_CB_SLOT, tools, job['local'])
                compute.update(shared=shared, guest_shared=job['shared'])
            guard_gmem = bool(mesh or (args.compute_gmem and stage == 'cs') or
                              (args.graphics_gmem and stage in ('vs', 'ps')))
            original = source.read_bytes()
            legalized = legalize(original, guard_gmem=guard_gmem)
            if legalized is not original:
                source = translated / (key + '.legal.spv')
                source.write_bytes(legalized)
            if mesh or per_primitive.get(key):
                if mesh:
                    mesh['shared'] = job['shared']
                source = win_mesh_spirv.lower_module(source, tools, run_spirv_tool,
                                                     stage=mesh, guest=dmsh_meshstage, primitive=per_primitive.get(key))
            uint = sint = 0
            if stage == 'ps':
                uint, sint = output_masks(recorded_colors(COLOR_CAPTURE), key, frag_masks[key])
                if uint or sint:
                    numeric = translated / (key + '.numeric.spv')
                    numeric.write_bytes(lower_output(source.read_bytes(), uint, sint))
                    source = numeric
            # Drop unreferenced descriptors before writing replay recipes.
            optimized = translated / (key + '.opt.spv')
            run_spirv_tool(spirv_opt, '--target-env=' + VULKAN_ENV, '--eliminate-dead-variables',
                           '-o', optimized, source)
            if key in vprs_vertices:
                optimized = win_vprs_viewport.lower_module(optimized, tools, run_spirv_tool)
            raw, declared, caps, exts = remap_spirv(optimized.read_bytes(), stage)
            bindings = runtime_bindings(res, declared)
            gmem = verify_gmem(bindings, stage, caps, guard_gmem, abi.GMEM_MAP_CB_SLOT)
            info = {'abi': 1, 'game': GAME, 'version': VERSION, 'translator_source': source_sha,
                    'source': sources[key],
                    'key': key, 'stage': stage, 'entry': 'main', 'set': STAGES[stage][1],
                    'bindings': bindings, 'c1': job['c1'], 'capabilities': caps,
                    'extensions': exts, 'notes': res['notes']}
            if key in vprs_notes:
                info['notes'].append(vprs_notes[key])
            if gmem:
                info['gmem_checked'] = True
            if stage == 'ps':
                # Ryujinx exposes the guest's 32-bit mask as a signed int.
                info.update(frag_write_mask=frag_masks[key], frag_out_map=res['frag_out_map'] & 0xffffffff,
                            uses_frag_coord=res['uses_frag_coord'])
                if uint or sint:
                    info.update(frag_uint_mask=uint, frag_sint_mask=sint)
            if mesh:
                info.update(local=job['local'], shared=job['shared'],
                            mesh={name: mesh[name] for name in ('payload', 'task', 'verts', 'max_v', 'max_p') if name in mesh})
            if stage == 'cs':
                info.update(local=job['local'], shared=job['shared'])
                info.update(compute)
            if args.compact_bindless and win_bindless_spirv.needs_lowering(bindings):
                raw = compact_shader(args, raw, info, work)
            if read_only_gmem_compute(info):
                raw = upgrade_read_alias(raw)
            reflection = reflection_blob(info, raw)
            module = translated / (key + '.vk.spv')
            module.write_bytes(raw)
            run_spirv_tool(args.spirv_val, '--target-env', VULKAN_ENV, module)
        except (IndexError, KeyError, OSError, TypeError, ValueError, struct.error, subprocess.CalledProcessError) as exc:
            failures.append(f'{key} ({STAGE_NAMES[stage]}, {sources[key]}): {exc}')
            continue
        module.rename(output / (key + '.spv'))
        (output / (key + '.json')).write_text(json.dumps(info, indent=2) + '\n', encoding='utf-8')
        (output / (key + '.vkr')).write_bytes(reflection)
        shaders[key] = info
    if not failures:
        result.check_returncode()
    return shaders, failures


def run_build(args):
    if os.name != 'nt':
        raise ValueError('use this offline builder on Windows')
    if args.out.exists():
        raise ValueError(f'{args.out} already exists; use a new output directory')
    if args.out.is_relative_to(REPO):
        raise ValueError('the shader output must be outside the repository')
    if args.base and args.out.is_relative_to(args.base):
        raise ValueError('the shader output must not be inside its immutable base library')
    keys = selected_keys(args)
    shaders = read_library(args.base)[0] if args.base else {}
    for key, stage in keys.items():
        if key in shaders and stage and shaders[key]['stage'] != stage:
            raise ValueError(f'{key}: manifest requires {stage}, base has {shaders[key]["stage"]}')
    replace = {key for key in keys.keys() & shaders.keys()
               if args.retranslate_vertices and shaders[key]['stage'] == 'vs'}
    translate = (keys.keys() - shaders.keys()) | replace
    rows = shader_rows(translate) if translate else {}
    failures = []
    for key, (stage, source) in list(rows.items()):
        if keys[key] and stage != keys[key]:
            raise ValueError(f'{key}: manifest requires {keys[key]}, shader map has {stage} ({source})')
        if key in replace and stage != 'vs':
            raise ValueError(f'{key}: base has a vertex shader but the shader map has {stage} ({source})')
        if stage not in STAGES:
            failures.append(f'{key}: NVN2 {STAGE_NAMES.get(stage, stage)} ({stage}) '
                            f'has no Windows Vulkan lowering ({source})')
            del rows[key]
    if not (keys or shaders):
        raise ValueError('no shader keys selected')
    if failures and not args.allow_partial:
        raise ValueError('\n[RED] '.join(failures))
    for key in replace:
        del shaders[key]

    args.out.parent.mkdir(parents=True, exist_ok=True)
    updated = 0
    with tempfile.TemporaryDirectory(prefix='nvn-vk-', dir=args.out.parent) as scratch:
        work = Path(scratch)
        output = work / 'library'
        output.mkdir()
        for key, info in shaders.items():
            module = output / (key + '.spv')
            raw = None
            if key in keys and info['stage'] == 'ps':
                uint, sint = output_masks(recorded_colors(COLOR_CAPTURE), key, info['frag_write_mask'])
                old = info.get('frag_uint_mask', 0), info.get('frag_sint_mask', 0)
                if (uint, sint) != old:
                    if old != (0, 0):
                        raise ValueError(f'{key}: the base uses different integer fragment outputs; rebuild it')
                    raw = lower_output((args.base / module.name).read_bytes(), uint, sint)
                    info.update(frag_uint_mask=uint, frag_sint_mask=sint)
            if args.compact_bindless and win_bindless_spirv.needs_lowering(info['bindings']):
                raw = compact_shader(args, raw if raw is not None else (args.base / module.name).read_bytes(), info, work)
            if args.compute_gmem and read_only_gmem_compute(info):
                original = raw if raw is not None else (args.base / module.name).read_bytes()
                upgraded = upgrade_read_alias(original)
                if upgraded != original:
                    raw = upgraded
            if raw is not None:
                if not args.spirv_val:
                    raise ValueError('install Khronos spirv-val or pass --spirv-val PATH')
                module.write_bytes(raw)
                run_spirv_tool(args.spirv_val, '--target-env', VULKAN_ENV, module)
                (output / (key + '.json')).write_text(json.dumps(info, indent=2) + '\n', encoding='utf-8')
                (output / (key + '.vkr')).write_bytes(reflection_blob(info, raw))
                updated += 1
                continue
            for suffix in ('.spv', '.json', '.vkr'):
                filename = key + suffix
                shutil.copyfile(args.base / filename, output / filename)
        if rows:
            new, failed = build_shaders(args, rows, work, output, shaders)
            failures.extend(failed)
        else:
            new = {}
        if failures and (not args.allow_partial or not (new or shaders)):
            raise ValueError('\n[RED] '.join(failures))
        write_capabilities(output, (*shaders.values(), *new.values()))
        output.rename(args.out)
    print(f'{len(new)} translated, {updated} updated, {len(shaders) - updated} reused '
          f'for {VULKAN_ENV}: {args.out}', flush=True)
    if failures:
        raise ValueError('partial library published; omitted shaders:\n[RED] ' + '\n[RED] '.join(failures))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n', 1)[0])
    parser.add_argument('--romfs', type=Path, help='packed Ocarina v131072 RomFS.img')
    parser.add_argument('--xlate', type=lambda p: Path(p).resolve(), help='native dmsh_xlate.exe')
    parser.add_argument('--spirv-val', default=shutil.which('spirv-val'),
                        help='Khronos spirv-val.exe beside spirv-opt.exe, spirv-dis.exe and spirv-as.exe')
    parser.add_argument('--out', type=lambda p: Path(p).resolve(), help='new library directory outside the checkout')
    parser.add_argument('--base', type=lambda p: Path(p).resolve(), help='immutable library to carry into the new output')
    parser.add_argument('--retranslate-vertices', action='store_true',
                        help='replace selected classic vertex shaders already in --base using their original NVN2 inputs')
    parser.add_argument('--manifest', type=Path, action='append', default=[],
                        help='add keys from a key list or Mac PSO capture (repeatable)')
    parser.add_argument('--allow-partial', action='store_true',
                        help='publish validated successes despite unsupported shaders; still report failures and exit nonzero')
    parser.add_argument('--compute-gmem', action='store_true',
                        help='include compute pool/map bindings and device-address capabilities for GMEM replay')
    parser.add_argument('--graphics-gmem', action='store_true',
                        help='include checked read-only global memory for classic vertex/fragment replay')
    parser.add_argument('--compact-bindless', action='store_true',
                        help='lower full guest-indexed sampled pools to checked compact Vulkan arrays')
    parser.add_argument('--ndve-title', action='store_true', help='include the first Rouleau network from the title movie')
    parser.add_argument('--reflect-existing', type=lambda p: Path(p).resolve(),
                        help='add .vkr companions to an existing Windows shader library without retranslating')
    parser.add_argument('keys', nargs='*', help='NVN2 vertex, fragment, compute, task or mesh keys (default all for --reflect-existing)')
    args = parser.parse_args()
    try:
        if args.reflect_existing:
            if (args.romfs or args.xlate or args.out or args.ndve_title or args.base or
                    args.manifest or args.allow_partial or args.compute_gmem or args.graphics_gmem or
                    args.compact_bindless or args.retranslate_vertices):
                parser.error('--reflect-existing cannot be combined with build options')
            reflect_existing(args.reflect_existing, args.keys)
        else:
            if not args.out:
                parser.error('building requires --out')
            if args.retranslate_vertices and not args.base:
                parser.error('--retranslate-vertices requires --base')
            run_build(args)
    except (OSError, ValueError, ImportError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f'[RED] {exc}\n')


if __name__ == '__main__':
    main()
