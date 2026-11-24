"""Pin upstream once and record the source and runtime inputs for each diagnostic run."""

from __future__ import annotations

from contextlib import suppress
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import uuid
from typing import BinaryIO


class SeriesError(ValueError):
    """A diagnostic series could not be started or recorded."""


_SCHEMA = 1
_PIN_FILE = 'series.json'
_MANIFEST_FILE = 'manifest.json'
_PATCH_FILE = 'tracked.patch'
_OID = re.compile(r'(?:[0-9a-f]{40}|[0-9a-f]{64})')


def _git(repo_root: Path, action: str, *args: str, output: int | BinaryIO = subprocess.PIPE) -> bytes:
    env = os.environ | {
        'GIT_OPTIONAL_LOCKS': '0',  # A diff must not refresh the caller's index on disk.
        'GIT_NO_LAZY_FETCH': '1',
        'GIT_NO_REPLACE_OBJECTS': '1',
        'GIT_TERMINAL_PROMPT': '0',
    }
    try:
        result = subprocess.run(['git', '-C', str(repo_root), *args], stdin=subprocess.DEVNULL,
                                stdout=output, stderr=subprocess.DEVNULL, env=env, check=False)
    except FileNotFoundError:
        raise SeriesError('Git is required to record a diagnostic series') from None
    if result.returncode:
        # Never expose Git's remote diagnostics (they can include credential URLs).
        raise SeriesError(f'Git could not {action} (exit {result.returncode})')
    return result.stdout or b''


def _commit(repo_root: Path, ref: str, action: str) -> str:
    return _git(repo_root, action, 'rev-parse', '--verify', '--end-of-options', ref + '^{commit}').decode('ascii').strip()


def _head_distance(repo_root: Path, pinned: str) -> dict:
    head = _commit(repo_root, 'HEAD', 'resolve the checkout HEAD')
    counts = _git(repo_root, 'count commits against the pinned upstream', 'rev-list', '--left-right', '--count',
                  f'{head}...{pinned}')
    ahead, behind = map(int, counts.split())
    return {'head': head, 'ahead': ahead, 'behind': behind}


def _external(repo_root: Path, path: Path, purpose: str) -> Path:
    resolved = path.resolve()
    if resolved.is_relative_to(repo_root):
        raise SeriesError(f'{purpose} must be outside the checkout')
    return resolved


def _write_json(path: Path, value: dict) -> None:
    partial = path.with_name(path.name + '.partial')
    stream = partial.open('x', encoding='utf-8', newline='\n')
    try:
        with stream:
            json.dump(value, stream, indent=2, allow_nan=False)
            stream.write('\n')
        partial.replace(path)
    except BaseException:
        with suppress(OSError):
            partial.unlink()
        raise


def start_series(repo_root: Path, series_dir: Path, *, remote: str = 'origin', branch: str = 'main') -> dict:
    """Fetch one configured branch, preserving HEAD, the index and remote-tracking refs."""
    repo_root = repo_root.resolve()
    series_dir = _external(repo_root, series_dir, 'series directory')
    series_dir.mkdir(parents=True, exist_ok=False)
    ref = f'refs/nxnative/diagnostic-series/{uuid.uuid4().hex}'
    fetched = False
    try:
        remotes = _git(repo_root, 'list configured remotes', 'remote').splitlines()
        if os.fsencode(remote) not in remotes:
            raise SeriesError('upstream remote must name a configured Git remote')
        source = f'refs/heads/{branch}'
        _git(repo_root, 'validate the upstream branch name', 'check-ref-format', source)
        # Private refs keep old pins alive across origin movement and Git GC, without changing
        # the status of the user's branches or capturing a possibly racing FETCH_HEAD.
        _git(repo_root, 'fetch the configured upstream branch', 'fetch', '--no-tags', '--no-prune',
             '--no-prune-tags', '--no-recurse-submodules', '--no-write-fetch-head', '--no-auto-maintenance',
             '--no-write-commit-graph', '--refmap=', '--', remote, f'{source}:{ref}', output=subprocess.DEVNULL)
        fetched = True
        commit = _commit(repo_root, ref, 'resolve the fetched commit')
        series = {'schema_version': _SCHEMA, 'upstream': {'remote': remote, 'branch': branch, 'commit': commit},
                  'start': _head_distance(repo_root, commit)}
        _write_json(series_dir / _PIN_FILE, series)
    except BaseException:
        if fetched:
            with suppress(SeriesError, OSError):
                _git(repo_root, 'remove the incomplete pin', 'update-ref', '-d', ref, output=subprocess.DEVNULL)
        with suppress(OSError):
            series_dir.rmdir()
        raise
    return series


