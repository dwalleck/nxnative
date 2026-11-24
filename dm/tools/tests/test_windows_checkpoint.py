"""Checkpoint backup, exact route evidence, and status without a game or GPU."""

from contextlib import closing
import io
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest


SCRIPTS = Path(__file__).resolve().parents[3] / 'scripts'
sys.path.insert(0, str(SCRIPTS))
from windows_checkpoint import Observation, backup_save, pane_opacity, read_frame, SAVE_DB  # noqa: E402


SLOT_OPEN = b'[fs] open save:/slot_03/progress.sav -> 0x106e0\n'
MESH = b'[nvn] first mesh task draw completed (first 0, count 2)\n'
HID = b'[hid] DM_CTL press returned at 6603510 ms: id=0 sample=362 buttons=0x1 sticks=(0,0,0,0)\n'


def create_save(path, slot):
    with closing(sqlite3.connect(path)) as db:
        db.executescript('PRAGMA application_id=0x4e585356; PRAGMA user_version=1;'
                         'CREATE TABLE node(id INTEGER PRIMARY KEY, parent INTEGER, name BLOB, kind INTEGER, size INTEGER);'
                         "INSERT INTO node VALUES(1,NULL,x'',0,0)")
        db.execute('INSERT INTO node VALUES(2,1,?,0,0)', (slot,))
        db.execute('INSERT INTO node VALUES(3,2,?,1,81)', (b'progress.sav',))
        db.commit()


