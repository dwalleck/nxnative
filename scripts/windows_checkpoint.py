#!/usr/bin/env python3
"""Run the Windows ARM64 Ocarina forest save checkpoint without touching its source save."""

from __future__ import annotations

import argparse
from contextlib import closing, contextmanager
import ctypes as ct
from ctypes import wintypes as wt
import json
import math
import os
from pathlib import Path
import re
import sqlite3
import subprocess
import sys
import time
from typing import BinaryIO


REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'dm/windows/tools'))
from diagnostic_series import load_series, start_series, write_run_manifest
from stall_trace import StallTrace

PROFILE = REPO / 'dm/games/0400486024DEC000/game.tsv'
SAVE_DB = 'nxnative-save-53574e415449564578626c0000000001.sqlite3'
SLOT = 'save:/slot_03/progress.sav'
RESERVE = 5 << 30
CONFIRM_AT = (120, 180, 240, 300, 360)
VULKAN_PRESENT = re.compile(r'\[nvn\] Vulkan present #(\d+) \(texture \d+\)')
SLOT_OPEN = re.compile(rf'\[fs\] open {re.escape(SLOT)} -> 0x[1-9a-f][0-9a-f]*')
MESH = re.compile(r'\[nvn\] first mesh task draw completed \(first \d+, count [1-9]\d*\)')
HID = re.compile(r'\[hid\] DM_CTL (?:press|release) returned at \d+ ms: id=\d+ sample=\d+ '
                 r'buttons=(?:0x[0-9a-f]+|0) sticks=\(-?\d+,-?\d+,-?\d+,-?\d+\)')
OVERLAY_PRESENT = re.compile(r'\[m4 overlay\] window=(\d+) present=(\d+) guest_frame=(\d+) '
                             r'black=(0|1|unavailable) armed=([01])')
OVERLAY_COMPLETE = re.compile(r'\[m4 overlay\] complete window=(\d+) present=(\d+) next_present=(\d+)')
PPM_PRESENT = re.compile(rb'# Vulkan present ([1-9]\d*), source (?:BGRA8|RGBA8)\r?\n')
OVERLAY_ENV = ('DM_OVERLAY_VS', 'DM_OVERLAY_PS', 'DM_OVERLAY_PS_INPUTS', 'DM_OVERLAY_VS_INPUTS', 'DM_OVERLAY_BLIT')


class MemoryStatus(ct.Structure):
    _fields_ = [('length', wt.DWORD), ('load', wt.DWORD)] + [
        (name, ct.c_uint64) for name in (
            'physical', 'free_physical', 'commit', 'free_commit', 'virtual', 'free_virtual', 'extended',
        )
    ]


class BasicJobLimits(ct.Structure):
    _fields_ = [('process_time', ct.c_int64), ('job_time', ct.c_int64), ('flags', wt.DWORD),
                ('min_working_set', ct.c_size_t), ('max_working_set', ct.c_size_t), ('process_limit', wt.DWORD),
                ('affinity', ct.c_size_t), ('priority_class', wt.DWORD), ('scheduling_class', wt.DWORD)]


class JobLimits(ct.Structure):
    _fields_ = [('basic', BasicJobLimits), ('io_counters', ct.c_uint64 * 6),
                ('process_memory', ct.c_size_t), ('job_memory', ct.c_size_t),
                ('peak_process_memory', ct.c_size_t), ('peak_job_memory', ct.c_size_t)]


RUN_ENV_RESERVED = ('DM_GAME', 'DM_ROMFS', 'DM_SAVE', 'DM_VK_SHADERS', 'DM_CAPTURE_DIR', 'DM_CTL')


def parse_extra_env(assignments: list[str]) -> dict[str, str]:
    """The DM_* variables a caller adds with --env, after the runner stripped its own."""
    extra = {}
    for assignment in assignments:
        name, separator, value = assignment.partition('=')
        if not separator or not name.startswith('DM_') or name in RUN_ENV_RESERVED:
            raise ValueError(f'--env expects DM_<name>=<value> outside {", ".join(RUN_ENV_RESERVED)}: {assignment!r}')
        extra[name] = value
    return extra