def load_series(repo_root: Path, series_dir: Path) -> dict:
    """Read and validate the pin using only local Git objects, not a moving upstream ref."""
    repo_root = repo_root.resolve()
    pin = _external(repo_root, series_dir, 'series directory') / _PIN_FILE
    try:
        data = json.loads(pin.read_text(encoding='utf-8'))
        upstream = data['upstream']
        remote, branch, commit = (upstream[key] for key in ('remote', 'branch', 'commit'))
        start = data['start']
        head, ahead, behind = (start[key] for key in ('head', 'ahead', 'behind'))
        if (type(data['schema_version']) is not int or data['schema_version'] != _SCHEMA or
                not all(isinstance(value, str) and value for value in (remote, branch, commit)) or
                not _OID.fullmatch(commit) or not isinstance(head, str) or not _OID.fullmatch(head) or
                any(type(count) is not int or count < 0 for count in (ahead, behind))):
            raise ValueError
    except FileNotFoundError:
        raise SeriesError('missing series.json; create a new series with start-series first') from None
    except (ValueError, KeyError, TypeError, RecursionError):
        raise SeriesError('malformed or unsupported diagnostic series pin') from None
    for ref in (f'refs/remotes/{remote}/pin', f'refs/heads/{branch}'):
        # Check provenance syntax without requiring the original remote to still exist.
        if '\0' in ref:
            raise SeriesError('malformed diagnostic series upstream')
        try:
            ref.encode('utf-8')
        except UnicodeEncodeError:
            raise SeriesError('malformed diagnostic series upstream') from None
        _git(repo_root, 'validate the pinned upstream name', 'check-ref-format', ref)
    if _commit(repo_root, commit, 'find the pinned commit in this checkout') != commit:
        raise SeriesError('diagnostic series pin does not name a commit')
    return {'schema_version': _SCHEMA, 'upstream': {'remote': remote, 'branch': branch, 'commit': commit},
            'start': {'head': head, 'ahead': ahead, 'behind': behind}}


def _open_regular_file(path: Path) -> BinaryIO:
    if not path.is_file():
        raise SeriesError(f'expected a regular file: {path}')
    return path.open('rb')


def _file_identity(path: Path) -> dict:
    with _open_regular_file(path) as stream:
        size = os.fstat(stream.fileno()).st_size
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return {'path': str(path), 'size_bytes': size, 'sha256': digest}


def _walk_error(error: OSError) -> None:
    raise error


def _shader_identity(library: Path) -> dict:
    if not library.is_dir():
        raise SeriesError(f'expected a shader library directory: {library}')
    digest = hashlib.sha256()
    count = 0
    for root, dirs, files in library.walk(on_error=_walk_error):
        dirs.sort()
        for name in dirs:
            if (root / name).is_junction():
                raise SeriesError(f'shader library must not contain directory junctions: {root / name}')
        for name in sorted(files):
            path = root / name
            if path.is_symlink():
                raise SeriesError(f'shader library must not contain symbolic links: {path}')
            # Each entry is a NUL-terminated relative path and a fixed-size content hash.
            digest.update(path.relative_to(library).as_posix().encode('utf-8', errors='surrogateescape'))
            digest.update(b'\0')
            with _open_regular_file(path) as stream:
                digest.update(hashlib.file_digest(stream, 'sha256').digest())
            count += 1
    return {'path': str(library), 'file_count': count, 'sha256': digest.hexdigest()}


