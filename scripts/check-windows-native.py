#!/usr/bin/env python3
"""Gate a local Windows ARM64 build with the full native fixture and docs suites."""

import argparse
import os
from pathlib import Path
import subprocess
import sys
import unittest


REPO = Path(__file__).resolve().parents[1]
REWRITE_TESTS = 'dm.tools.tests.test_windows_rewrite'
CHECKPOINT_TESTS = 'dm.tools.tests.test_windows_checkpoint'
DOCS_TESTS = 'dm.tools.tests.test_docs'

# These are coverage anchors, not a second suite or a fixed test count. In
# particular, the two graphics cases below contain the opt-in WSI assertions.
REQUIRED_CASES = {
    f'{REWRITE_TESTS}.WindowsRewrite.test_native_gate',
    f'{REWRITE_TESTS}.WindowsHost.test_native_module_and_memory',
    f'{REWRITE_TESTS}.WindowsHost.test_native_romfs_and_nvn',
    f'{REWRITE_TESTS}.WindowsHost.test_native_headless_present',
    f'{REWRITE_TESTS}.WindowsHost.test_native_submit_texture_copies',
    f'{REWRITE_TESTS}.WindowsHost.test_sdk_software_opus_pcm',
    f'{REWRITE_TESTS}.WindowsHost.test_sdk_av1_nv12',
    f'{CHECKPOINT_TESTS}.WindowsCheckpoint.test_blit_must_cover_the_presented_crop_not_the_whole_backing',
    f'{DOCS_TESTS}.Links.test_relative_links_and_anchors_resolve',
}


class NativeResult(unittest.TextTestResult):
    def __init__(self, stream, descriptions, verbosity):
        super().__init__(stream, descriptions, verbosity)
        self.pending = REQUIRED_CASES.copy()

    def addSuccess(self, test):
        super().addSuccess(test)
        self.pending.discard(test.id())

    def wasSuccessful(self):
        return super().wasSuccessful() and not (self.pending or self.skipped or self.expectedFailures)


def require_arm64_pe(path: Path) -> None:
    with path.open('rb') as file:
        dos = file.read(64)
        if len(dos) == 64 and dos[:2] == b'MZ':
            file.seek(int.from_bytes(dos[60:64], 'little'))  # e_lfanew
            if file.read(6) == b'PE\0\0\x64\xaa':  # signature + IMAGE_FILE_MACHINE_ARM64
                return
    raise ValueError(f'{path}: expected a native ARM64 PE binary')


def main() -> int:
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--build-dir', required=True, type=Path, help='M2/M3/M4 output from build_host.cmd')
    cli.add_argument('--m1-dir', type=Path, help='M1 probe and guest output (default: --build-dir)')
    cli.add_argument('--rewrite', required=True, type=Path, help='Ocarina Windows rewrite directory outside the checkout')
    args = cli.parse_args()
    if sys.platform != 'win32':
        cli.error('the native gate requires Windows on ARM64')

    try:
        build = args.build_dir.resolve()
        m1 = args.m1_dir.resolve() if args.m1_dir else build
        rewrite = args.rewrite.resolve()
        if rewrite.is_relative_to(REPO):
            raise ValueError('--rewrite must be outside the checkout')
        binaries = (m1 / 'm1_probe.exe', build / 'm2_host.exe', build / 'm4_overlay_smoke.exe',
                    build / 'libdav1d-7.dll')
        # Check the game files the host opens. The suites read the other guest
        # fixtures and check both rewrite compatibility and dav1d's ABI in use.
        modules = (rewrite / f'{name}.winarm64.{suffix}' for name in ('main', 'subsdk0', 'sdk')
                   for suffix in ('patched.bin', 'fixups.tsv', 'gates.tsv'))
        missing = [str(path) for path in (*binaries, m1 / 'm1_guest.bin', *modules) if not path.is_file()]
        if missing:
            raise ValueError('missing native gate inputs:\n  ' + '\n  '.join(missing))
        for path in binaries:
            require_arm64_pe(path)
    except (OSError, ValueError) as error:
        cli.exit(1, f'{cli.prog}: {error}\n')

    # A shell used for real game boots may carry save, controller, shader and
    # display options. Fixtures supply their own inputs; do not inherit those.
    for name in tuple(os.environ):
        if name.startswith('DM_'):
            del os.environ[name]
    os.environ.update(PYTHONUTF8='1', DM_WINARM64_M1_DIR=str(m1), DM_WINARM64_M2_DIR=str(build),
                      DM_WINARM64_M3_DIR=str(build), DM_WINARM64_GAME_REWRITE=str(rewrite), DM_WINARM64_M4_WSI='1')
    os.chdir(REPO)
    sys.path.insert(0, str(REPO))

    # Load only after setting flags: skipUnless predicates run at import time.
    suite = unittest.TestLoader().loadTestsFromNames((REWRITE_TESTS, CHECKPOINT_TESTS, DOCS_TESTS))
    result = unittest.TextTestRunner(verbosity=2, resultclass=NativeResult).run(suite)
    if result.skipped or result.expectedFailures:
        print('Native gate requires every test to run without skips or expected failures.', file=sys.stderr)
    if result.pending:
        print('Required cases did not pass:\n  ' + '\n  '.join(sorted(result.pending)), file=sys.stderr)
    return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
    # The docs suite also reads UTF-8 in this process, not just in child tools.
    if not sys.flags.utf8_mode:
        sys.exit(subprocess.call([sys.executable, '-X', 'utf8', __file__, *sys.argv[1:]]))
    sys.exit(main())