def winerror() -> OSError:
    return getattr(ct, 'WinError')()


class Windows:
    def __init__(self):
        dll = getattr(ct, 'WinDLL')
        kernel = dll('kernel32', use_last_error=True)
        user = dll('user32', use_last_error=True)

        def bind(library, name, result, *args):
            call = getattr(library, name)
            call.restype, call.argtypes = result, args
            return call

        handle, dword, boolean = wt.HANDLE, wt.DWORD, wt.BOOL
        filetime = ct.POINTER(wt.FILETIME)
        self.close = bind(kernel, 'CloseHandle', boolean, handle)
        self.mutex = bind(kernel, 'CreateMutexW', handle, ct.c_void_p, boolean, wt.LPCWSTR)
        self.job = bind(kernel, 'CreateJobObjectW', handle, ct.c_void_p, wt.LPCWSTR)
        self.job_limits = bind(kernel, 'SetInformationJobObject', boolean, handle, ct.c_int, ct.c_void_p, dword)
        self.job_assign = bind(kernel, 'AssignProcessToJobObject', boolean, handle, handle)
        self.memory_status = bind(kernel, 'GlobalMemoryStatusEx', boolean, ct.POINTER(MemoryStatus))
        self.open_process = bind(kernel, 'OpenProcess', handle, dword, boolean, dword)
        self.process_times = bind(kernel, 'GetProcessTimes', boolean, handle, filetime, filetime, filetime, filetime)
        self.process_exit = bind(kernel, 'GetExitCodeProcess', boolean, handle, ct.POINTER(dword))
        self.wait = bind(kernel, 'WaitForSingleObject', dword, handle, dword)
        self.foreground_window = bind(user, 'GetForegroundWindow', wt.HWND)
        self.window_pid = bind(user, 'GetWindowThreadProcessId', dword, wt.HWND, ct.POINTER(dword))
        self.open_file = bind(kernel, 'CreateFileW', handle, wt.LPCWSTR, dword, dword, ct.c_void_p, dword, dword, handle)

    @contextmanager
    def player_job(self):
        mutex = self.mutex(None, True, r'Global\nxnative.WindowsCheckpoint')
        if not mutex:
            raise winerror()
        try:
            if getattr(ct, 'get_last_error')() == 183:  # ERROR_ALREADY_EXISTS
                raise ValueError('another Windows checkpoint runner already owns the player')
            job = self.job(None, None)
            if not job:
                raise winerror()
            try:
                limits = JobLimits()
                limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
                if not self.job_limits(job, 9, ct.byref(limits), ct.sizeof(limits)):
                    raise winerror()
                yield job
            finally:
                self.close(job)
        finally:
            self.close(mutex)

    def memory(self) -> tuple[int, int]:
        m = MemoryStatus()
        m.length = ct.sizeof(m)
        if not self.memory_status(ct.byref(m)):
            raise winerror()
        return m.free_physical, m.free_commit

    def creation_time(self, handle) -> int:
        created, exited, kernel, user = (wt.FILETIME() for _ in range(4))
        if not self.process_times(handle, ct.byref(created), ct.byref(exited), ct.byref(kernel), ct.byref(user)):
            raise winerror()
        return (created.dwHighDateTime << 32) | created.dwLowDateTime

    def attach(self, job, player: subprocess.Popen) -> int | None:
        # PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_QUOTA | PROCESS_TERMINATE
        handle = self.open_process(0x1101, False, player.pid)
        if not handle:
            if player.poll() is not None:
                return None
            raise winerror()
        try:
            if not self.job_assign(job, handle) and player.poll() is None:
                raise winerror()
            return self.creation_time(handle)
        finally:
            self.close(handle)

    def foreground(self, pid: int) -> bool:
        hwnd = self.foreground_window()
        owner = wt.DWORD()
        return bool(hwnd and self.window_pid(hwnd, ct.byref(owner)) and owner.value == pid)

    def player_status(self, pid: int, created: int) -> tuple[bool | None, int | None]:
        # Query-and-synchronize access; compare creation times to reject a reused PID.
        handle = self.open_process(0x101000, False, pid)
        if not handle:
            error = getattr(ct, 'get_last_error')()
            return (False if error == 87 else None), None  # ERROR_INVALID_PARAMETER for a vanished PID
        try:
            if self.creation_time(handle) != created:
                return False, None
            state = self.wait(handle, 0)
            if state == 258:  # WAIT_TIMEOUT: the process has not exited
                return True, None
            code = wt.DWORD()
            if state == 0 and self.process_exit(handle, ct.byref(code)):
                return False, code.value
            return None, None
        finally:
            self.close(handle)

    def read_text(self, path: Path) -> str:
        # A status/manifest reader must not block the writer's atomic replacement.
        from importlib import import_module
        open_osfhandle = import_module('msvcrt').open_osfhandle
        handle = self.open_file(str(path), 0x80000000, 7, None, 3, 0, None)
        if handle == ct.c_void_p(-1).value:
            raise winerror()
        try:
            fd = open_osfhandle(handle, os.O_RDONLY | getattr(os, 'O_BINARY'))
        except OSError:
            self.close(handle)
            raise
        with os.fdopen(fd, encoding='utf-8') as source:
            return source.read()


