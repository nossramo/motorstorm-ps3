#!/usr/bin/env python3
"""Build the shared native engine and export CMake's exact link dependencies.

Xcode compiles the app entry point and links the engine in-process. CMake owns
the generated game source, its per-file arithmetic flags, and SDK dependencies.
The File API avoids a second handwritten list of Homebrew/framework libraries.
"""
import fcntl
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

from launch_profile import environment


def write_changed(path, contents):
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != contents:
        path.write_text(contents)


def main():
    root = Path(__file__).resolve().parents[1]
    if os.environ.get('PLATFORM_NAME') != 'macosx':
        sys.exit('error: This runtime integration currently supports macOS only.')
    for required in ('vendor/ps3recomp/CMakeLists.txt', 'port/recompiled/ppu_recomp.h',
                     'game/PS3_GAME/USRDIR/EBOOT.elf', 'port/spu/spu_workloads.c'):
        if not (root / required).is_file():
            sys.exit(f'error: Missing {required}. Restore the local runtime/game and generate sources as described in BACKUP.md.')
    build = root / 'build/game'
    build.mkdir(parents=True, exist_ok=True)
    with (build / '.xcode-build.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        query = build / '.cmake/api/v1/query/client-motorstorm/codemodel-v2'
        query.parent.mkdir(parents=True, exist_ok=True)
        query.touch()
        subprocess.run(['sh', 'tools/build_port.sh'], cwd=root, check=True)
        reply = build / '.cmake/api/v1/reply'
        index = json.loads(max(reply.glob('index-*.json'), key=lambda p: p.stat().st_mtime).read_text())
        model_ref = index['reply']['client-motorstorm']['codemodel-v2']['jsonFile']
        model = json.loads((reply / model_ref).read_text())
        target_ref = next(t for t in model['configurations'][0]['targets'] if t['name'] == 'motorstorm_runner')
        target = json.loads((reply / target_ref['jsonFile']).read_text())
        args = []
        for fragment in target['link']['commandFragments']:
            if fragment['role'] not in ('libraries', 'libraryPath', 'frameworkPath'):
                continue
            for arg in shlex.split(fragment['fragment']):
                if not arg.startswith('-') and arg.endswith(('.a', '.dylib')) and not Path(arg).is_absolute():
                    arg = str(build / arg)
                args.append(arg)
        if not any(arg.endswith('/libMotorStormRecomp.a') for arg in args):
            sys.exit('error: CMake did not export the native game archive.')
        # Clang response-file quoting (paths may contain spaces).
        response = '\n'.join('"' + arg.replace('\\', '\\\\').replace('"', '\\"') + '"' for arg in args) + '\n'
        write_changed(build / 'xcode-link.rsp', response)
        resources = Path(os.environ['TARGET_BUILD_DIR']) / os.environ['UNLOCALIZED_RESOURCES_FOLDER_PATH']
        config = {'root': str(root), 'environment': environment(root)}
        write_changed(resources / 'MotorStormLaunch.json', json.dumps(config, indent=2, ensure_ascii=False) + '\n')
        print('Native runtime ready for the macOS app; game data stays in the project folder.')


if __name__ == '__main__':
    main()
