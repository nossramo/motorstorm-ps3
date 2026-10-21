#!/usr/bin/env python3
"""Build macOS from its pinned source snapshot, independent of iOS experiments.

Only local Git objects and the user's game executable are read. Runtime source,
generators, generated PPU/SPU code and object files all have separate copies.
The game assets and saves are not copied. No live source is reset or patched.
"""
import argparse
import fcntl
import hashlib
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / 'build/macos-stable'
SOURCE = CACHE / 'source'


def baseline_commit(root=ROOT):
    commit = json.loads((root / 'port/macos-baseline.json').read_text())['commit']
    if not re.fullmatch('[0-9a-f]{40}', commit):
        raise RuntimeError('macOS baseline must be a full commit ID')
    return commit


def macos_environment(root=ROOT):
    data = subprocess.check_output(['git', '-C', str(root), 'show',
                                    baseline_commit(root) + ':port/macos-launch.json'])
    return {key: value.replace('${ROOT}', str(root)) for key, value in json.loads(data).items()}


def run(*args, cwd=ROOT, **kwargs):
    return subprocess.run(args, cwd=cwd, check=True, **kwargs)


def archive(repo, revision, destination, *paths):
    data = subprocess.check_output(['git', '-C', str(repo), 'archive', revision, *paths])
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        # These are our pinned source objects, not an external uploaded archive.
        # Still reject links/traversal: source snapshots must be independent.
        for item in tar.getmembers():
            if (item.name.startswith('/') or '..' in Path(item.name).parts
                    or not (item.isfile() or item.isdir())):
                raise RuntimeError(f'Unsafe source archive member: {item.name}')
        tar.extractall(destination, filter='data')


def source_hashes(source):
    result = {}
    for directory in ('port', 'tools', 'vendor', 'patches', 'research'):
        for path in sorted((source / directory).rglob('*')):
            if '.git' in path.parts or '__pycache__' in path.parts:
                continue
            if path.is_symlink():
                raise RuntimeError(f'Source must not link to the iOS working tree: {path}')
            if path.is_file():
                result[path.relative_to(source).as_posix()] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def verify(source, commit):
    manifest = json.loads((source / 'macos-source.json').read_text())
    if manifest['commit'] != commit or manifest['files'] != source_hashes(source):
        raise RuntimeError('Pinned macOS source changed. Left untouched; inspect build/macos-stable/source before rebuilding.')
    return manifest


def prepare(commit):
    if SOURCE.exists():
        verify(SOURCE, commit)
        return
    staging = CACHE / 'preparing'
    if staging.exists():
        raise RuntimeError('An incomplete macOS preparation exists at build/macos-stable/preparing; inspect it before retrying.')
    archive(ROOT, commit, staging, 'port', 'tools', 'patches/ps3recomp-local.patch',
            'research/ps3recomp-commit.txt')
    sdk_commit = (staging / 'research/ps3recomp-commit.txt').read_text().strip()
    if not re.fullmatch('[0-9a-f]{40}', sdk_commit):
        raise RuntimeError('Invalid runtime revision in macOS snapshot')
    sdk = staging / 'vendor/ps3recomp'
    archive(ROOT / 'vendor/ps3recomp', sdk_commit, sdk)
    run('git', 'init', '--quiet', cwd=sdk)
    run('git', 'apply', '--check', str(staging / 'patches/ps3recomp-local.patch'), cwd=sdk)
    run('git', 'apply', str(staging / 'patches/ps3recomp-local.patch'), cwd=sdk)
    # Re-lift with the pinned generators. Reusing port/recompiled or port/spu
    # would carry iOS-only hooks/registration into the otherwise frozen build.
    (staging / 'game').symlink_to(ROOT / 'game', target_is_directory=True)
    run('sh', 'tools/analyze_game.sh', cwd=staging)
    run(sys.executable, 'tools/lift_motorstorm_audio.py', cwd=staging)
    manifest = {'commit': commit, 'runtime_commit': sdk_commit, 'files': source_hashes(staging)}
    (staging / 'macos-source.json').write_text(json.dumps(manifest, indent=2) + '\n')
    staging.rename(SOURCE)


def configure_macos_options(env):
    # Keep the pinned builder's settings, with one explicit macOS-only CMake
    # include. Configure it before invoking that builder; the cache retains the
    # include on its second configure and on subsequent Xcode/Terminal builds.
    # No source snapshot edits or shared/iOS CMake changes are needed.
    run('cmake', '-S', str(SOURCE / 'port'), '-B', str(SOURCE / 'build/game'), '-G', 'Ninja',
        f'-DPS3RECOMP_DIR={SOURCE}/vendor/ps3recomp', '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_OSX_ARCHITECTURES=arm64', '-DPS3RECOMP_FFMPEG_RUNTIME=ON',
        '-DMOTORSTORM_CULL_SCOPE=ON', '-DMOTORSTORM_NEON_VMX=ON',
        f'-DCMAKE_PROJECT_MotorStormRecomp_INCLUDE={ROOT}/tools/macos_runtime_options.cmake',
        env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-only', action='store_true', help='Build the standalone macOS runner')
    parser.add_argument('--prepare-only', action='store_true', help='Restore and verify isolated sources')
    parser.add_argument('--check', action='store_true', help='Verify isolated source checksums without building')
    args = parser.parse_args()
    commit = baseline_commit()
    CACHE.mkdir(parents=True, exist_ok=True)
    with (CACHE / '.build.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if args.check:
            manifest = verify(SOURCE, commit)
            print(f'macOS {commit}: {len(manifest["files"])} independent source files verified.')
            return
        prepare(commit)
        if args.prepare_only:
            return
        print(f'Building isolated macOS baseline {commit}', flush=True)
        env = dict(os.environ)
        env.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
        configure_macos_options(env)
        if args.build_only:
            run('sh', 'tools/build_port.sh', cwd=SOURCE, env=env)
            return
        if env.get('PLATFORM_NAME') != 'macosx':
            raise RuntimeError('This build phase is for macOS only; iOS uses build_xcode_runtime.py')
        # Use the original builder too, so flags and library selection cannot
        # silently inherit changes made to the iOS build helper.
        run(sys.executable, 'tools/build_xcode_runtime.py', cwd=SOURCE, env=env)
        response = SOURCE / 'build/game/xcode-link.rsp'
        (CACHE / 'xcode-link.rsp').write_bytes(response.read_bytes())
        values = macos_environment()
        resources = Path(env['TARGET_BUILD_DIR']) / env['UNLOCALIZED_RESOURCES_FOLDER_PATH']
        config = {'root': str(ROOT), 'environment': values, 'source_commit': commit}
        (resources / 'MotorStormLaunch.json').write_text(json.dumps(config, indent=2, ensure_ascii=False) + '\n')
        print('macOS uses its pinned runtime; the iOS working tree is unchanged.')


if __name__ == '__main__':
    main()
