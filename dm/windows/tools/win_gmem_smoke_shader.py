#!/usr/bin/env python3
"""Create the checked shader for `m4_shader_smoke --gmem` without a game image."""

import argparse
import json
from pathlib import Path
import struct
import tempfile

from win_nvn_shaders import (
    VULKAN_ENV, reflection_blob, remap_spirv, run_spirv_tool, runtime_bindings, verify_gmem, write_capabilities,
)
from win_spirv_legalize import legalize, upgrade_read_alias

KEY = '676d656d736d6f6b'
READ_KEY = '676d656d72656164'
PT = 7 << 16
C0_STORAGE = 0x310
GMEM_SENTINEL = 0xff
U8, U16, B32, B128 = 0, 2, 4, 6


def mov_c0(dst, off):
    return 0x4c98 << 48 | 0xf << 39 | off // 4 << 20 | PT | dst


def ldg(dst, addr, off=0, size=B32):
    return 0xeed0 << 48 | size << 48 | 1 << 45 | off << 20 | PT | addr << 8 | dst


def stg(addr, src, off, size=B32):
    return 0xeed8 << 48 | size << 48 | 3 << 45 | off << 20 | PT | addr << 8 | src


def program():
    # Maxwell bundles: R2:R3 designates an ordinary SSBO of four guest
    # addresses. Each raw memory access takes an address loaded from it.
    # Keep all vector lanes live, and separate the two subword destinations
    # so the host can detect a wrongly logged halfword store.
    ops = [mov_c0(2, C0_STORAGE), mov_c0(3, C0_STORAGE + 4),
           ldg(4, 2), ldg(5, 2, 4), ldg(8, 2, 8), ldg(9, 2, 12),
           ldg(12, 4, size=B128),
           *(stg(8, 12 + i, 4 * i) for i in range(4)),
           ldg(4, 2, 16), ldg(5, 2, 20), ldg(8, 2, 24), ldg(9, 2, 28),
           ldg(12, 4, size=U8), ldg(13, 4, size=U16),
           stg(8, 12, 0, size=U8), stg(8, 13, 16, size=U16),
           0xe30000000007000f, 0x50b0000000070f00]  # EXIT, NOP padding
    return b''.join(struct.pack('<4Q', 0x001f8000fc0007e0, *ops[i:i + 3])
                    for i in range(0, len(ops), 3))


def read_program():
    # A tracked SSBO contains the raw source address. The result goes to a
    # different tracked SSBO, not a raw store; this shader needs no write log.
    ops = [mov_c0(2, C0_STORAGE), mov_c0(3, C0_STORAGE + 4),
           ldg(4, 2), ldg(5, 2, 4),
           mov_c0(6, C0_STORAGE + 16), mov_c0(7, C0_STORAGE + 20),
           ldg(8, 4, size=B128), *(stg(6, 8 + i, 4 * i) for i in range(4)),
           0xe30000000007000f]
    return b''.join(struct.pack('<4Q', 0x001f8000fc0007e0, *ops[i:i + 3])
                    for i in range(0, len(ops), 3))


def build(args, key, binary, expected_sbufs):
    opt = args.spirv_val.with_name('spirv-opt' + args.spirv_val.suffix)
    with tempfile.TemporaryDirectory(prefix='win_gmem_smoke_') as temp:
        work = Path(temp)
        code = work / 'fixture.bin'
        code.write_bytes(binary)
        spec = work / 'jobs.json'
        spec.write_text(json.dumps([dict(key=key, stage='cs', code=str(code), tex={}, img={}, attr={}, fout={},
                                         local=[1, 1, 1], debug=True)]), encoding='utf-8')
        run_spirv_tool(args.xlate, spec, work)
        res = json.loads((work / (key + '.res.json')).read_text(encoding='utf-8'))
        checked = work / 'fixture.checked.spv'
        checked.write_bytes(legalize((work / (key + '.spv')).read_bytes(), guard_gmem=True))
        optimized = work / 'fixture.opt.spv'
        run_spirv_tool(opt, '--target-env=' + VULKAN_ENV, '--eliminate-dead-variables',
                       '-o', optimized, checked)
        raw, declared, caps, _ = remap_spirv(optimized.read_bytes(), 'cs')
        bindings = runtime_bindings(res, declared)
        if not verify_gmem(bindings, 'cs', caps, True, GMEM_SENTINEL):
            raise ValueError('fixture lost its raw GMEM accesses')
        sbufs = sorted((b['cb_slot'], b['cb_offset'], b['flags']) for b in bindings if b['source'] == 'sbuf')
        if sbufs != expected_sbufs:
            raise ValueError(f'unexpected smoke shader storage bindings: {sbufs}')
        if key == READ_KEY:
            raw = upgrade_read_alias(raw)
        info = dict(key=key, stage='cs', c1='', capabilities=caps, bindings=bindings,
                    local=(1, 1, 1), gmem_checked=True)
        companion = reflection_blob(info, raw)
        module = work / 'fixture.spv'
        module.write_bytes(raw)
        run_spirv_tool(args.spirv_val, '--target-env', VULKAN_ENV, module)

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / (key + '.spv')).write_bytes(raw)
    (args.out / (key + '.vkr')).write_bytes(companion)
    return info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--xlate', required=True, type=lambda p: Path(p).resolve())
    parser.add_argument('--spirv-val', required=True, type=lambda p: Path(p).resolve())
    parser.add_argument('--out', required=True, type=Path)
    args = parser.parse_args()
    packet = (0, C0_STORAGE // 4, 0)
    table = (GMEM_SENTINEL, 0, 0)
    write_capabilities(args.out, [
        build(args, KEY, program(), [packet, table, (GMEM_SENTINEL, 1, 1)]),
        build(args, READ_KEY, read_program(), [packet, (0, (C0_STORAGE + 16) // 4, 1), table]),
    ])


if __name__ == '__main__':
    main()