def read_pairs(text: str) -> dict[str, str]:
    settings = {}
    for line in text.splitlines():
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        name, tab, value = line.partition('\t')
        if not tab or not name or not value or '\t' in value or name in settings:
            raise ValueError(f'invalid key/value TSV line: {line!r}')
        settings[name] = value
    return settings


def overlay_settings(profile: Path) -> dict[str, str]:
    path = profile.with_name('windows_overlay.tsv')
    settings = read_pairs(path.read_text(encoding='utf-8'))
    if settings.keys() != set(OVERLAY_ENV):
        raise ValueError(f'{path} must contain exactly {", ".join(OVERLAY_ENV)}')
    for key in ('DM_OVERLAY_VS', 'DM_OVERLAY_PS'):
        if not re.fullmatch(r'[0-9a-fA-F]{16}', settings[key]):
            raise ValueError(f'{path}: {key} must be a 16-digit hexadecimal shader key')
        settings[key] = settings[key].lower()
    for key, terms in (('DM_OVERLAY_PS_INPUTS', 3), ('DM_OVERLAY_VS_INPUTS', 4)):
        if not re.fullmatch(rf'\d+:\d+(?:,\d+){{{terms - 1}}}', settings[key]):
            raise ValueError(f'{path}: {key} must be binding:offset for {terms} comma-separated byte offsets')
    if not re.fullmatch(r'[0-9a-fA-F]{16}:\d+', settings['DM_OVERLAY_BLIT']):
        raise ValueError(f'{path}: DM_OVERLAY_BLIT must be a 16-digit shader key and sampler binding')
    settings['DM_OVERLAY_BLIT'] = settings['DM_OVERLAY_BLIT'].lower()
    return settings


def pane_opacity(fields: dict[str, str], prefix: str, known_selector: bool):
    if not known_selector or fields.get(prefix + 'status') != ('matched' if prefix else 'paired'):
        return 'unavailable'
    if fields.get(prefix + 'opacity_scope') != 'source_alpha_blend':
        return 'unavailable'
    try:
        a, b, c = map(float, fields[prefix + 'ps_inputs'].split(','))
        corners = tuple(map(float, fields[prefix + 'vs_inputs'].split(',')))
    except (KeyError, ValueError):
        return 'unavailable'
    if len(corners) != 4 or not all(math.isfinite(x) for x in (a, b, c, *corners)):
        return 'unavailable'
    material = min(1.0, max(0.0, a + b)) * c
    opacity = tuple(material * corner for corner in corners)
    if not all(math.isfinite(x) for x in opacity):
        return 'unavailable'
    return opacity[0] if all(x == opacity[0] for x in opacity) else opacity