def _untracked(repo_root: Path) -> list[dict]:
    paths = _git(repo_root, 'list untracked source', 'ls-files', '--others', '--exclude-standard', '-z')
    result = []
    for raw in sorted(paths.split(b'\0')):
        if not raw or raw.endswith(b'/'):
            continue  # An embedded repository is reported as a directory, not a source file.
        relative = os.fsdecode(raw)
        path = repo_root / relative
        if path.is_symlink():
            # Like Git, hash the link's payload instead of following it out of the checkout.
            link = os.fsencode(os.readlink(path))
            identity = {'size_bytes': len(link), 'sha256': hashlib.sha256(link).hexdigest(), 'type': 'symlink'}
        else:
            identity = _file_identity(path)
        identity['path'] = relative
        result.append(identity)
    return result


def _source_identity(repo_root: Path, pinned: str, patch: Path) -> dict:
    git = _head_distance(repo_root, pinned)
    base = _git(repo_root, 'find a merge base for HEAD and the pinned upstream', 'merge-base', git['head'], pinned)
    merge_base = base.decode('ascii').strip()
    stream = patch.open('xb')
    try:
        with stream:
            _git(repo_root, 'write the tracked working-tree patch', 'diff', '--binary', '--full-index',
                 '--no-ext-diff', '--no-textconv', '--no-color', '--src-prefix=a/', '--dst-prefix=b/',
                 '--ignore-submodules=none', '--submodule=short', merge_base, '--', output=stream)
    except BaseException:
        with suppress(OSError):
            patch.unlink()
        raise
    patch_identity = _file_identity(patch)
    patch_identity['path'] = patch.name
    return {**git, 'merge_base': merge_base, 'tracked_patch': patch_identity, 'untracked': _untracked(repo_root)}


def write_run_manifest(repo_root: Path, series: dict, run_dir: Path, *, host: Path, pdb: Path | None,
                       shader_library: Path, original_save: Path, backup_save: Path, profile: Path,
                       rewrite: Path, romfs: Path, capture_settings: dict) -> dict:
    """Record inputs after the save backup is made, and before the player can modify it."""
    repo_root = repo_root.resolve()
    run_dir = _external(repo_root, run_dir, 'run directory')
    shader_library = _external(repo_root, shader_library, 'shader library')
    original_save = _external(repo_root, original_save, 'original save')
    backup_save = _external(repo_root, backup_save, 'backup save')
    rewrite = _external(repo_root, rewrite, 'rewrite directory')
    romfs = _external(repo_root, romfs, 'ROMFS')
    if repo_root.is_relative_to(shader_library):
        raise SeriesError('shader library must not contain the checkout')
    if run_dir.is_relative_to(shader_library):
        raise SeriesError('run directory must not be inside the shader library')
    if not rewrite.is_dir() or not romfs.is_file():
        raise SeriesError('expected a rewrite directory and a ROMFS image file')
    if (run_dir / _MANIFEST_FILE).exists():
        raise SeriesError('run manifest already exists')
    pinned = series['upstream']['commit']
    git = _source_identity(repo_root, pinned, run_dir / _PATCH_FILE)
    romfs_stat = romfs.stat()
    runtime = {
        'host': _file_identity(host.resolve()),
        'pdb': _file_identity(pdb.resolve()) if pdb is not None else None,
        'shader_library': _shader_identity(shader_library),
        'original_save': _file_identity(original_save),
        'backup_save': _file_identity(backup_save),
        'profile': _file_identity(profile.resolve()),
        'rewrite': {'path': str(rewrite)},
        # The image can be several GiB: its path and metadata suffice for the launch record.
        'romfs': {'path': str(romfs), 'size_bytes': romfs_stat.st_size, 'mtime_ns': romfs_stat.st_mtime_ns},
    }
    manifest = {'schema_version': _SCHEMA, 'series': series, 'git': git, 'runtime': runtime,
                'capture_settings': capture_settings}
    _write_json(run_dir / _MANIFEST_FILE, manifest)
    return manifest
