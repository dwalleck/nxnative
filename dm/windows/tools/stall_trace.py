"""One CPU trace per Windows diagnostic run, driven by accepted WSI presents."""

import mmap
import os
from pathlib import Path
import shutil
import struct
import subprocess
import time
import uuid


class StallTrace:
    def __init__(self, root: Path, host: Path, seconds: float):
        self.root, self.helper, self.seconds = root, host.with_name('m2_cpu_trace.exe'), seconds
        if seconds:
            for path in (self.helper, host.with_suffix('.pdb')):
                if not path.is_file():
                    raise ValueError(f'missing {path}; rebuild with build_host.cmd or disable profiling with --stall-seconds 0')
        self.name = 'Local\\nxnative-present-' + uuid.uuid4().hex
        self.clock = self.process = None
        self.count, self.changed, self.started = -1, 0., 0.
        self.state = {'status': 'armed' if seconds else 'disabled', 'threshold_s': seconds}

    def __enter__(self):
        if self.seconds:
            self.clock = mmap.mmap(-1, 8, tagname=self.name)
            self.clock[:] = struct.pack('<q', -1)  # host sets zero when its window attaches
        return self

    def __exit__(self, *_):
        try:
            if self.process is not None:
                self.stop('player or monitor stopped during CPU trace')
            elif self.state['status'] == 'armed':
                self.state.update(status='not_triggered', accepted_presents=max(0, self.count))
        finally:
            if self.clock is not None:
                self.clock.close()

    def freeze_frame(self):
        """Preserve an already-published capture, not a screenshot of the desktop."""
        dst = self.root / 'stall-frame.ppm'
        for name in ('last.ppm', 'm4-latest.ppm'):
            source = self.root / name
            try:
                try:
                    os.link(source, dst)  # later atomic PPM replacements leave this inode intact
                except FileNotFoundError:
                    continue
                except OSError:
                    shutil.copyfile(source, dst)
                with dst.open('rb') as f:
                    first, comment = f.readline(64), f.readline(96)
                if first != b'P6\n' or not comment.startswith(b'# Vulkan present '):
                    raise ValueError('not a Vulkan present PPM')
                return {'file': dst.name, 'present': int(comment.split()[3].rstrip(b','))}
            except (OSError, ValueError, IndexError) as error:
                return {'error': str(error)}
        return {'error': 'no published PPM at trigger'}

    def update(self, player, created, last_present, now):
        if self.clock is None or self.state['status'] not in ('armed', 'recording'):
            return
        count, = struct.unpack_from('<q', self.clock)
        if count < 0 and last_present:
            raise ValueError('player presented without the diagnostic heartbeat; rebuild with build_host.cmd')
        if self.process is not None:
            result = self.process.poll()
            if result is not None:
                self.finish(result)
            elif now - self.started >= 15:
                self.stop('CPU trace exceeded its 15-second deadline')
            return
        if count != self.count:
            self.count, self.changed = count, now
        if count <= 0 or now - self.changed < self.seconds:
            return
        self.started = now
        path = self.root / 'stall-cpu.txt'
        self.state.update(status='recording', file=path.name, pid=player.pid, trigger_unix_time=time.time(),
                          accepted_presents=count, gap_s=round(now - self.changed, 3),
                          last_observed_present=last_present, frame=self.freeze_frame())
        try:
            with path.open('xb') as out:
                self.process = subprocess.Popen([self.helper, str(player.pid), '--created-100ns', str(created)],
                                                stdin=subprocess.DEVNULL,
                                                stdout=out, stderr=subprocess.STDOUT,
                                                creationflags=subprocess.CREATE_NO_WINDOW)
        except OSError as error:
            self.state.update(status='error', error=str(error))
            raise

    def finish(self, code):
        self.state.update(status={0: 'complete', 1: 'partial'}.get(code, 'error'), exit_code=code)
        self.process = None

    def stop(self, reason):
        code = self.process.poll()
        if code is not None:
            self.finish(code)
        else:
            self.process.kill()
            self.finish(self.process.wait(timeout=5))
            self.state.update(status='interrupted', error=reason)