class WindowsCheckpoint(unittest.TestCase):
    def test_backup_copies_the_selected_slot_without_writing_the_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'original.sqlite3'
            create_save(source, b'slot_03')
            original = source.read_bytes()
            target = root / 'save-copy' / SAVE_DB
            self.assertEqual(backup_save(source, target, time.monotonic() + 5), 81)
            self.assertEqual(source.read_bytes(), original)
            self.assertEqual({p.name for p in root.iterdir()}, {'original.sqlite3', 'save-copy'})
            with closing(sqlite3.connect(target)) as copy:
                copy.execute('DELETE FROM node')
                copy.commit()
            self.assertEqual(source.read_bytes(), original)

    def test_backup_rejects_progress_in_the_wrong_slot(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            source = root / 'original.sqlite3'
            create_save(source, b'slot_02')
            with self.assertRaisesRegex(ValueError, 'lacks a non-empty save:/slot_03/progress.sav'):
                backup_save(source, root / 'save-copy' / SAVE_DB, time.monotonic() + 5)

    def test_route_requires_actual_vulkan_and_the_exact_successful_open(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ctl = root / 'input.txt'
            ctl.touch()
            obs = Observation(root, capture=True)
            path = root / 'overlay.arm'
            for n in (120, 180, 240, 300, 360):
                obs.drain(io.BytesIO(f'[nvn] skipped present #{n} (texture 2)\n'.encode()))
                obs.drive()
                self.assertEqual(ctl.read_bytes(), b'A~0.4\n' * ((n - 120) // 60))
                obs.drain(io.BytesIO(f'[nvn] Vulkan present #{n} (texture 2)\n'.encode()))
                obs.drive()
            self.assertEqual(ctl.read_bytes(), b'A~0.4\n' * 5)
            obs.drain(io.BytesIO(b'[fs] open save:/slot_02/progress.sav -> 0x23\n'
                                 b'[fs] open save:/slot_03/progress.sav.bak -> 0x24\n'
                                 b'[fs] open save:/slot_03/progress.sav -> 0x0\n' + MESH + HID))
            obs.drive()
            self.assertFalse(path.exists())
            self.assertFalse(obs.checkpoint['verified'])
            obs.drain(io.BytesIO(SLOT_OPEN))
            obs.drive()
            self.assertFalse(path.exists())
            obs.drain(io.BytesIO(MESH))
            obs.drive()
            self.assertEqual(path.read_bytes(), b'')
            self.assertTrue(obs.checkpoint['verified'])
            self.assertEqual(obs.data['last_acknowledged_hid']['line'], HID.decode().strip())

    def test_confirmation_stops_on_the_exact_open_even_in_one_log_batch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            ctl = root / 'input.txt'
            ctl.touch()
            obs = Observation(root, capture=False)
            obs.drain(io.BytesIO(b'[nvn] Vulkan present #120 (texture 2)\n' + SLOT_OPEN))
            obs.drive()
            self.assertEqual(ctl.read_bytes(), b'')
            self.assertFalse(obs.checkpoint['verified'])
            self.assertFalse((root / 'overlay.arm').exists())

    def test_black_capture_checks_every_rgb_byte(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'last.ppm'
            header = b'P6\n# Vulkan present 657, source BGRA8\n2 1\n255\n'
            path.write_bytes(header + bytes(6))
            frame, _, black = read_frame(path)
            self.assertEqual(frame, 657)
            self.assertTrue(black)
            path.write_bytes(header + bytes(5) + b'\1')
            self.assertFalse(read_frame(path)[2])
            path.write_bytes(header + bytes(5))
            with self.assertRaisesRegex(ValueError, 'incomplete frame payload'):
                read_frame(path)

    def test_opacity_requires_the_known_supported_draw_and_finite_inputs(self):
        terms = {'status': 'paired', 'ps_inputs': '0.75,0.5,0.5', 'vs_inputs': '1,1,1,1',
                 'opacity_scope': 'source_alpha_blend', 'next_opacity_scope': 'source_alpha_blend',
                 'next_status': 'matched', 'next_ps_inputs': '0.5,0.25,0.5', 'next_vs_inputs': '0,0.5,1,0.25'}
        self.assertEqual(pane_opacity(terms, '', True), 0.5)
        self.assertEqual(pane_opacity(terms, 'next_', True), (0.0, 0.1875, 0.375, 0.09375))
        self.assertEqual(pane_opacity(terms, '', False), 'unavailable')
        self.assertEqual(pane_opacity(terms | {'next_status': 'no_matching_draw'}, 'next_', True), 'unavailable')
        self.assertEqual(pane_opacity(terms | {'opacity_scope': 'fragment_alpha_only'}, '', True), 'unavailable')
        self.assertEqual(pane_opacity(terms | {'ps_inputs': '0.75,NaN,0.5'}, '', True), 'unavailable')

    @unittest.skipUnless(sys.platform == 'win32' and os.environ.get('DM_WINARM64_M2_DIR'),
                         'requires the native overlay fixture from build_host.cmd')
    def test_blit_must_cover_the_presented_crop_not_the_whole_backing(self):
        fixture = Path(os.environ['DM_WINARM64_M2_DIR']) / 'm4_overlay_smoke.exe'
        for mode in ('inside', 'outside'):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / 'overlay.arm').touch()
                env = dict(os.environ, DM_CAPTURE_DIR=tmp, DM_OVERLAY_CAPTURE='1',
                           DM_OVERLAY_VS='01' + '00' * 7, DM_OVERLAY_PS='02' + '00' * 7,
                           DM_OVERLAY_BLIT='03' + '00' * 7 + ':1',
                           DM_OVERLAY_PS_INPUTS='1:0', DM_OVERLAY_VS_INPUTS='1:0')
                result = subprocess.run([fixture, mode], env=env, capture_output=True, timeout=10)
                self.assertEqual(result.returncode, 0, (result.stdout, result.stderr))
                fields = dict(line.split('\t', 1) for line in (root / 'overlay-w1-p2.tsv').read_text().splitlines())
                if mode == 'outside':
                    self.assertEqual((fields['status'], fields['before'], fields['next_status']),
                                     ('unlinked_target', 'unavailable', 'unlinked_target'))
                else:
                    self.assertEqual((fields['link'], fields['status'], fields['next_status']), ('blit', 'paired', 'matched'))
                    self.assertEqual((fields['ps_inputs'], fields['next_ps_inputs']), ('1', '0.75'))
                    self.assertEqual((root / fields['before']).read_bytes().split(b'\n255\n', 1)[1],
                                     bytes([0, 0, 0, 8, 16, 32, 0, 0, 0, 1, 0, 0]))
                    self.assertEqual((root / fields['after']).read_bytes().split(b'\n255\n', 1)[1], bytes(12))

    def test_status_keeps_stopped_errors_and_does_not_invent_checkpoint_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            obs = Observation(root, capture=False)
            obs.drain(io.BytesIO(b'[nvn] Vulkan present #60 (texture 2)\n[RED] missing shader\n'))
            obs.data.update(pid=271, phase='stopped', exit_code=1, reason='host_exited', error='[RED] missing shader')
            obs.publish()
            result = subprocess.run([sys.executable, str(SCRIPTS / 'windows_checkpoint.py'),
                                     'status', '--run-dir', str(root)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            state = json.loads(result.stdout)
            self.assertEqual((state['pid'], state['foreground'], state['exit_code']), (271, False, 1))
            self.assertEqual(state['error'], '[RED] missing shader')
            self.assertEqual(state['last_present']['frame'], 60)
            self.assertGreaterEqual(state['last_present_age_s'], 0)
            self.assertFalse(state['checkpoint']['verified'])


if __name__ == '__main__':
    unittest.main()