def save_source(path: Path) -> Path:
    if path.is_dir():
        path /= SAVE_DB
    if not path.is_file():
        raise ValueError(f'no NxNative account SQLite database: {path}')
    return path.resolve()


def backup_save(source: Path, target: Path, deadline: float) -> int:
    # Windows NxNative uses rollback journals and rejects WAL/SHM sidecars. Do not
    # open a WAL source: even a read-only SQLite connection can update its SHM.
    with source.open('rb') as file:
        header = file.read(20)
    if not header.startswith(b'SQLite format 3\0') or header[18:20] != b'\1\1' or any(
        Path(str(source) + suffix).exists() for suffix in ('-wal', '-shm')
    ):
        raise ValueError('save source must be a Windows NxNative SQLite database in rollback-journal mode')
    target.parent.mkdir()

    def check_deadline(_status, _remaining, _total):
        if time.monotonic() >= deadline:
            raise TimeoutError('timed out backing up the source save')

    with closing(sqlite3.connect(source.as_uri() + '?mode=ro', uri=True, timeout=0.25)) as original:
        with closing(sqlite3.connect(target)) as copy:
            original.backup(copy, pages=64, progress=check_deadline, sleep=0.05)
            if (copy.execute('PRAGMA application_id').fetchone() != (0x4e585356,) or
                    copy.execute('PRAGMA user_version').fetchone() != (1,)):
                raise ValueError('save source is not a Windows NxNative account database (schema 1)')
            progress = copy.execute(
                'SELECT file.size FROM node AS slot JOIN node AS file ON file.parent=slot.id '
                'WHERE slot.parent=1 AND slot.name=? AND slot.kind=0 AND file.name=? AND file.kind=1',
                (b'slot_03', b'progress.sav'),
            ).fetchone()
            if progress is None or progress[0] <= 0:
                raise ValueError(f'save backup lacks a non-empty {SLOT}')
            return progress[0]


def read_frame(path: Path) -> tuple[int, float, bool]:
    with path.open('rb') as file:
        if file.readline(64) != b'P6\n':
            raise ValueError('bad frame magic')
        match = PPM_PRESENT.fullmatch(file.readline(128))
        if not match:
            raise ValueError('no Vulkan present number in frame header')
        width, height = map(int, file.readline(64).split())
        if width <= 0 or height <= 0 or file.readline(16) != b'255\n':
            raise ValueError('bad RGB8 frame header')
        stamp = os.fstat(file.fileno())
        size = 3 * width * height
        if stamp.st_size - file.tell() != size:
            raise ValueError('incomplete frame payload')
        while size:
            rgb = file.read(min(size, 256 << 10))
            if not rgb:
                raise ValueError('could not read complete frame payload')
            if rgb.count(b'\0') != len(rgb):
                return int(match[1]), stamp.st_mtime, False
            size -= len(rgb)
        return int(match[1]), stamp.st_mtime, True


