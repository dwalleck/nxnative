"""Windows ARM64 rewrite contract and optional native M1/M2 runs."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_nso_match import image, RET   # noqa: E402

TOOLS = Path(__file__).resolve().parents[1]
ENTRY = 0x100
GRAPHICS_FIXTURE_SPLIT = 0xF00  # m3_graphics_guest.S's two NRO payloads


def make_nro(words):
    assert ENTRY + 4 * len(words) <= 0x1000, 'fixture code exceeds the NRO text page'
    raw = image({ENTRY: words})
    raw[0x1100:0x111c] = raw[0x20:0x3c]
    struct.pack_into('<I', raw, 4, 0x1100)
    struct.pack_into('<ii', raw, 0x1110, 0x3000 - 0x1100, 0x3014 - 0x1100)
    raw[0x10:0x14] = b'NRO0'
    struct.pack_into('<I', raw, 0x18, len(raw))
    struct.pack_into('<6I', raw, 0x20, 0, 0x1000, 0x1000, 0x2000, 0x3000, 0x1000)
    struct.pack_into('<I', raw, 0x38, 0)
    return raw


def rewrite(tmp, words, target=None, workers=None, *, name='nro0', raw=None):
    src = tmp / 'input.nro'
    src.write_bytes(raw if raw is not None else make_nro(words))
    env = os.environ | {'DM_GAME': '010015100B514000'}
    env.pop('DM_REWRITE_J', None)
    if workers is not None:
        env['DM_REWRITE_J'] = workers
    argv = [sys.executable, str(TOOLS / 'patch_nso.py'), str(src), str(tmp), name]
    if target:
        argv.append('--target=' + target)
    return subprocess.run(argv, env=env, capture_output=True, text=True, timeout=60)


class WindowsRewrite(unittest.TestCase):
    def test_windows_slots_and_gate_metadata_are_distinct_from_apple(self):
        # mrs x18,tpidr_el0; msr tpidr_el0,x18; svc #0x42; ret
        words = (0xd53bd052, 0xd51bd052, 0xd4000841, RET)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            apple = rewrite(root, words)
            self.assertEqual(apple.returncode, 0, apple.stdout + apple.stderr)
            old = (root / 'nro0.patched.bin').read_bytes()
            win = rewrite(root, words, 'windows-arm64')
            self.assertEqual(win.returncode, 0, win.stdout + win.stderr)
            self.assertNotIn('[RED]', win.stdout + win.stderr)
            self.assertEqual((root / 'nro0.patched.bin').read_bytes(), old)
            fixups = dict(line[2:].split('\t') for line in (root / 'nro0.winarm64.fixups.tsv').read_text().splitlines())
            self.assertEqual(fixups['TARGET'], 'windows-arm64')
            self.assertEqual(fixups['KEY_WIN_TLS_IDX'], '4')
            patch_off = int(fixups['PATCH_OFF'], 0)
            md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
            patched = (root / 'nro0.winarm64.patched.bin').read_bytes()
            asm = {i.address: f'{i.mnemonic} {i.op_str}' for i in md.disasm(patched[patch_off:], patch_off)}
            self.assertTrue(any('[x18, #0x58]' in s for s in asm.values()))
            self.assertFalse(any('tpidr' in s for s in asm.values()))
            self.assertTrue(any('mrs x30, tpidrro_el0' == f'{i.mnemonic} {i.op_str}'
                                for i in md.disasm(old[patch_off:], patch_off)))
            ret, kind, svc, orig = (root / 'nro0.winarm64.gates.tsv').read_text().strip().split('\t')
            self.assertEqual((kind, int(svc), int(orig, 0)), ('svc', 0x42, ENTRY + 8))
            self.assertEqual(asm[int(ret, 0) - 4], 'blr x30')
            self.assertTrue(asm[int(ret, 0)].startswith('ldp x29, x30'))

    def test_unknown_target_is_not_silently_treated_as_apple(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            run = rewrite(root, (RET,), 'windows')
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('[RED] usage:', run.stderr)
            self.assertFalse((root / 'nro0.patched.bin').exists())

    def test_prototype_rejects_x18_exclusive_window(self):
        # ldaxrb w18,[x1]; stlxrb w2,w18,[x1]; ret
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            run = rewrite(root, (0x085ffc32, 0x0802fc32, RET), 'windows-arm64')
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('[RED] Windows ARM64 prototype does not support x18 exclusive windows', run.stderr)
            self.assertFalse((root / 'nro0.winarm64.patched.bin').exists())

    def test_prototype_rejects_unported_x18_stack_site(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            # ldur x18, [sp, #-8] overlaps the scratch red zone.
            win = rewrite(root, (0xf85f83f2, RET), 'windows-arm64')
            self.assertNotEqual(win.returncode, 0)
            self.assertIn('[RED] Windows ARM64 prototype cannot rewrite 0x100:', win.stderr)
            self.assertFalse((root / 'nro0.winarm64.patched.bin').exists())

    @unittest.skipUnless(sys.platform == 'win32', 'native Windows worker configuration')
    def test_explicit_fork_workers_are_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            win = rewrite(Path(tmp), (RET,), workers='2')
            self.assertNotEqual(win.returncode, 0)
            self.assertIn('DM_REWRITE_J must be 1', win.stderr)
            self.assertNotIn('Traceback', win.stderr)

    @unittest.skipUnless(sys.platform == 'win32' and os.environ.get('DM_WINARM64_M1_DIR'), 'requires the native M1 probe and guest fixture')
    def test_native_gate(self):
        build = Path(os.environ['DM_WINARM64_M1_DIR'])
        raw = (build / 'm1_guest.bin').read_bytes()
        words = struct.unpack('<' + 'I' * (len(raw) // 4), raw)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            win = rewrite(root, words, 'windows-arm64')
            self.assertEqual(win.returncode, 0, win.stdout + win.stderr)
            self.assertNotIn('[RED]', win.stdout + win.stderr)
            probe = subprocess.run([str(build / 'm1_probe.exe'), *(str(root / ('nro0.winarm64.' + f))
                                    for f in ('patched.bin', 'fixups.tsv', 'gates.tsv'))],
                                   capture_output=True, text=True, timeout=30)
            self.assertEqual(probe.returncode, 0, probe.stdout + probe.stderr)
            self.assertIn('2 SVC sites resolved on 2 threads', probe.stdout)


def fixture_module(words, imported, *, symbol='fixture_fn'):
    raw = make_nro(words)
    struct.pack_into('<i', raw, 0x1104, 0x1200 - 0x1100)
    names = b'\0' + symbol.encode('ascii') + b'\0'
    tags = [(5, 0x2100), (6, 0x20d0), (10, len(names))]
    if imported:
        tags += [(7, 0x2200), (8, 24), (36, 0x2240), (35, 8), (23, 0x2260), (2, 24)]
        struct.pack_into('<QQq', raw, 0x2200, 0x3800, 1027, 0x3820)
        struct.pack_into('<Q', raw, 0x2240, 0x3808)
        struct.pack_into('<QQq', raw, 0x2260, 0x3810, (1 << 32) | 1026, 0)
        struct.pack_into('<Q', raw, 0x3808, 0x3820)
        struct.pack_into('<4iI', raw, 0x2280, *(target - 0x2280 for target in range(0x400, 0x410, 4)), 1)
    for i, (tag, value) in enumerate(tags):
        struct.pack_into('<qQ', raw, 0x1200 + 16 * i, tag, value)
    struct.pack_into('<IBBHQQ', raw, 0x20e8, 1, 0x12, 0, 0 if imported else 1, 0 if imported else ENTRY, 8)
    raw[0x2100:0x2100 + len(names)] = names
    return raw


def import_fixture(words, names, strtab):
    raw = make_nro(words)
    struct.pack_into('<i', raw, 0x1104, 0x1200 - 0x1100)
    strings = bytearray(b'\0')
    rela = 0x2000 + 24 * (len(names) + 1)
    for i, name in enumerate(names, 1):
        struct.pack_into('<IBBHQQ', raw, 0x2000 + 24 * i, len(strings), 0x12, 0, 0, 0, 0)
        strings += name.encode('ascii') + b'\0'
        struct.pack_into('<QQq', raw, rela + 24 * (i - 1), 0x3800 + 8 * (i - 1), (i << 32) | 1026, 0)
    assert rela + 24 * len(names) <= strtab and strtab + len(strings) < 0x3000
    raw[strtab:strtab + len(strings)] = strings
    for i, (tag, value) in enumerate(((5, strtab), (6, 0x2000), (10, len(strings)), (23, rela), (2, 24 * len(names)))):
        struct.pack_into('<qQ', raw, 0x1200 + 16 * i, tag, value)
    return raw


def romfs_fixture_module(words):
    names = (
        '_ZN2nn2oe20GetCurrentFocusStateEv', '_ZN2nn2fs22QueryMountRomCacheSizeEPm',
        '_ZN2nn2fs8MountRomEPKc', '_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci',
        '_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE', '_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm',
        '_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm', '_ZN2nn2fs9CloseFileENS0_10FileHandleE',
        '_ZN2nn2fs7UnmountEPKc', 'nvnBootstrapLoader', 'dmFixtureSamplerSnapshot',
        '_ZN2nn7account10InitializeEv', '_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE',
        '_ZN2nn7account13ListOpenUsersEPiPNS0_3UidEi', '_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE',
        '_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE', '_ZN2nn7account9CloseUserERKNS0_10UserHandleE',
        '_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE',
        '_ZN2nn2fs26SetPriorityOnCurrentThreadENS0_8PriorityE',
    )
    strtab = 0x23b0
    assert strtab + 1 + sum(len(name) + 1 for name in names) <= 0x2700
    raw = import_fixture(words, names, strtab)
    for at, name in ((0x2a00, b'content'), (0x2a10, b'content:\\file'), (0x2a30, b'content:/../file'),
                     (0x2a50, b'nvnCommandBufferDrawTransformFeedback')):
        raw[at:at + len(name)] = name
    for i, name in enumerate((
        'nvnDeviceGetInteger', 'nvnDeviceBuilderSetDefaults', 'nvnDeviceInitialize',
        'nvnMemoryPoolBuilderSetDevice', 'nvnMemoryPoolBuilderSetDefaults',
        'nvnMemoryPoolBuilderSetStorage', 'nvnMemoryPoolInitialize', 'nvnMemoryPoolMap',
        'nvnMemoryPoolGetBufferAddress', 'nvnMemoryPoolFinalize', 'nvnMemoryPoolBuilderSetFlags',
        'nvnSamplerPoolInitialize', 'nvnSamplerPoolGetSize', 'nvnSamplerPoolGetMemoryOffset',
        'nvnSamplerPoolGetMemoryPool', 'nvnSamplerPoolFinalize', 'nvnTexturePoolInitialize',
        'nvnTexturePoolFinalize',
    )):
        at = 0x2ac0 + i * 0x40
        raw[at:at + len(name)] = name.encode('ascii')
    for i, name in enumerate((
        'nvnSamplerBuilderSetDevice', 'nvnSamplerBuilderSetDefaults', 'nvnSamplerBuilderSetLodClamp',
        'nvnSamplerBuilderSetMinMagFilter', 'nvnSamplerInitialize', 'nvnSamplerFinalize',
        'nvnSamplerPoolRegisterSampler',
    )):
        at = 0x2700 + i * 0x40
        raw[at:at + len(name)] = name.encode('ascii')
    sampler_get = b'nvnSamplerGetMinMagFilter'
    raw[0x1a00:0x1a00 + len(sampler_get)] = sampler_get
    for i, name in enumerate((
        'nvnQueueBuilderSetDevice', 'nvnQueueBuilderSetDefaults', 'nvnQueueBuilderSetComputeMemorySize',
        'nvnQueueBuilderSetCommandMemorySize', 'nvnQueueBuilderGetComputeMemorySize',
        'nvnQueueBuilderGetCommandMemorySize', 'nvnQueueBuilderSetQueuePriority', 'nvnQueueBuilderGetQueuePriority',
        'nvnQueueBuilderSetFlags', 'nvnQueueBuilderGetFlags', 'nvnQueueBuilderGetDevice',
        'nvnQueueBuilderSetControlMemorySize', 'nvnQueueBuilderGetControlMemorySize',
        'nvnQueueBuilderSetRayTracingMemorySize', 'nvnQueueBuilderGetRayTracingMemorySize',
        'nvnQueueBuilderSetCommandFlushThreshold', 'nvnQueueBuilderGetCommandFlushThreshold',
        'nvnQueueBuilderSetQueueMemory', 'nvnQueueBuilderGetMemory', 'nvnQueueBuilderGetMemorySize',
    )):
        at = 0x1500 + i * 0x40
        raw[at:at + len(name)] = name.encode('ascii')
    return raw


def nvn_sdk_fixture():
    # Synthetic SDK switch at 0x120. The test never executes either switch.
    # Items 0/1 are constants, 2 forwards a default, 3 reads device state.
    raw = fixture_module((RET,), False)
    switch = (
        0xd10013a1, 0x2a0003f4, 0xb81fc3bf, 0x94000015,  # zero result slot; BL 0x180
        0x7100969f, 0x540001c8, 0xb0000009, 0x91100129,  # bounds and table at 0x1400
        0x2a1403e8, 0x52801ce0, 0x1000008a, 0x3868692b,
        0x8b0b094a, 0xd61f0140, 0x52800500, 0x14000005,  # minor = 40
        0xb85fc3a0, 0x14000003, 0xb9400260, 0x2a1f03e0, RET,
    )
    default = (
        0x2a0003e8, 0x51000909, 0x2a1f03e0, 0x71008d3f,
        0x54000288, 0xb000000a, 0x9111014a, 0x528001c8,  # table at 0x1440
        0x1000008b, 0x3869694c, 0x8b0c096b, 0xd61f0160,
        0x52802008, 0x14000009, 0x52800408, 0x14000007,  # 256, descriptor size 32
        0x52800068, 0x14000005, 0x52800108, 0x14000003,  # 3 reserved sampler slots, max 8
        0x52800028, 0x14000001,                         # 1 reserved texture slot
        0x52800020, 0xb9000028, RET,
    )
    for at, words in ((0x120, switch), (0x180, default)):
        struct.pack_into(f'<{len(words)}I', raw, at, *words)
    raw[0x1400:0x1426] = bytes((6, 0, 2, 4)) + bytes((2,)) * 34
    defaults = bytearray((12,)) * 36
    for item, offset in {2: 0, 0xf: 2, 0x10: 2, 0x11: 8, 0x12: 4, 0x24: 6, 0x25: 6}.items():
        defaults[item - 2] = offset
    raw[0x1440:0x1464] = defaults
    return raw


@unittest.skipUnless(sys.platform == 'win32' and os.environ.get('DM_WINARM64_M2_DIR'), 'requires the native Windows host and guest fixtures')
class WindowsHost(unittest.TestCase):
    def test_native_module_and_memory(self):
        build = Path(os.environ['DM_WINARM64_M2_DIR'])
        code = (build / 'm2_guest.bin').read_bytes()
        words = struct.unpack('<' + 'I' * (len(code) // 4), code)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name, ins, imported in (('fixture', words, True), ('dep', (0xd2800540, RET), False)):
                run = rewrite(root, ins, 'windows-arm64', name=name, raw=fixture_module(ins, imported))
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            args = [str(build / 'm2_host.exe'), '--fixture', str(root), 'fixture', 'dep']
            run = subprocess.run(args, env=os.environ | {'DM_SVC': '1'}, capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("sm: service 'acc:u0' is not registered", run.stderr)
            self.assertIn('Windows ARM64 fixture returned 0', run.stdout)
            self.assertIn('fixture: reloc rela=1 relr=1 jump=1', run.stdout)
            self.assertIn('SetHeapSize 0x200000', run.stdout)
            self.assertIn('svc 0x2d', run.stdout)
            self.assertIn('svc 0x1a', run.stdout)
            self.assertIn('svc 0x1b', run.stdout)
            self.assertIn('[thread] started 0x1002', run.stdout)
            self.assertIn('[thread] exited 0x1002', run.stdout)
            self.assertIn('thread=0x1002', run.stdout)

            # A wrong-ABI image must be rejected before a single guest instruction runs.
            fixups = root / 'fixture.winarm64.fixups.tsv'
            fixups.write_text(fixups.read_text().replace('windows-arm64', 'apple'))
            run = subprocess.run(args, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('invalid Windows near-layout fixups', run.stderr)

    def test_native_reentrant_guest_call(self):
        build = Path(os.environ['DM_WINARM64_M2_DIR'])
        code = (build / 'm2_guest_call_guest.bin').read_bytes()
        words = struct.unpack(f'<{len(code) // 4}I', code)
        sdk_words = (0xd2826ae0, 0xd2848d01, RET)  # x0=0x1357, x1=0x2468
        sdk = fixture_module(sdk_words, False, symbol='dmFixtureGuestCall')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            fixture = import_fixture(words, ('dmFixtureGuestCall',), 0x2500)
            for name, ins, raw in (('fixture', words, fixture), ('sdk', sdk_words, sdk)):
                run = rewrite(root, ins, 'windows-arm64', name=name, raw=raw)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            run = subprocess.run([str(build / 'm2_host.exe'), '--fixture', str(root), 'fixture', 'sdk'],
                                 capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('Windows ARM64 fixture returned 0', run.stdout)

    @unittest.skipUnless(os.environ.get('DM_WINARM64_M3_DIR'), 'requires the native M3 service fixture')
    def test_native_romfs_and_nvn(self):
        build = Path(os.environ['DM_WINARM64_M3_DIR'])
        code = (build / 'm3_guest.bin').read_bytes()
        words = struct.unpack('<' + 'I' * (len(code) // 4), code)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            run = rewrite(root, words, 'windows-arm64', name='fixture', raw=romfs_fixture_module(words))
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            img = root / 'rom-\u00e9.img'
            image_bytes = (struct.pack('<8sQQQ', b'DMROMFS1', 40, 52, 2) + b'12345678' +
                           struct.pack('<QQIHBB', 1, 1, 0, 0, 0, 0) +
                           struct.pack('<QQIHBB', 32, 8, 0, 4, 1, 0) + b'file')
            img.write_bytes(image_bytes)
            sdk = rewrite(root, (RET,), 'windows-arm64', name='sdk', raw=nvn_sdk_fixture())
            self.assertEqual(sdk.returncode, 0, sdk.stdout + sdk.stderr)
            args = [str(build / 'm2_host.exe'), '--fixture', str(root), 'fixture', 'sdk']
            env = os.environ | {'DM_ROMFS': str(img), 'DM_NICK': 'Link-\u00e9', 'DM_SVC': '1', 'DM_FS': '1'}
            env.pop('DM_CONTROL_NACP', None)
            run = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertEqual(run.stdout.count('[account] initialized local launch user'), 1)
            self.assertNotIn('[ipc]', run.stdout)
            self.assertIn('[fs] mounted content: 2 image entries', run.stdout)
            self.assertIn('[fs] read 0x10 + 0x4: 4 bytes (priority 2)', run.stdout)
            self.assertIn('[nvn] SDK API 231.40', run.stdout)
            self.assertIn('Windows ARM64 fixture returned 0', run.stdout)
            run = subprocess.run(args[:-1], env=env, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('headless NVN device query 0 has no decoded SDK constant', run.stderr)
            module = root / 'fixture.winarm64.patched.bin'
            fixture = bytearray(module.read_bytes())
            for mode, error in ((1, 'nvnCommandBufferDrawTransformFeedback'),
                                (2, 'headless NVN device query 0xff has no decoded SDK constant'),
                                (3, 'headless NVN device query 0x3 has no decoded SDK constant'),
                                (4, 'nvnSamplerPoolRegisterSampler: slot 2 outside writable range [3, 4)'),
                                (5, 'nvnSamplerPoolRegisterSampler: slot 4 outside writable range [3, 4)'),
                                (6, 'NVN descriptor pool: overlapping pool views are not supported'),
                                (7, 'nvnSamplerPoolRegisterSampler: invalid sampler'),
                                (8, 'nvnSamplerPoolRegisterSampler: no live sampler pool')):
                struct.pack_into('<I', fixture, 0x2a80, mode)
                module.write_bytes(fixture)
                run = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)
                self.assertNotEqual(run.returncode, 0)
                self.assertIn(error, run.stderr)

            # The native host uses the same index validation as Apple, not an unchecked file view.
            img.write_bytes(image_bytes[:-1])
            run = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('could not open a valid DMROMFS1 image', run.stderr)


    @unittest.skipUnless(os.environ.get('DM_WINARM64_M3_DIR'), 'requires the native M3 add-on fixture')
    def test_native_without_addons(self):
        build = Path(os.environ['DM_WINARM64_M3_DIR'])
        code = (build / 'm3_aoc_guest.bin').read_bytes()
        words = struct.unpack(f'<{len(code) // 4}I', code)
        names = ('_ZN2nn3aoc17CountAddOnContentEv', '_ZN2nn3aoc16ListAddOnContentEPiii')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            win = rewrite(root, words, 'windows-arm64', name='fixture', raw=import_fixture(words, names, 0x2500))
            self.assertEqual(win.returncode, 0, win.stdout + win.stderr)
            args = [str(build / 'm2_host.exe'), '--fixture', str(root), 'fixture']
            env = os.environ | {'DM_SVC': '1'}
            env.pop('DM_AOC_DIR', None)
            run = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertNotIn('[ipc]', run.stdout)
            addons = root / 'add-ons'
            addons.mkdir()
            run = subprocess.run(args, env=env | {'DM_AOC_DIR': str(addons)}, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('DM_AOC_DIR is set, but this host does not support add-on mounts', run.stderr)

    @unittest.skipUnless(os.environ.get('DM_WINARM64_M3_DIR'), 'requires the native M3 HID fixture')
    def test_native_headless_hid(self):
        build = Path(os.environ['DM_WINARM64_M3_DIR'])
        code = (build / 'm3_hid_guest.bin').read_bytes()
        words = struct.unpack(f'<{len(code) // 4}I', code)
        names = (
            '_ZN2nn3hid14InitializeNpadEv', '_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm',
            '_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE',
            '_ZN2nn3hid24GetSupportedNpadStyleSetEv', '_ZN2nn3hid15GetNpadStyleSetERKj',
            '_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj',
            '_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj',
            '_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj',
            '_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj',
            '_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj',
            '_ZN2nn3hid15InitializeMouseEv', '_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi',
            '_ZN2nn3hid18InitializeKeyboardEv', '_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE',
            '_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE',
            '_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE',
            '_ZN2nn3hid22GetSensorFusionHandlesEPNS0_18SensorFusionHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE',
            '_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE',
            '_ZN2nn3hid14DisconnectNpadERKj',
        )
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            win = rewrite(root, words, 'windows-arm64', name='fixture', raw=import_fixture(words, names, 0x2500))
            self.assertEqual(win.returncode, 0, win.stdout + win.stderr)
            args = [str(build / 'm2_host.exe'), '--fixture', str(root), 'fixture']
            env = os.environ | {'DM_SVC': '1', 'DM_KEYBOARD': '1'}
            env.pop('DM_WINDOW', None)
            env.pop('DM_CTL', None)
            run = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertEqual(run.stdout.count('[hid] initialized headless Npad'), 1)
            self.assertNotIn('[ipc]', run.stdout)
            module = root / 'fixture.winarm64.patched.bin'
            fixture = bytearray(module.read_bytes())
            struct.pack_into('<I', fixture, 0x2fe0, 1)
            module.write_bytes(fixture)
            run = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('hid: invalid Npad ID 0xff', run.stderr)

    @unittest.skipUnless(os.environ.get('DM_WINARM64_M3_DIR'), 'requires the native M3 raster fixture')
    def test_native_raster_state(self):
        build = Path(os.environ['DM_WINARM64_M3_DIR'])
        code = (build / 'm3_raster_guest.bin').read_bytes()
        words = struct.unpack(f'<{len(code) // 4}I', code)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            raw = import_fixture(words, ('nvnDeviceGetProcAddress',), 0x2200)
            win = rewrite(root, words, 'windows-arm64', name='fixture', raw=raw)
            self.assertEqual(win.returncode, 0, win.stdout + win.stderr)
            run = subprocess.run([build / 'm2_host.exe', '--fixture', root, 'fixture'],
                                 capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('Windows ARM64 fixture returned 0', run.stdout)

    @unittest.skipUnless(os.environ.get('DM_WINARM64_M3_DIR'), 'requires the native M3 program fixture')
    def test_native_headless_program(self):
        build = Path(os.environ['DM_WINARM64_M3_DIR'])
        code = (build / 'm3_program_guest.bin').read_bytes()
        words = struct.unpack(f'<{len(code) // 4}I', code)
        names = (
            'nvnDeviceBuilderSetDefaults', 'nvnDeviceInitialize', 'nvnProgramBuilderSetDefaults',
            'nvnProgramBuilderSetDevice', 'nvnProgramBuilderSetShaders', 'nvnProgramInitializeFromBuilder',
            'nvnProgramFinalize', 'nvnProgramInitialize', 'nvnProgramSetShaders', 'nvnProgramSetShadersExt',
        )
        raw = import_fixture(words, names, 0x2200)
        for stage, (control_len, sph_type) in enumerate(((0, 1), (0x48, 5))):
            slot = 0x1500 + stage * 0x100
            shader = bytearray(0x80)
            struct.pack_into('<I', shader, 0, (sph_type << 10) | 0x61)
            raw[slot + 0x80:slot + 0x100] = shader
            control = bytearray(0x48)
            struct.pack_into('<II', control, 0, 0x98761234, 0xc9)
            struct.pack_into('<I', control, 0x44, len(shader))
            key_at = 0x1700 + stage * 8
            raw[key_at:key_at + 8] = hashlib.sha256(control + shader).digest()[:8]
            struct.pack_into('<I', control, 0x30, control_len)
            raw[slot:slot + len(control)] = control
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            win = rewrite(root, words, 'windows-arm64', name='fixture', raw=raw)
            self.assertEqual(win.returncode, 0, win.stdout + win.stderr)
            module = root / 'fixture.winarm64.patched.bin'
            good = module.read_bytes()
            args = [str(build / 'm2_host.exe'), '--fixture', str(root), 'fixture']
            for mode in range(7):
                with self.subTest(mode=mode):
                    fixture = bytearray(good)
                    struct.pack_into('<I', fixture, 0x1780, mode)
                    if mode == 1:
                        struct.pack_into('<I', fixture, 0x1500, 0)  # invalid control marker
                    if mode == 4:
                        struct.pack_into('<I', fixture, 0x1544, 1 << 24)  # oversized code span
                    module.write_bytes(fixture)
                    run = subprocess.run(args, capture_output=True, text=True, timeout=30)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.assertIn('Windows ARM64 fixture returned 0', run.stdout)

    @unittest.skipUnless(os.environ.get('DM_WINARM64_M3_DIR'), 'requires the native M3 copy fixture')
    def test_native_submit_texture_copies(self):
        build = Path(os.environ['DM_WINARM64_M3_DIR'])
        code = (build / 'm3_copy_guest.bin').read_bytes()
        words = struct.unpack(f'<{len(code) // 4}I', code)
        names = (
            'nvnDeviceBuilderSetDefaults', 'nvnDeviceInitialize', 'nvnMemoryPoolBuilderSetDefaults',
            'nvnMemoryPoolBuilderSetDevice', 'nvnMemoryPoolBuilderSetStorage', 'nvnMemoryPoolInitialize',
            'nvnTextureBuilderSetDefaults', 'nvnTextureBuilderSetDevice', 'nvnTextureBuilderSetSize2D',
            'nvnTextureBuilderSetFormat', 'nvnTextureBuilderSetStorage', 'nvnTextureInitialize',
            'nvnTextureViewSetDefaults', 'nvnTextureViewSetLevels', 'nvnCommandBufferInitialize',
            'nvnCommandBufferAddCommandMemory', 'nvnCommandBufferAddControlMemory', 'nvnCommandBufferSetCopyRowStride',
            'nvnCommandBufferSetCopyImageStride', 'nvnCommandBufferBeginRecording', 'nvnCommandBufferCopyBufferToTexture',
            'nvnCommandBufferEndRecording', 'nvnTextureFinalize', 'nvnQueueBuilderSetDefaults',
            'nvnQueueBuilderSetDevice', 'nvnQueueInitialize', 'nvnQueueSubmitCommands', 'nvnCommandBufferBarrier',
            'nvnQueueFinish', 'nvnDeviceFinalizeCommandHandle',
            'nvnBufferBuilderSetDefaults', 'nvnBufferBuilderSetDevice', 'nvnBufferBuilderSetStorage',
            'nvnBufferInitialize', 'nvnBufferGetMemoryOffset', 'nvnBufferGetMemoryPool', 'nvnMemoryPoolGetBufferAddress',
            'nvnTextureBuilderSetPackagedTextureData',
            'nvnCommandBufferCopyTextureToTexture', 'nvnTextureBuilderSetTarget', 'nvnTextureBuilderSetSize3D',
            'nvnTextureBuilderSetLevels', 'nvnTextureViewSetLayers', 'nvnTextureBuilderSetFlags',
            'nvnTextureBuilderSetStride', 'nvnTextureViewSetFormat', 'nvnWindowBuilderSetDefaults',
            'nvnWindowBuilderSetDevice', 'nvnWindowBuilderSetTextures', 'nvnWindowInitialize', 'nvnWindowFinalize',
        )
        raw = import_fixture(words, names, 0x2a00)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            win = rewrite(root, words, 'windows-arm64', name='fixture', raw=raw)
            self.assertEqual(win.returncode, 0, win.stdout + win.stderr)
            args = [str(build / 'm2_host.exe'), '--fixture', str(root), 'fixture']
            run = subprocess.run(args, capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('Windows ARM64 fixture returned 0', run.stdout)
            self.assertIn('first headless submission: 1 command lists, 1 buffer-to-texture copies', run.stdout)
            module = root / 'fixture.winarm64.patched.bin'
            bad = bytearray(module.read_bytes())
            for mode, error in ((1, 'nvnCommandBufferCopyBufferToTexture: region exceeds the subresource'),
                                (2, 'nvnQueueSubmitCommands: buffer-to-texture copy: invalid source span'),
                                (3, 'nvnQueueSubmitCommands: invalid record size'),
                                (4, 'nvnQueueFinish: invalid queue')):
                with self.subTest(mode=mode):
                    struct.pack_into('<I', bad, 0x1780, mode)
                    module.write_bytes(bad)
                    run = subprocess.run(args, capture_output=True, text=True, timeout=30)
                    self.assertNotEqual(run.returncode, 0)
                    self.assertIn(error, run.stderr)
                    self.assertNotIn('first headless submission', run.stdout)
            with self.subTest(mode='mirrored scaling from tiled array mip to padded linear texture'):
                struct.pack_into('<I', bad, 0x1780, 5)
                module.write_bytes(bad)
                run = subprocess.run(args, capture_output=True, text=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn('Windows ARM64 fixture returned 0', run.stdout)
            if os.environ.get('DM_WINARM64_M4_WSI'):
                with self.subTest(mode='Vulkan sRGB decoding, filtered scaling and BGRA writeback'):
                    shaders = root / 'blit-shaders'
                    shaders.mkdir()
                    (shaders / 'graphics.vkc').write_bytes(struct.pack('<8sII', b'DMVKCAP1', 0, 0))
                    env = os.environ.copy() | {'DM_WINDOW': '1', 'DM_VK_SHADERS': str(shaders)}
                    struct.pack_into('<I', bad, 0x1780, 6)
                    module.write_bytes(bad)
                    run = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                    self.assertIn('Windows ARM64 fixture returned 0', run.stdout)

    @unittest.skipUnless(os.environ.get('DM_WINARM64_M3_DIR'), 'requires the native M3 counter fixture')
    def test_native_submit_timestamp_reports(self):
        build = Path(os.environ['DM_WINARM64_M3_DIR'])
        code = (build / 'm3_counter_guest.bin').read_bytes()
        words = struct.unpack(f'<{len(code) // 4}I', code)
        names = (
            'nvnDeviceBuilderSetDefaults', 'nvnDeviceInitialize', 'nvnMemoryPoolBuilderSetDefaults',
            'nvnMemoryPoolBuilderSetDevice', 'nvnMemoryPoolBuilderSetStorage', 'nvnMemoryPoolInitialize',
            'nvnMemoryPoolGetBufferAddress', 'nvnCommandBufferInitialize', 'nvnCommandBufferAddCommandMemory',
            'nvnCommandBufferAddControlMemory', 'nvnQueueBuilderSetDefaults', 'nvnQueueBuilderSetDevice',
            'nvnQueueInitialize', 'nvnDeviceGetProcAddress', 'nvnCommandBufferBeginRecording',
            'nvnCommandBufferEndRecording', 'nvnDeviceGetCurrentTimestampInNanoseconds', 'nvnQueueSubmitCommands',
            'nvnDeviceGetTimestampInNanoseconds', 'nvnCommandBufferUpdateUniformBuffer',
        )
        raw = import_fixture(words, names, 0x2600)
        name = b'nvnCommandBufferReportCounter\0'
        raw[0x1500:0x1500 + len(name)] = name
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            win = rewrite(root, words, 'windows-arm64', name='fixture', raw=raw)
            self.assertEqual(win.returncode, 0, win.stdout + win.stderr)
            module = root / 'fixture.winarm64.patched.bin'
            fixture = bytearray(module.read_bytes())
            args = [build / 'm2_host.exe', '--fixture', root, 'fixture']
            for mode, error in enumerate((
                None,
                'nvnCommandBufferReportCounter: unsupported counter type 0x2',
                'nvnQueueSubmitCommands: invalid timestamp report destination',
            )):
                with self.subTest(mode=mode):
                    struct.pack_into('<I', fixture, 0x1780, mode)
                    module.write_bytes(fixture)
                    run = subprocess.run(args, capture_output=True, text=True, timeout=30)
                    if error:
                        self.assertNotEqual(run.returncode, 0)
                        self.assertIn(error, run.stderr)
                        self.assertNotIn('first headless submission', run.stdout)
                    else:
                        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                        self.assertIn('Windows ARM64 fixture returned 0', run.stdout)

    @unittest.skipUnless(os.environ.get('DM_WINARM64_M3_DIR'), 'requires the native M3 event fixture')
    def test_native_queue_event_dependencies(self):
        build = Path(os.environ['DM_WINARM64_M3_DIR'])
        code = (build / 'm3_event_guest.bin').read_bytes()
        words = struct.unpack(f'<{len(code) // 4}I', code)
        names = (
            'nvnDeviceBuilderSetDefaults', 'nvnDeviceInitialize', 'nvnMemoryPoolBuilderSetDefaults',
            'nvnMemoryPoolBuilderSetDevice', 'nvnMemoryPoolBuilderSetStorage', 'nvnMemoryPoolInitialize',
            'nvnCommandBufferInitialize', 'nvnCommandBufferAddCommandMemory', 'nvnCommandBufferAddControlMemory',
            'nvnQueueBuilderSetDefaults', 'nvnQueueBuilderSetDevice', 'nvnQueueInitialize',
            'nvnCommandBufferBeginRecording', 'nvnCommandBufferEndRecording', 'nvnCommandBufferWaitEvent',
            'nvnCommandBufferSignalEvent', 'nvnCommandBufferFenceSync', 'nvnCommandBufferWaitSync',
            'nvnQueueSubmitCommands', 'nvnQueueFlush', 'nvnQueueFinish', 'nvnQueueWaitSync', 'nvnQueueFenceSync',
            'nvnQueueFinalize', 'nvnSyncWait', 'nvnSyncInitialize', 'nvnEventBuilderSetDefaults',
            'nvnEventBuilderSetStorage', 'nvnEventInitialize', 'nvnEventFinalize', 'nvnEventSignal',
        )
        raw = import_fixture(words, names, 0x2600)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            win = rewrite(root, words, 'windows-arm64', name='fixture', raw=raw)
            self.assertEqual(win.returncode, 0, win.stdout + win.stderr)
            run = subprocess.run([build / 'm2_host.exe', '--fixture', root, 'fixture'],
                                 capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('Windows ARM64 fixture returned 0', run.stdout)

    @unittest.skipUnless(os.environ.get('DM_WINARM64_M3_DIR'), 'requires the native M4 draw state fixture')
    def test_native_draw_state_recording_and_queue_lifetime(self):
        build = Path(os.environ['DM_WINARM64_M3_DIR'])
        code = (build / 'm4_draw_state_guest.bin').read_bytes()
        words = struct.unpack(f'<{len(code) // 4}I', code)
        names = (
            'nvnDeviceBuilderSetDefaults', 'nvnDeviceInitialize', 'nvnMemoryPoolBuilderSetDefaults',
            'nvnMemoryPoolBuilderSetDevice', 'nvnMemoryPoolBuilderSetStorage', 'nvnMemoryPoolInitialize',
            'nvnCommandBufferInitialize', 'nvnCommandBufferAddCommandMemory', 'nvnCommandBufferAddControlMemory',
            'nvnCommandBufferBeginRecording', 'nvnCommandBufferBindUniformBuffers', 'nvnCommandBufferSetDepthRanges',
            'nvnCommandBufferBindProgram', 'nvnCommandBufferEndRecording', 'nvnCommandBufferGetCommandMemoryUsed',
            'nvnCommandBufferBindTexture', 'nvnCommandBufferSetViewport', 'nvnCommandBufferDrawElements',
            'nvnQueueBuilderSetDefaults', 'nvnQueueBuilderSetDevice', 'nvnQueueInitialize', 'nvnQueueSubmitCommands',
            'nvnQueueFinalize', 'nvnProgramBuilderSetDefaults', 'nvnProgramBuilderSetDevice',
            'nvnProgramBuilderSetShaders', 'nvnProgramInitializeFromBuilder', 'nvnProgramFinalize',
            'nvnCommandBufferSetLineWidth', 'nvnCommandBufferSetPointSize',
            'nvnCommandBufferSetGeometryProcessingMode',
            'nvnCommandBufferBindTexturesDeferred',
            'nvnCommandBufferShadingRateImagePalette', 'nvnCommandBufferBindShadingRateImage',
            'nvnCommandBufferSetShadingRatePaletteMode', 'nvnTextureBuilderSetDefaults',
            'nvnTextureBuilderSetDevice', 'nvnTextureBuilderSetFormat', 'nvnTextureBuilderSetSize2D',
            'nvnTextureBuilderSetLevels', 'nvnTextureBuilderSetStorage', 'nvnTextureInitialize',
            'nvnTextureViewSetDefaults', 'nvnTextureViewSetLevels',
        )
        raw = import_fixture(words, names, 0x2890)
        for slot, stage in ((0x1500, 1), (0x1600, 5)):
            struct.pack_into('<II', raw, slot, 0x98761234, 0xc9)
            struct.pack_into('<I', raw, slot + 0x30, 0x48)
            struct.pack_into('<I', raw, slot + 0x44, 0x80)
            struct.pack_into('<I', raw, slot + 0x80, (stage << 10) | 0x61)
            raw[slot + 0x80 + 0x34] = 0x80  # Only the vertex header describes a VPRS table output.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            win = rewrite(root, words, 'windows-arm64', name='fixture', raw=raw)
            self.assertEqual(win.returncode, 0, win.stdout + win.stderr)
            module = root / 'fixture.winarm64.patched.bin'
            fixture = bytearray(module.read_bytes())
            args = [build / 'm2_host.exe', '--fixture', root, 'fixture']
            for mode, error in enumerate((
                None,
                '',  # A malformed command must fail without submitting the draw.
                'nvnCommandBufferBindUniformBuffers: invalid stage 0 or slot range 15 + 2',
                '',  # Vertex-only draws are valid; the headless fixture has no render device.
                'indexed draw has no active vertex shader',
                'indexed draw requires classic NVN geometry processing mode',  # retained mode 1
                'indexed draw requires classic NVN geometry processing mode',  # retained mode 2
                'nvnCommandBufferSetGeometryProcessingMode: invalid mode 3',
                None,  # The list is unmapped at record time, then used by both submissions.
                'nvnQueueSubmitCommands: deferred texture handles: invalid guest input span',  # truncated on second submit
                'nvnQueueSubmitCommands: deferred texture handles: invalid guest input span',  # null on first submit
                None,  # Palette and texture view are captured by value before caller reuse.
                'nvnCommandBufferShadingRateImagePalette: invalid table 16 or entry count 2',
                'nvnCommandBufferShadingRateImagePalette: invalid table 15 or entry count 17',
                'nvnCommandBufferShadingRateImagePalette: invalid guest input span',
                'nvnCommandBufferShadingRateImagePalette: invalid entry 0 rate 12',
                'nvnQueueSubmitCommands: invalid recorded shading rate',
            )):
                with self.subTest(mode=mode):
                    struct.pack_into('<I', fixture, 0x1780, mode)
                    module.write_bytes(fixture)
                    run = subprocess.run(args, capture_output=True, text=True, timeout=30)
                    if error is not None:
                        self.assertNotEqual(run.returncode, 0)
                        if error:
                            self.assertIn(error, run.stderr)
                        if mode == 3:  # Only the fragment stage was unbound.
                            self.assertRegex(run.stderr, r'VS (?!0{16})[0-9a-f]{16}, PS 0{16}\)')
                        if mode == 9:
                            self.assertIn('first headless submission', run.stdout)
                        if mode == 10:
                            self.assertNotIn('first headless submission', run.stdout)
                    else:
                        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                        self.assertIn('Windows ARM64 fixture returned 0', run.stdout)
                    self.assertNotIn('first indexed draw submission completed', run.stdout)

    @unittest.skipUnless(os.environ.get('DM_WINARM64_M3_DIR'), 'requires the native M4 attachment clear fixture')
    def test_native_submit_attachment_clears(self):
        build = Path(os.environ['DM_WINARM64_M3_DIR'])
        code = (build / 'm4_clear_guest.bin').read_bytes()
        words = struct.unpack(f'<{len(code) // 4}I', code)
        names = (
            'nvnDeviceBuilderSetDefaults', 'nvnDeviceBuilderSetFlags', 'nvnDeviceInitialize',
            'nvnMemoryPoolBuilderSetDefaults', 'nvnMemoryPoolBuilderSetDevice', 'nvnMemoryPoolBuilderSetStorage',
            'nvnMemoryPoolInitialize', 'nvnTextureBuilderSetDefaults', 'nvnTextureBuilderSetDevice',
            'nvnTextureBuilderSetTarget', 'nvnTextureBuilderSetFormat', 'nvnTextureBuilderSetFlags',
            'nvnTextureBuilderSetSize3D', 'nvnTextureBuilderSetLevels', 'nvnTextureBuilderSetStorage',
            'nvnTextureInitialize', 'nvnTextureViewSetDefaults', 'nvnTextureViewSetLevels',
            'nvnTextureViewSetLayers', 'nvnTextureViewSetFormat', 'nvnTextureViewSetSwizzle',
            'nvnTextureViewSetDepthStencilMode', 'nvnCommandBufferInitialize', 'nvnCommandBufferAddCommandMemory',
            'nvnCommandBufferAddControlMemory', 'nvnCommandBufferBeginRecording', 'nvnCommandBufferSetRenderTargets',
            'nvnCommandBufferSetScissor', 'nvnCommandBufferClearColor', 'nvnCommandBufferClearDepthStencil',
            'nvnCommandBufferEndRecording', 'nvnQueueBuilderSetDefaults', 'nvnQueueBuilderSetDevice',
            'nvnQueueInitialize', 'nvnQueueSubmitCommands', 'nvnQueueFinish', 'nvnDeviceFinalizeCommandHandle',
            'nvnCommandBufferClearTexture',
            'nvnCommandBufferSetRenderEnableConditional', 'nvnCommandBufferSetRenderEnable',
            'nvnCommandBufferClearBuffer', 'nvnCommandBufferDispatchComputeIndirect', 'nvnTextureGetTextureAddress',
        )

        def gob(x, y, pitch, block_height, x1, block_depth=1):
            # x is bytes. The GOB's four 16-byte columns and eight rows have
            # distinct permutations in the two documented hardware orders.
            cols = (0, 32, 256, 288) if x1 else (0, 64, 256, 320)
            rows = (0, 16, 64, 80, 128, 144, 192, 208) if x1 else (0, 16, 32, 48, 128, 144, 160, 176)
            block = 512 * block_height * block_depth
            return (y // (8 * block_height) * pitch + x // 64) * block + y // 8 % block_height * 512 + (
                cols[x // 16 % 4] + rows[y % 8] + x % 16
            )

        for case in ('linear', 'x1', 'switch2', 'whole-gob', 'gob-tail', 'volume-slices', 'volume-mip'):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                raw = import_fixture(words, names, 0x2900)
                x1 = case == 'x1'
                device_flags = 0x10000 if x1 else 0
                volume = case in ('volume-slices', 'volume-mip')
                color_bd = 1
                clear_z, clear_count, clear_channel = 0, 1, 3
                clear_bases = None
                if case == 'linear':
                    color = (1, 0x9a, 0x40, 5, 4, 1, 1)  # BGRA8, linear render target
                    depth = (0x35, 6, 4)  # D24S8, wider than the color attachment
                    view = (0, 1, 0, 1, 0x9c)  # BGRA8_SRGB
                    a, b = (-2, 1, 6, 2), (4, 3, 3, 4)
                    region_a, region_b = (range(4), range(1, 3)), (range(4, 5), range(3, 4))
                    color_base, color_pitch, color_bh = (0,), 0, 0
                    depth_pitch, depth_bh = 1, 1
                elif case in ('whole-gob', 'gob-tail'):
                    height = 8 if case == 'whole-gob' else 11
                    color = (1, 0x25, 0, 16, height, 1, 1)
                    depth = (0x35, 16, height)
                    view = (0, 1, 0, 1, 0x38)
                    a, b = (0, 0, 0, height), (0, 0, 16, height)
                    region_a, region_b = (range(0), range(height)), (range(16), range(height))
                    color_bh = depth_bh = 1 if height == 8 else 2
                    color_base, color_pitch, depth_pitch = (0,), 1, 1
                elif volume:
                    depth = (0, 0, 0)  # no separate depth attachment
                    if case == 'volume-slices':
                        color = (2, 0x3f, 0, 8, 8, 8, 1)  # R11G11B10F, 3D
                        view = (0, 1, 3, 2, 0x3f)
                        # Clear two slices of a single block, leaving half of each GOB padded.
                        color_base, color_pitch, color_bh, color_bd = (0x600, 0x800), 1, 1, 8
                        clear_bases, width = (0x200, 0x400), 8
                    else:
                        color = (2, 0x3f, 0, 64, 16, 6, 2)
                        view = (1, 1, 2, 1, 0x3f)
                        # Mip 0 has 4 columns x 2 block rows x 8 GOBs (0x8000
                        # bytes). Mip 1 is one row of two blocks, each four
                        # GOBs deep: three image slices and a poisoned pad slice.
                        color_base, color_pitch, color_bh, color_bd = (0x8400,), 2, 1, 4
                        clear_bases, width = (0x8200, 0x8400), 32
                    a, b = (0, 0, 0, 8), (0, 0, width, 8)
                    region_a, region_b = (range(0), range(8)), (range(width), range(8))
                    clear_z, clear_count, clear_channel = 1, 2, 2
                else:
                    color = (4, 0x25, 0, 34, 20, 3, 3)  # RGBA8, 2D array
                    depth = (0x36, 17, 10)  # D32FS8
                    view = (1, 0, 1, 0, 0x38)  # base mip only; all remaining layers
                    a, b = (2, 1, 17, 3), (0, 8, 17, 4)
                    region_a, region_b = (range(2, 17), range(1, 4)), (range(17), range(8, 10))
                    # 34x20 has 6144 bytes, 17x10 has 2048, 8x5
                    # has 512. Array layers round up to 1024-byte blocks.
                    color_base, color_pitch, color_bh = (9216 + 6144, 2 * 9216 + 6144), 2, 2
                    depth_pitch, depth_bh = 3, 1  # standalone 17x10, not a mip of 34x20
                    clear_z = 1
                clear_region = (1, 0, clear_z, 1, 1, clear_count)
                struct.pack_into('<31i', raw, 0x1400, device_flags, *color, *depth, *view, *a, *b,
                                 *clear_region, clear_channel)

                def color_offset(x, y):
                    return gob(4*x, y, color_pitch, color_bh, x1, color_bd) if color_pitch else 128*y + 4*x

                expected = {}
                for masked, (xs, ys) in ((True, region_a), (False, region_b)):
                    for y in ys:
                        for x in xs:
                            for base in color_base:
                                off = color_offset(x, y)
                                # Linear RGB .25, .5 encode as 137, 188; alpha .5 as 128.
                                encoded = 0x8089c7bc if case == 'linear' else 0x80bcc789
                                expected[base + off] = encoded if masked else 0
                            if depth[0] == 0x35:
                                off = 0x7000 + gob(4*x, y, depth_pitch, depth_bh, x1)
                                expected[off] = 0xa5b6c7d3 if masked else 0x800000d8
                            elif depth[0] == 0x36:
                                off = 0x7000 + gob(8*x, y, depth_pitch, depth_bh, x1)
                                if masked:
                                    expected[off + 4] = 0xa5b6c7d3
                                else:
                                    expected[off] = 0x3f000000
                bits, encoded = (0xffc00000, 0x78000000) if volume else (0xff000000, 0xff000000)
                for base in clear_bases or color_base[-1:]:
                    off = base + color_offset(1, 0)
                    expected[off] = (expected.get(off, 0xa5b6c7d8) & ~bits) | encoded
                assert 0x1480 + 8 * (len(expected) + 1) <= 0x2000
                for i, (off, word) in enumerate((*sorted(expected.items()), (0xffffffff, 0))):
                    struct.pack_into('<II', raw, 0x1480 + 8*i, off, word)
                win = rewrite(root, words, 'windows-arm64', name='fixture', raw=raw)
                self.assertEqual(win.returncode, 0, win.stdout + win.stderr)
                run = subprocess.run([build / 'm2_host.exe', '--fixture', root, 'fixture'],
                                     capture_output=True, text=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn('Windows ARM64 fixture returned 0', run.stdout)

    @unittest.skipUnless(os.environ.get('DM_WINARM64_GAME_REWRITE'), 'requires the Ocarina Windows rewrite')
    def test_sdk_software_opus_pcm(self):
        build = Path(os.environ['DM_WINARM64_M2_DIR'])
        real = Path(os.environ['DM_WINARM64_GAME_REWRITE'])
        code = (build / 'm3_opus_guest.bin').read_bytes()
        words = struct.unpack(f'<{len(code) // 4}I', code)
        names = tuple('_ZN2nn5codec' + name for name in (
            '36GetHardwareOpusDecoderWorkBufferSizeEiii',
            '29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiPvmi',
            '32GetHardwareOpusDecoderSampleRateEPKNS0_23HardwareOpusDecoderTypeE',
            '34GetHardwareOpusDecoderChannelCountEPKNS0_23HardwareOpusDecoderTypeE',
            '33DecodeOpusInterleavedWithHardwareEPNS0_23HardwareOpusDecoderTypeEPmPiPsmPKvm',
            '24ResetHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE',
            '27FinalizeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE',
        ))
        with tempfile.TemporaryDirectory(dir=real.parent) as tmp:
            root = Path(tmp)
            modules = ('main', 'subsdk0', 'sdk')
            for name in modules:
                for path in real.glob(name + '.winarm64.*'):
                    os.link(path, root / path.name)
            run = rewrite(root, words, 'windows-arm64', name='fixture', raw=import_fixture(words, names, 0x2300))
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            run = subprocess.run([build / 'm2_host.exe', '--fixture', root, 'fixture', *modules],
                                 capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    @unittest.skipUnless(os.environ.get('DM_WINARM64_GAME_REWRITE'), 'requires the Ocarina Windows rewrite')
    def test_sdk_av1_nv12(self):
        build = Path(os.environ['DM_WINARM64_M2_DIR'])
        real = Path(os.environ['DM_WINARM64_GAME_REWRITE'])
        if not (build / 'libdav1d-7.dll').is_file():
            self.skipTest('requires native libdav1d-7.dll beside the host')
        code = (build / 'm3_video_guest.bin').read_bytes()
        words = struct.unpack(f'<{len(code) // 4}I', code)
        names = (
            '_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_15ImageTilingModeE',
            '_ZN2nn5codec31CalculateDecoderImagePoolBufferEPmS1_RKNS0_27VideoDecoderImagePreferenceEj',
            '_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEj',
            '_ZN2nn5codec36InitializeAv1DecoderByTransferMemoryEPNS0_14Av1DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE',
            '_ZN2nn5codec24RegisterAv1DecoderBufferEPNS0_14Av1DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj',
            '_ZN2nn5codec33GetAv1DecoderSubmissionReadyEventEPNS0_14Av1DecoderTypeE',
            '_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE',
            '_ZN2nn5codec35GetAv1DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Av1DecoderTypeEj',
            '_ZN2nn5codec29AcquireAv1DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Av1DecoderTypeE',
            '_ZN2nn2gs17TryWaitNativeSyncERKNS0_16NativeSyncHandleE',
            '_ZN2nn5codec29ReleaseAv1DecoderOutputBufferEPNS0_14Av1DecoderTypeEjNS_2gs16NativeSyncHandleE',
            '_ZN2nn6syncpt3sdk20CreateSyncPointEventEPNS1_18SyncPointEventTypeE',
            '_ZN2nn6syncpt3sdk21RequestSyncPointEventEPNS1_18SyncPointEventTypeERKNS1_15SyncPointHandleE',
            '_ZN2nn5codec27SubmitAv1DecoderInputBufferEPmPNS0_14Av1DecoderTypeEPKvmNS_8TimeSpanEb',
            '_ZN2nn2os20TimedWaitSystemEventEPNS0_15SystemEventTypeENS_8TimeSpanE',
            '_ZN2nn5codec26ReadDecoderOutputImageNv12EPKNS0_25VideoDecoderImagePoolTypeEjttttPvmmS4_mm',
            '_ZN2nn6syncpt3sdk21DestroySyncPointEventEPNS1_18SyncPointEventTypeE',
            '_ZN2nn5codec18FinalizeAv1DecoderEPNS0_14Av1DecoderTypeE',
            '_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE',
        )
        with tempfile.TemporaryDirectory(dir=real.parent) as tmp:
            root = Path(tmp)
            modules = ('main', 'subsdk0', 'sdk')
            for name in modules:
                for path in real.glob(name + '.winarm64.*'):
                    os.link(path, root / path.name)
            raw = import_fixture(words, names, 0x2400)
            # Lossless libaom temporal unit: 16x16 4:2:0, Y=40/180 luma bars, U=91/V=172.
            # Keep it in NRO data so the rewriter won't treat packet bytes as instructions.
            packet = bytes.fromhex('12000a0a000000019ff9b5f200803215140020000086c646361fe152696b14eed79ad9d25f')
            struct.pack_into('<Q', raw, 0x3900, len(packet))
            raw[0x3908:0x3908 + len(packet)] = packet
            made = rewrite(root, words, 'windows-arm64', name='fixture', raw=raw)
            self.assertEqual(made.returncode, 0, made.stdout + made.stderr)
            run = subprocess.run([build / 'm2_host.exe', '--fixture', root, 'fixture', *modules],
                                 capture_output=True, text=True, timeout=30)
            output = run.stdout + run.stderr
            self.assertEqual(run.returncode, 0, output)
            self.assertIn('output 3, mode 1 (dav1d)', output)
            self.assertIn('first picture 16x16 (timestamp 4321)', output)

    @unittest.skipUnless(os.environ.get('DM_WINARM64_M3_DIR'), 'requires the native M3 save fixture')
    def test_native_save_transactions(self):
        build = Path(os.environ['DM_WINARM64_M3_DIR'])
        code = (build / 'm3_save_guest.bin').read_bytes()
        words = struct.unpack(f'<{len(code) // 4}I', code)
        names = tuple('_ZN2nn2fs' + name for name in (
            '13MountSaveDataEPKcRKNS_7account3UidE', '21MountTemporaryStorageEPKc', '8MountRomEPKc',
            '7UnmountEPKc', '15CreateDirectoryEPKc', '10CreateFileEPKcl', '12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc',
            '8OpenFileEPNS0_10FileHandleEPKci', '9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE',
            '9FlushFileENS0_10FileHandleE', '11GetFileSizeEPlNS0_10FileHandleE', '11SetFileSizeENS0_10FileHandleEl',
            '8ReadFileENS0_10FileHandleElPvm', '8ReadFileEPmNS0_10FileHandleElPvm', '9CloseFileENS0_10FileHandleE',
            '13OpenDirectoryEPNS0_15DirectoryHandleEPKci', '13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl',
            '22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE', '14CloseDirectoryENS0_15DirectoryHandleE',
            '6CommitEPKc', '10DeleteFileEPKc', '15DeleteDirectoryEPKc', '14EnsureSaveDataERKNS_7account3UidE',
        ))
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            save = root / 'savedata'
            save.mkdir()
            image = root / 'rom.img'
            image.write_bytes(struct.pack('<8sQQQ', b'DMROMFS1', 40, 52, 2) + b'12345678' +
                              struct.pack('<QQIHBB', 1, 1, 0, 0, 0, 0) +
                              struct.pack('<QQIHBB', 32, 8, 0, 4, 1, 0) + b'file')
            run = rewrite(root, words, 'windows-arm64', name='fixture', raw=import_fixture(words, names, 0x2500))
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            module = root / 'fixture.winarm64.patched.bin'
            raw = bytearray(module.read_bytes())
            # The fixture keeps its mode word at data + 0xc00; the rewriter may move the module's data onto a 16 KB page of its own
            # (dm/tools/mod_layout.py), so take the data offset from the rewritten NRO header instead of the pre-rewrite 0x3c00.
            mode_off = struct.unpack_from('<I', raw, 0x30)[0] + 0xc00
            for mode, location in ((0, save), (1, save), (2, root / 'missing'), (2, 'relative')):
                with self.subTest(mode=mode, location=location):
                    struct.pack_into('<I', raw, mode_off, mode)
                    module.write_bytes(raw)
                    run = subprocess.run([build / 'm2_host.exe', '--fixture', root, 'fixture'],
                                         env=os.environ | {'DM_ROMFS': str(image), 'DM_SAVE': str(location)},
                                         capture_output=True, text=True, timeout=30)
                    self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    @unittest.skipUnless(os.environ.get('DM_WINARM64_M3_DIR'), 'requires the native M3 socket fixture')
    def test_native_socket_poll_wake_and_loopback(self):
        build = Path(os.environ['DM_WINARM64_M3_DIR'])
        code = (build / 'm3_socket_guest.bin').read_bytes()
        words = struct.unpack(f'<{len(code) // 4}I', code)
        names = tuple('_ZN2nn6socket' + name for name in (
            '13IsInitializedEv', '10InitializeERKNS0_6ConfigE', '7EventFdEmNS0_12EventFdFlagsE',
            '11EventFdReadEiPm', '12EventFdWriteEim',
        )) + (
            'nnsocketPoll', 'nnsocketGetLastErrno', 'nnsocketSocket', 'nnsocketBind',
            'nnsocketGetSockName', 'nnsocketConnect', 'nnsocketSend', 'nnsocketRecv',
            'nnsocketClose', '_ZN2nn6socket8FinalizeEv',
        )
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            run = rewrite(root, words, 'windows-arm64', name='fixture', raw=import_fixture(words, names, 0x2300))
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            run = subprocess.run([build / 'm2_host.exe', '--fixture', root, 'fixture'],
                                 capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('Windows ARM64 fixture returned 0', run.stdout)

    @unittest.skipUnless(os.environ.get('DM_WINARM64_M3_DIR'), 'requires the native M3 kernel fixture')
    def test_native_steady_clock_and_events(self):
        build = Path(os.environ['DM_WINARM64_M3_DIR'])
        code = (build / 'm3_kernel_guest.bin').read_bytes()
        words = struct.unpack(f'<{len(code) // 4}I', code)
        names = ('_ZN2nn4time37GetStandardSteadyClockCurrentTimespecEP8timespec',
                 '_ZN2nn4time41GetStandardUserSystemClockCurrentTimespecEP8timespec',
                 '_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_',
                 '_ZN2nn4time20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            win = rewrite(root, words, 'windows-arm64', name='fixture', raw=import_fixture(words, names, 0x2200))
            self.assertEqual(win.returncode, 0, win.stdout + win.stderr)
            args = [build / 'm2_host.exe', '--fixture', root, 'fixture']
            run = subprocess.run(args, capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            module = root / 'fixture.winarm64.patched.bin'
            bad = bytearray(module.read_bytes())
            struct.pack_into('<I', bad, 0x1780, 1)
            module.write_bytes(bad)
            run = subprocess.run(args, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('steady clock: invalid timespec', run.stderr)

    @unittest.skipUnless(os.environ.get('DM_WINARM64_M3_DIR'), 'requires the native M3 audio fixture')
    def test_native_logical_audio_device(self):
        build = Path(os.environ['DM_WINARM64_M3_DIR'])
        code = (build / 'm3_audio_guest.bin').read_bytes()
        words = struct.unpack(f'<{len(code) // 4}I', code)
        names = (
            '_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE',
            '_ZN2nn5audio25ListAudioOutputDeviceNameEPNS0_15AudioDeviceNameEi',
            '_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv',
            '_ZN2nn5audio26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf',
            '_ZN2nn5audio26GetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameE',
            '_ZN2nn5audio40SetDeviceOutputVolumeAutoTuneModeEnabledEb',
            '_ZN2nn5audio39IsDeviceOutputVolumeAutoTuneModeEnabledEv',
        )
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            run = rewrite(root, words, 'windows-arm64', name='fixture', raw=import_fixture(words, names, 0x2300))
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            run = subprocess.run([build / 'm2_host.exe', '--fixture', root, 'fixture'],
                                 capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    @unittest.skipUnless(os.environ.get('DM_WINARM64_M3_DIR'), 'requires the native M3 graphics fixture')
    def test_native_headless_present(self):
        build = Path(os.environ['DM_WINARM64_M3_DIR'])
        code = (build / 'm3_graphics_guest.bin').read_bytes()[:GRAPHICS_FIXTURE_SPLIT]
        words = struct.unpack(f'<{len(code) // 4}I', code)
        names = (
            'nvnDeviceBuilderSetDefaults', 'nvnDeviceInitialize', 'nvnQueueBuilderSetDefaults',
            'nvnQueueBuilderSetDevice', 'nvnQueueBuilderGetQueueMemorySize', 'nvnQueueBuilderSetQueueMemory',
            'nvnQueueInitialize', 'nvnMemoryPoolBuilderSetDefaults', 'nvnMemoryPoolBuilderSetDevice',
            'nvnMemoryPoolBuilderSetStorage', 'nvnMemoryPoolInitialize', 'nvnTextureBuilderSetDefaults',
            'nvnTextureBuilderSetDevice', 'nvnTextureBuilderSetSize2D', 'nvnTextureBuilderSetFormat',
            'nvnTextureBuilderGetStorageSize', 'nvnTextureBuilderSetStorage', 'nvnTextureInitialize',
            'nvnWindowBuilderSetDefaults', 'nvnWindowBuilderSetDevice', 'nvnWindowBuilderSetTextures',
            'nvnWindowBuilderSetPresentInterval', 'nvnWindowInitialize', 'nvnSyncInitialize',
            'nvnWindowAcquireTexture', 'nvnQueuePresentTexture', 'nvnEventBuilderSetDefaults',
            'nvnEventBuilderSetStorage', 'nvnEventInitialize', 'nvnEventSignal', 'nvnEventGetValue',
            '_ZN2nn6applet37TryPopFromApplicationParameterChannelEPNS0_13StorageHandleENS0_19LaunchParameterKindE',
            'nvnDeviceGetProcAddress', 'nvnMemoryPoolBuilderSetFlags', 'nvnMemoryPoolFinalize',
            'nvnTextureBuilderSetSwizzle', 'nvnTextureBuilderGetSwizzle', 'nvnTextureGetSwizzle',
            'nvnTextureBuilderSetDepthStencilMode', 'nvnTextureBuilderGetDepthStencilMode', 'nvnTextureGetDepthStencilMode',
            'nvnTextureBuilderGetStorageClass', 'nvnQueueBuilderSetPrimaryQueue', 'nvnQueueBuilderGetPrimaryQueue',
            'nvnMemoryPoolMapVirtual', 'nvnMemoryPoolGetBufferAddress', 'nvnMemoryPoolMap',
            'nvnTextureGetFlags', 'nvnTextureBuilderSetFlags', 'nvnTexturePoolInitialize',
            'nvnTexturePoolRegisterTexture', 'nvnTexturePoolFinalize',
        )
        raw = import_fixture(words, names, 0x29e0)
        proc_names = (
            b'nvnMemoryPoolFlushMappedRange', b'nvnMemoryPoolGetFlags',
            b'nvnTextureViewSetDefaults', b'nvnTextureViewSetLevels', b'nvnTextureWriteTexels', b'nvnTextureFlushTexels',
            b'nvnVertexAttribStateSetDefaults', b'nvnVertexAttribStateSetStreamIndex', b'nvnVertexAttribStateSetFormat',
            b'nvnVertexStreamStateSetDefaults', b'nvnVertexStreamStateSetStride', b'nvnVertexStreamStateSetDivisor',
            b'nvnCommandBufferInitialize', b'nvnCommandBufferAddCommandMemory', b'nvnCommandBufferAddControlMemory',
            b'nvnCommandBufferBeginRecording', b'nvnCommandBufferGetCommandMemoryUsed', b'nvnCommandBufferDrawElements',
            b'nvnCommandBufferEndRecording', b'nvnCommandBufferIsRecording', b'nvnDeviceGetTextureHandle',
            b'nvnMemoryPoolGetSize', b'nvnMemoryPoolSetDebugLabel', b'nvnCommandBufferSetRecordingMode',
            b'nvnCommandBufferBarrier', b'nvnTextureBuilderSetTarget', b'nvnTextureGetTarget',
            b'nvnCommandBufferSetCopyRowStride', b'nvnCommandBufferGetCopyRowStride',
            b'nvnCommandBufferSetCopyImageStride', b'nvnCommandBufferGetCopyImageStride',
        )
        for offset, name in enumerate(proc_names, 1):
            slot = 0x1700 + offset * 0x40
            self.assertFalse(any(raw[slot:slot + len(name) + 1]))
            raw[slot:slot + len(name)] = name
        clear_slot = 0x1700
        clear_name = b'nvnDeviceRegisterFastClearColor'
        assert not any(raw[clear_slot:clear_slot + len(clear_name) + len(b'Index\0')])
        raw[clear_slot:clear_slot + len(clear_name)] = clear_name
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            win = rewrite(root, words, 'windows-arm64', name='fixture', raw=raw)
            self.assertEqual(win.returncode, 0, win.stdout + win.stderr)
            sdk = rewrite(root, (RET,), 'windows-arm64', name='sdk', raw=nvn_sdk_fixture())
            self.assertEqual(sdk.returncode, 0, sdk.stdout + sdk.stderr)
            args = [str(build / 'm2_host.exe'), '--fixture', str(root), 'fixture', 'sdk']
            env = os.environ.copy()
            env.pop('DM_WINDOW', None)
            run = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertEqual([line for line in run.stdout.splitlines() if line.startswith('[nvn] headless present #')], [
                '[nvn] headless present #1 (texture 0)', '[nvn] headless present #2 (texture 1)',
                '[nvn] headless present #3 (texture 0)',
            ])
            if os.environ.get('DM_WINARM64_M4_WSI'):
                windowed = subprocess.run(args, env=env | {'DM_WINDOW': '1'},
                                          capture_output=True, text=True, timeout=30)
                self.assertEqual(windowed.returncode, 0, windowed.stdout + windowed.stderr)
                self.assertIn('[nvn] Vulkan present #3 (texture 0)', windowed.stdout)
            module = root / 'fixture.winarm64.patched.bin'
            fixture = bytearray(module.read_bytes())
            for suffix in (b'i', b'ui', b'Index'):
                with self.subTest(suffix=suffix):
                    name = clear_name + suffix + b'\0'
                    fixture[clear_slot:clear_slot + len(name)] = name
                    module.write_bytes(fixture)
                    run = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)
                    if suffix == b'Index':
                        self.assertNotEqual(run.returncode, 0)
                        self.assertIn('nvnDeviceRegisterFastClearColorIndex is not implemented', run.stderr)
                    else:
                        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            fixture[clear_slot + len(clear_name)] = 0
            for mode in (6, 7):  # CPU-inaccessible and CPU-uncached pools
                struct.pack_into('<I', fixture, 0x2fe0, mode)
                module.write_bytes(fixture)
                run = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            for mode, error in ((1, 'nvnQueuePresentTexture: texture 0 was not acquired'),
                                (2, 'nvnDeviceRegisterFastClearColor: invalid device or color'),
                                (3, None), (4, None), (5, None),  # Negative offset, oversized span, past pool end.
                                (8, 'NVN memory pool: invalid pool'),
                                (9, 'nvnMemoryPoolMapVirtual: partial mappings'),
                                (10, 'nvnMemoryPoolFinalize: physical pool still has virtual aliases'),
                                (11, 'nvnTexturePoolRegisterTexture: slot 0 outside writable range'),
                                (12, 'nvnTextureWriteTexels: region exceeds the subresource'),
                                (13, 'nvnCommandBufferBeginRecording: already recording'),
                                (14, 'nvnDeviceGetTextureHandle: invalid device'),
                                (15, 'nvnMemoryPoolGetBufferAddress: invalid pool'),
                                (16, 'nvnCommandBufferSetRecordingMode: unsupported mode 3'),
                                (17, 'nvnCommandBufferSetRecordingMode: unsupported mode 4'),
                                (18, 'NVN command buffer: command outside recording')):
                with self.subTest(mode=mode):
                    struct.pack_into('<I', fixture, 0x2fe0, mode)
                    module.write_bytes(fixture)
                    run = subprocess.run(args, env=env, capture_output=True, text=True, timeout=30)
                    self.assertNotEqual(run.returncode, 0)
                    if error is not None:
                        self.assertIn(error, run.stderr)
                    self.assertNotIn('[nvn] headless present #', run.stdout)

    @unittest.skipUnless(os.environ.get('DM_WINARM64_M3_DIR'), 'requires the native M3 graphics fixture')
    def test_native_command_recording_memory(self):
        build = Path(os.environ['DM_WINARM64_M3_DIR'])
        code = (build / 'm3_graphics_guest.bin').read_bytes()[GRAPHICS_FIXTURE_SPLIT:]
        words = struct.unpack(f'<{len(code) // 4}I', code)
        names = (
            'nvnDeviceBuilderSetDefaults', 'nvnDeviceInitialize', 'nvnMemoryPoolBuilderSetDefaults',
            'nvnMemoryPoolBuilderSetDevice', 'nvnMemoryPoolBuilderSetStorage', 'nvnMemoryPoolInitialize',
            'nvnQueueBuilderSetDefaults', 'nvnQueueBuilderSetDevice', 'nvnQueueInitialize',
            'nvnTextureBuilderSetDefaults', 'nvnTextureBuilderSetDevice', 'nvnTextureBuilderSetFormat',
            'nvnQueueSubmitCommands', 'nvnCommandBufferInitialize', 'nvnCommandBufferAddCommandMemory',
            'nvnCommandBufferAddControlMemory', 'nvnCommandBufferSetMemoryCallback',
            'nvnCommandBufferSetMemoryCallbackData', 'nvnCommandBufferSetCommandMemoryCallbackEnabled',
            'nvnCommandBufferBeginRecording', 'nvnCommandBufferEndRecording', 'nvnCommandBufferCallCommands',
            'nvnCommandBufferCopyCommands', 'nvnCommandBufferCopyBufferToTexture', 'nvnTextureBuilderSetSize2D',
            'nvnTextureBuilderSetStorage', 'nvnTextureInitialize', 'nvnCommandBufferGetCommandMemoryUsed',
        )
        raw = import_fixture(words, names, 0x2600)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            win = rewrite(root, words, 'windows-arm64', name='fixture', raw=raw)
            self.assertEqual(win.returncode, 0, win.stdout + win.stderr)
            sdk = rewrite(root, (RET,), 'windows-arm64', name='sdk', raw=nvn_sdk_fixture())
            self.assertEqual(sdk.returncode, 0, sdk.stdout + sdk.stderr)
            args = [str(build / 'm2_host.exe'), '--fixture', str(root), 'fixture', 'sdk']
            run = subprocess.run(args, capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('first headless submission: 1 command lists, 4 buffer-to-texture copies', run.stdout)
            module = root / 'fixture.winarm64.patched.bin'
            fixture = bytearray(module.read_bytes())
            struct.pack_into('<I', fixture, 0x1780, 1)
            module.write_bytes(fixture)
            run = subprocess.run(args, capture_output=True, text=True, timeout=30)
            self.assertNotEqual(run.returncode, 0)
            self.assertIn('NVN command buffer: command memory exhausted', run.stderr)


if __name__ == '__main__': unittest.main()
