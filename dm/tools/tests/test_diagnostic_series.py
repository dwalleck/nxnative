"""Exercise diagnostic series with local Git history and synthetic runtime inputs."""

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


TOOLS = Path(__file__).resolve().parents[2] / 'windows' / 'tools'
sys.path.insert(0, str(TOOLS))
from diagnostic_series import SeriesError, load_series, start_series, write_run_manifest  # noqa: E402


def git(repo, *args):
    return subprocess.check_output([
        'git', '-C', str(repo), '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid',
        '-c', 'commit.gpgSign=false', *args,
    ], text=True).strip()


def commit(repo, message):
    git(repo, 'add', '-A')
    git(repo, 'commit', '-m', message)
    return git(repo, 'rev-parse', 'HEAD')


@unittest.skipUnless(shutil.which('git'), 'requires Git')
class DiagnosticSeries(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name).resolve()
        self.upstream = self.root / 'upstream'
        self.upstream.mkdir()
        git(self.upstream, 'init', '-b', 'main')
        (self.upstream / '.gitattributes').write_bytes(b'* -text\n')
        (self.upstream / '.gitignore').write_bytes(b'*.tmp\n')
        (self.upstream / 'tracked.bin').write_bytes(b'\0base binary\xff')
        self.base = commit(self.upstream, 'common base')
        self.repo = self.root / 'checkout'
        git(self.root, 'clone', str(self.upstream), str(self.repo))
        self.series_dir = self.root / 'series'

    def test_moved_upstream_keeps_old_pin_and_records_working_tree_offline(self):
        (self.upstream / 'incoming.txt').write_bytes(b'incoming at pin\n')
        pinned = commit(self.upstream, 'upstream at start')
        (self.repo / 'committed.txt').write_bytes(b'first local commit\n')
        start_head = commit(self.repo, 'local before pin')
        (self.repo / 'pending.txt').write_bytes(b'pending at start\n')
        git(self.repo, 'add', 'pending.txt')
        index = self.repo / '.git' / 'index'
        previous_index = index.read_bytes()

        series = start_series(self.repo, self.series_dir)
        self.assertEqual(series['upstream'], {'remote': 'origin', 'branch': 'main', 'commit': pinned})
        self.assertEqual(series['start'], {'head': start_head, 'ahead': 1, 'behind': 1})
        self.assertEqual(git(self.repo, 'branch', '--show-current'), 'main')
        self.assertEqual(git(self.repo, 'rev-parse', 'HEAD'), start_head)
        self.assertEqual(git(self.repo, 'rev-parse', 'refs/remotes/origin/main'), self.base)
        self.assertEqual(index.read_bytes(), previous_index)

        (self.upstream / 'later.txt').write_bytes(b'newer than pin\n')
        moved = commit(self.upstream, 'upstream moved')
        git(self.repo, 'fetch', 'origin')
        self.assertEqual(git(self.repo, 'rev-parse', 'refs/remotes/origin/main'), moved)
        git(self.repo, 'remote', 'set-url', 'origin', str(self.root / 'unreachable'))
        self.assertEqual(load_series(self.repo, self.series_dir), series)

        run_head = commit(self.repo, 'local after pin')
        (self.repo / 'staged.txt').write_bytes(b'new staged file\n')
        tracked = self.repo / 'tracked.bin'
        tracked.write_bytes(b'\0superseded index contents\xfe')
        git(self.repo, 'add', 'staged.txt', 'tracked.bin')
        actual = b'\0actual working contents\xfd'
        tracked.write_bytes(actual)
        untracked = {'a \u03a9 source.c': b'int a;\n', 'z source.c': b'int z;\n'}
        for name, contents in reversed(tuple(untracked.items())):
            (self.repo / name).write_bytes(contents)
        (self.repo / 'ignored.tmp').write_bytes(b'not part of the inventory')
        previous_index = index.read_bytes()

        run_dir = self.series_dir / 'run-1'
        run_dir.mkdir()
        shaders = self.root / 'shaders'
        (shaders / 'sub').mkdir(parents=True)
        (shaders / 'sub' / 'b.vkr').write_bytes(b'reflection')
        (shaders / 'a.spv').write_bytes(b'shader')
        rewrite = self.root / 'rewrite'
        rewrite.mkdir()
        files = {}
        for name in ('host', 'pdb', 'original_save', 'backup_save', 'profile', 'romfs'):
            path = (run_dir if name == 'backup_save' else self.root) / name
            path.write_bytes(name.encode('ascii'))
            files[name] = path
        capture = {'overlay_capture': True, 'timeout_seconds': 90}
        manifest = write_run_manifest(self.repo, load_series(self.repo, self.series_dir), run_dir,
                                      shader_library=shaders, rewrite=rewrite, capture_settings=capture, **files)
        self.assertEqual(json.loads((run_dir / 'manifest.json').read_text(encoding='utf-8')), manifest)
        self.assertEqual(manifest['series'], series)
        self.assertEqual(manifest['capture_settings'], capture)
        source = manifest['git']
        self.assertEqual((source['head'], source['merge_base'], source['ahead'], source['behind']),
                         (run_head, self.base, 2, 1))
        self.assertEqual(index.read_bytes(), previous_index)
        self.assertEqual(source['untracked'], [
            {'path': name, 'size_bytes': len(contents), 'sha256': hashlib.sha256(contents).hexdigest()}
            for name, contents in untracked.items()
        ])

        patch = run_dir / source['tracked_patch']['path']
        self.assertIn(b'GIT binary patch', patch.read_bytes())
        self.assertEqual(source['tracked_patch']['sha256'], hashlib.sha256(patch.read_bytes()).hexdigest())
        replay = self.root / 'replay'
        git(self.repo, 'worktree', 'add', '--detach', str(replay), self.base)
        self.assertEqual(load_series(replay, self.series_dir), series)
        git(replay, 'apply', str(patch))
        for name, contents in (
            ('committed.txt', b'first local commit\n'), ('pending.txt', b'pending at start\n'),
            ('staged.txt', b'new staged file\n'), ('tracked.bin', actual),
        ):
            self.assertEqual((replay / name).read_bytes(), contents)
        self.assertFalse((replay / 'incoming.txt').exists())

        runtime = manifest['runtime']
        for name in ('host', 'pdb', 'original_save', 'backup_save', 'profile'):
            self.assertEqual(runtime[name]['path'], str(files[name]))
            self.assertEqual(runtime[name]['sha256'], hashlib.sha256(files[name].read_bytes()).hexdigest())
        self.assertEqual(runtime['rewrite']['path'], str(rewrite))
        self.assertEqual(runtime['romfs']['path'], str(files['romfs']))
        self.assertNotIn('sha256', runtime['romfs'])
        shader_hash = hashlib.sha256(b'a.spv\0' + hashlib.sha256(b'shader').digest() +
                                     b'sub/b.vkr\0' + hashlib.sha256(b'reflection').digest()).hexdigest()
        self.assertEqual(runtime['shader_library'], {'path': str(shaders), 'file_count': 2, 'sha256': shader_hash})

    def test_missing_malformed_and_unavailable_pins_are_rejected(self):
        self.series_dir.mkdir()
        with self.assertRaisesRegex(SeriesError, 'start-series'):
            load_series(self.repo, self.series_dir)
        with self.assertRaises(FileExistsError):
            start_series(self.repo, self.series_dir)
        pin = self.series_dir / 'series.json'
        pin.write_text('{', encoding='utf-8')
        with self.assertRaisesRegex(SeriesError, 'malformed'):
            load_series(self.repo, self.series_dir)
        pin.write_text(json.dumps({
            'schema_version': 1,
            'upstream': {'remote': 'origin', 'branch': 'main', 'commit': '0' * len(self.base)},
            'start': {'head': self.base, 'ahead': 0, 'behind': 0},
        }), encoding='utf-8')
        with self.assertRaisesRegex(SeriesError, 'pinned commit'):
            load_series(self.repo, self.series_dir)


if __name__ == '__main__':
    unittest.main()