class Observation:
    def __init__(self, root: Path, capture: bool):
        self.root = root
        self.checkpoint = {'expected_path': SLOT, 'preflight_bytes': None, 'slot_open': None,
                           'first_mesh': None, 'verified': False}
        self.overlay = {'requested': capture, 'armed': False, 'complete': False, 'result': None}
        self.data = {'pid': None, 'process_created': None, 'phase': 'preparing', 'foreground': False,
                     'last_present': None, 'last_acknowledged_hid': None, 'last_frame': None,
                     'confirmations': [], 'checkpoint': self.checkpoint, 'overlay': self.overlay,
                     'first_red': None, 'exit_code': None, 'error': None, 'reason': None}
        self.pending_confirmation: int | None = None
        self.overlay_complete: tuple[int, int, int] | None = None

    def publish(self):
        temp = self.root / 'status.json.tmp'
        temp.write_text(json.dumps(self.data, indent=2, allow_nan=False) + '\n', encoding='utf-8')
        temp.replace(self.root / 'status.json')

    def present(self, frame: int, stamp: float, source: str, **details):
        last = self.data['last_present']
        if last is None or stamp >= last['unix_time']:
            self.data['last_present'] = {'frame': frame, 'unix_time': stamp, 'source': source, **details}

    def log_line(self, raw: bytes):
        line = raw.decode('utf-8', errors='replace').rstrip('\r\n')
        if '[RED]' in line and self.data['first_red'] is None:
            self.data['first_red'] = line
        if self.checkpoint['slot_open'] is None and SLOT_OPEN.fullmatch(line):
            self.checkpoint['slot_open'] = line
        if self.checkpoint['slot_open'] and self.checkpoint['first_mesh'] is None and MESH.fullmatch(line):
            self.checkpoint['first_mesh'] = line
            self.checkpoint['verified'] = True
        if HID.fullmatch(line):
            self.data['last_acknowledged_hid'] = {'line': line, 'observed_unix_time': time.time()}
        if match := VULKAN_PRESENT.fullmatch(line):
            n = int(match[1])
            last = self.data['last_present']
            if last is None or last.get('guest_frame') != n:
                self.present(n, time.time(), 'sampled_host_log')
            if n in CONFIRM_AT and n not in self.data['confirmations']:
                self.pending_confirmation = n
        elif match := OVERLAY_PRESENT.fullmatch(line):
            self.present(int(match[2]), time.time(), 'overlay_host_log', window=int(match[1]),
                         guest_frame=int(match[3]), black=None if match[4] == 'unavailable' else match[4] == '1')
        elif match := OVERLAY_COMPLETE.fullmatch(line):
            self.overlay_complete = (int(match[1]), int(match[2]), int(match[3]))

    def drain(self, log: BinaryIO, *, final: bool = False):
        # Leave an incomplete CRT/log write for the next pass rather than dropping it.
        budget = 4096
        while final or budget:
            start = log.tell()
            line = log.readline()
            if not line:
                break
            if not final and not line.endswith(b'\n'):
                log.seek(start)
                break
            self.log_line(line)
            budget -= 1

    def drive(self):
        if self.pending_confirmation and not self.checkpoint['slot_open']:
            with (self.root / 'input.txt').open('ab') as ctl:
                ctl.write(b'A~0.4\n')
            self.data['confirmations'].append(self.pending_confirmation)
        self.pending_confirmation = None
        if self.overlay['requested'] and not self.overlay['armed'] and self.checkpoint['verified']:
            (self.root / 'overlay.arm').touch(exist_ok=False)
            self.overlay['armed'] = True

    def capture_frame(self, history):
        # The host treats absence of m4-latest.ppm as a request for its next
        # accepted present. Move the completed file before opening it, so the
        # monitor never holds the renderer's publication path open on Windows.
        latest, last = self.root / 'm4-latest.ppm', self.root / 'last.ppm'
        try:
            latest.replace(last)
        except (FileNotFoundError, PermissionError):
            return
        try:
            frame, stamp, black = read_frame(last)
        except (OSError, ValueError) as error:
            self.data['frame_error'] = str(error)
            return
        self.data.pop('frame_error', None)
        self.data['last_frame'] = {'frame': frame, 'unix_time': stamp, 'black': black, 'image': last.name}
        self.present(frame, stamp, 'captured_ppm', black=black)
        history.write(f'{frame}\t{stamp:.6f}\t{int(black)}\n')

    def read_overlay(self, win: Windows, known_selector: bool, *, final: bool = False):
        if not self.overlay['armed']:
            return
        complete = self.overlay_complete
        if complete:
            window, frame, _ = complete
            manifest = self.root / f'overlay-w{window}-p{frame}.tsv'
        elif final:
            manifest = next(iter(sorted(self.root.glob('overlay-w*-p*.tsv'))), None)
            if manifest is None:
                return
        else:
            # Do not open the first-present manifest while the renderer can
            # still be replacing it with the next-present result.
            return
        fields = read_pairs(win.read_text(manifest))
        self.overlay['result'] = {
            'opacity_scope': fields.get('opacity_scope', 'unavailable'),
            'next_opacity_scope': fields.get('next_opacity_scope', 'unavailable'),
            'manifest': manifest.name, 'evidence': fields,
            'opacity': pane_opacity(fields, '', known_selector),
            'next_opacity': pane_opacity(fields, 'next_', known_selector),
        }
        if complete:
            if (fields.get('window'), fields.get('present'), fields.get('next_present')) != tuple(map(str, complete)):
                raise ValueError(f'overlay completion disagrees with {manifest.name}')
            if fields.get('next_status') in (None, 'pending'):
                raise ValueError(f'overlay completion missing next result in {manifest.name}')
            self.overlay['complete'] = True


def launch(args, root: Path, win: Windows, obs: Observation) -> int:
    deadline = time.monotonic() + args.timeout
    start = time.monotonic()
    player = None
    reason = 'setup_error'
    runner_error = None
    settings = {}
    known_selector = False
    profile, host, rewrite, romfs, shaders = (getattr(args, name).resolve() for name in
                                              ('profile', 'host', 'rewrite', 'romfs', 'shaders'))
    try:
        if min(win.memory()) < RESERVE:
            raise ValueError('less than 5 GiB free physical memory or commit; not launching')
        for path in (profile, host, romfs):
            if not path.is_file():
                raise ValueError(f'expected a file: {path}')
        for path in (rewrite, shaders):
            if not path.is_dir():
                raise ValueError(f'expected a directory: {path}')
        series = load_series(REPO, args.series_dir)
        if args.overlay_capture:
            settings = overlay_settings(profile)
            # The shipped Ocarina profile defines the inputs for the pane formula.
            known_selector = profile == PROFILE or settings == overlay_settings(PROFILE)
        scratch = root / 'save-copy'
        source = save_source(args.save)
        obs.checkpoint['preflight_bytes'] = backup_save(source, scratch / SAVE_DB, deadline)
        ctl = root / 'input.txt'
        ctl.touch(exist_ok=False)
        extra = parse_extra_env(args.env)
        env = {key: value for key, value in os.environ.items() if not key.upper().startswith('DM_')}
        env.update(DM_GAME=profile.parent.name, DM_ROMFS=str(romfs), DM_SAVE=str(scratch),
                   DM_VK_SHADERS=str(shaders), DM_CAPTURE_DIR=str(root), DM_CTL=str(ctl),
                   DM_WINDOW='1', DM_KEYBOARD='1', DM_OPMODE='0', DM_FS='1')
        if args.overlay_capture:
            env.update(settings, DM_OVERLAY_CAPTURE='1')
        env.update(extra)
        with win.player_job() as job, StallTrace(root, host, args.stall_seconds) as trace:
            obs.data['cpu_trace'] = trace.state
            pdb = host.with_suffix('.pdb')
            write_run_manifest(REPO, series, root, host=host, pdb=pdb if pdb.is_file() else None,
                               shader_library=shaders, original_save=source, backup_save=scratch / SAVE_DB,
                               profile=profile, rewrite=rewrite, romfs=romfs,
                               capture_settings={'overlay': args.overlay_capture, 'selectors': settings,
                                                 'stall_seconds': args.stall_seconds, 'timeout_seconds': args.timeout,
                                                 'opmode': env['DM_OPMODE'], 'slot': SLOT, 'env': extra})
            obs.data['manifest'] = 'manifest.json'
            if args.stall_seconds:
                env['DM_PRESENT_COUNTER'] = trace.name
            log_path = root / 'host.log'
            with (log_path.open('xb') as output, log_path.open('rb') as log,
                  (root / 'memory.tsv').open('x', encoding='utf-8', buffering=1) as memory,
                  (root / 'present.tsv').open('x', encoding='utf-8', buffering=1) as history):
                memory.write('elapsed_s\tfree_physical_bytes\tfree_commit_bytes\n')
                history.write('present\tunix_time\tblack_rgb\n')
                if min(win.memory()) < RESERVE:
                    raise ValueError('less than 5 GiB free physical memory or commit; not launching')
                if time.monotonic() >= deadline:
                    raise TimeoutError('checkpoint deadline elapsed before launch')
                player = subprocess.Popen([host, profile, rewrite, 'main', 'subsdk0', 'sdk'], cwd=REPO, env=env,
                                          stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
                                          creationflags=getattr(subprocess, 'CREATE_NO_WINDOW'))
                obs.data.update(pid=player.pid, phase='running')
                try:
                    obs.data['process_created'] = win.attach(job, player)
                    next_memory = next_publish = 0.0
                    while True:
                        done = player.poll() is not None
                        obs.drain(log, final=done)
                        if obs.checkpoint['first_mesh']:
                            obs.capture_frame(history)
                        obs.read_overlay(win, known_selector)
                        if done:
                            reason = 'host_exited'
                            break
                        now = time.monotonic()
                        if now >= next_memory:
                            available = win.memory()
                            memory.write(f'{now - start:.2f}\t{available[0]}\t{available[1]}\n')
                            next_memory = now + 1.0
                            if min(available) < RESERVE:
                                reason = 'memory_guard'
                                runner_error = 'less than 5 GiB free physical memory or commit'
                                break
                        if obs.overlay['complete'] and trace.process is None:
                            reason = 'overlay_complete'
                            break
                        if now >= deadline:
                            reason = 'timeout'
                            runner_error = f'checkpoint timed out after {args.timeout:g} seconds'
                            break
                        trace.update(player, obs.data['process_created'], obs.data['last_present'], now)
                        obs.drive()
                        if now >= next_publish:
                            obs.data['foreground'] = win.foreground(player.pid)
                            obs.publish()
                            next_publish = now + 1.0
                        time.sleep(0.18)
                finally:
                    if player.poll() is None:
                        try:
                            player.terminate()
                        except PermissionError:
                            if player.poll() is None:
                                raise
                    obs.data['exit_code'] = player.wait(timeout=5)
                    obs.drain(log, final=True)
                    if obs.checkpoint['first_mesh']:
                        obs.capture_frame(history)
                    obs.read_overlay(win, known_selector, final=True)
    except KeyboardInterrupt:
        reason, runner_error = 'interrupted', 'checkpoint interrupted'
    except TimeoutError as error:
        reason, runner_error = 'timeout', str(error)
    except (OSError, ValueError, sqlite3.Error, subprocess.SubprocessError) as error:
        reason = 'monitor_error' if player is not None else 'setup_error'
        runner_error = str(error)
    if reason == 'host_exited' and runner_error is None:
        if obs.data['exit_code']:
            runner_error = f'host exited with code {obs.data["exit_code"]}'
        elif not obs.checkpoint['verified']:
            runner_error = f'host exited before verifying {SLOT} and the first mesh'
        elif args.overlay_capture and not obs.overlay['complete']:
            runner_error = 'host exited before recording the overlay and next-present results'
    trace_state = obs.data.get('cpu_trace', {})
    if runner_error is None and trace_state.get('status') in ('error', 'partial', 'interrupted'):
        runner_error = f"CPU trace {trace_state['status']}; see {trace_state.get('file', 'status.json')}"
    obs.data.update(phase='stopped', reason=reason, foreground=False,
                    error=obs.data['first_red'] or runner_error, monitor_error=runner_error)
    obs.publish()
    if obs.data['error']:
        print(obs.data['error'], file=sys.stderr)
    print(f'{reason}: see {root / "status.json"}', flush=True)
    return 130 if reason == 'interrupted' else (1 if obs.data['error'] else 0)


def status(root: Path) -> int:
    win = Windows() if os.name == 'nt' else None
    path = root / 'status.json'
    state = json.loads(win.read_text(path) if win else path.read_text(encoding='utf-8'))
    if state['phase'] == 'running':
        if win and state['pid'] is not None and state['process_created'] is not None:
            alive, exit_code = win.player_status(state['pid'], state['process_created'])
            state['foreground'] = win.foreground(state['pid']) if alive else (False if alive is False else None)
            if alive is False:
                state.update(phase='stopped', exit_code=exit_code, reason='host_no_longer_live')
                state['error'] = state['first_red'] or 'host is no longer live; final monitor status not yet recorded'
        else:
            state['foreground'] = None
    last = state['last_present']
    state['last_present_age_s'] = round(max(0.0, time.time() - last['unix_time']), 2) if last else None
    print(json.dumps(state, indent=2, allow_nan=False))
    return 0


def main() -> int:
    cli = argparse.ArgumentParser(description=__doc__)
    commands = cli.add_subparsers(dest='command', required=True)
    begin = commands.add_parser('start-series', help='fetch and pin upstream once; do not modify the checkout')
    begin.add_argument('--series-dir', type=Path, required=True, help='new directory outside the checkout')
    begin.add_argument('--remote', default='origin', help='configured Git remote (default: origin)')
    begin.add_argument('--branch', default='main', help='upstream branch (default: main)')
    setup = commands.add_parser('run', help='launch one native player and monitor the saved forest route')
    for name, help_text in (
        ('host', 'native Windows ARM64 m2_host executable'),
        ('rewrite', 'Windows ARM64 rewritten modules directory'),
        ('romfs', 'DMROMFS1 image file'),
        ('shaders', 'Windows Vulkan shader library directory'),
        ('save', f'NxNative rollback-mode SQLite file or directory containing {SAVE_DB}; copied read-only'),
        ('run-dir', 'new output directory outside the checkout'),
        ('series-dir', 'existing pin created with start-series; never fetched by run'),
    ):
        setup.add_argument('--' + name, required=True, type=Path, help=help_text)
    setup.add_argument('--profile', type=Path, default=PROFILE, help='game.tsv (default: repo Ocarina profile)')
    setup.add_argument('--overlay-capture', action='store_true', help='arm overlay capture after the verified checkpoint')
    setup.add_argument('--stall-seconds', type=float, default=30, metavar='SECONDS',
                       help='trace once after this gap in accepted presents (default 30; 0 disables)')
    setup.add_argument('--timeout', type=float, default=1800, metavar='SECONDS', help='whole-run deadline (default 1800)')
    setup.add_argument('--env', action='append', default=[], metavar='NAME=VALUE',
                       help='extra DM_* variable for the player (repeatable); the runner strips the caller\'s own')
    query = commands.add_parser('status', help='print checkpoint evidence and live status as JSON')
    query.add_argument('--run-dir', required=True, type=Path)
    args = cli.parse_args()
    try:
        if args.command == 'start-series':
            pin = start_series(REPO, args.series_dir.resolve(), remote=args.remote, branch=args.branch)
            print(f"Pinned {pin['upstream']['commit']} in {args.series_dir.resolve()}; "
                  f"local HEAD {pin['start']['head']} is {pin['start']['ahead']} ahead, {pin['start']['behind']} behind")
            return 0
        root = args.run_dir.resolve()
        if args.command == 'status':
            return status(root)
        if os.name != 'nt':
            raise ValueError('the checkpoint run command requires Windows')
        if not math.isfinite(args.timeout) or args.timeout <= 0:
            raise ValueError('--timeout must be positive and finite')
        if not math.isfinite(args.stall_seconds) or args.stall_seconds < 0:
            raise ValueError('--stall-seconds must be nonnegative and finite')
        if root.is_relative_to(REPO):
            raise ValueError('--run-dir must be outside the checkout')
        for source in (args.save, args.shaders, args.rewrite):
            if source.is_dir() and root.is_relative_to(source.resolve()):
                raise ValueError('--run-dir must not be inside an input directory')
        root.mkdir(parents=True, exist_ok=False)
        win = Windows()
        obs = Observation(root, args.overlay_capture)
        obs.publish()
        return launch(args, root, win, obs)
    except (OSError, ValueError) as error:
        cli.exit(1, f'{cli.prog}: {error}\n')


if __name__ == '__main__':
    sys.exit(main())
