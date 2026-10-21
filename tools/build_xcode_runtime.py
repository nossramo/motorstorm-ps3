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
    platform = os.environ.get('PLATFORM_NAME')
    if platform not in ('macosx', 'iphoneos', 'iphonesimulator'):
        sys.exit('error: Unsupported Apple build destination.')
    if platform == 'macosx':
        # Older project files may still invoke this helper. Never export the
        # live development tree's stale build/game archive for macOS.
        subprocess.run([sys.executable, str(root / 'tools/build_macos_runtime.py')], check=True)
        return
    ios = platform != 'macosx'
    # iPhone caller samples: historical full trace + current lock-candidate
    # fragment. Keep macOS's five-unit/FMA-only configuration independent.
    # The component test is positive, but hot-phone gameplay is inconclusive.
    # Require an explicit comparison build until matched gameplay validates it.
    ios_vmx = os.environ.get('MOTORSTORM_BUILD_IOS_VMX') == '1'
    vmx_units = ('001;002;034;035;036;042;051;055;057;058;060;064;065;068;069;070;074;075;076;081;082;092'
                 if ios_vmx else '002;057;075;076;082')
    for required in ('vendor/ps3recomp/CMakeLists.txt', 'port/recompiled/ppu_recomp.h',
                     'game/PS3_GAME/USRDIR/EBOOT.elf', 'port/spu/spu_workloads.c'):
        if not (root / required).is_file():
            sys.exit(f'error: Missing {required}. Restore the local runtime/game and generate sources as described in BACKUP.md.')
    build = root / ('build/ios-game/' + platform if ios else 'build/game')
    build.mkdir(parents=True, exist_ok=True)
    with (build / '.xcode-build.lock').open('w') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        query = build / '.cmake/api/v1/query/client-motorstorm/codemodel-v2'
        query.parent.mkdir(parents=True, exist_ok=True)
        query.touch()
        if ios:
            deps = root / 'build/ios-deps' / platform
            prefix = deps / 'install'
            manifest = json.loads((root / 'port/ios/dependencies.json').read_text())
            ready = deps / 'ready.json'
            if not ready.exists() or json.loads(ready.read_text()).get('dependencies') != manifest:
                sys.exit(f'error: First run python3 tools/build_ios_dependencies.py --sdk {platform}')
            sdk = subprocess.check_output(['xcrun', '--sdk', platform, '--show-sdk-path'], text=True).strip()
            subprocess.run(['cmake', '-S', str(root / 'port'), '-B', str(build), '-G', 'Ninja',
                '-DCMAKE_SYSTEM_NAME=iOS', '-DCMAKE_SYSTEM_PROCESSOR=arm64',
                f'-DCMAKE_OSX_SYSROOT={sdk}', '-DCMAKE_OSX_ARCHITECTURES=arm64',
                '-DCMAKE_OSX_DEPLOYMENT_TARGET=18.0', '-DCMAKE_BUILD_TYPE=Release',
                f'-DCMAKE_PREFIX_PATH={prefix}', f'-DCMAKE_FIND_ROOT_PATH={prefix}',
                '-DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=ONLY', '-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY',
                '-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY', '-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER',
                '-DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON',
                f'-DSDL2_DIR={prefix}/lib/cmake/SDL2', f'-Dglslang_DIR={prefix}/lib/cmake/glslang',
                f'-DSPVC_INC_DIR={prefix}/include/spirv_cross', f'-DSPVC_LIB={prefix}/lib/libspirv-cross-c.a',
                f'-DPS3RECOMP_DIR={root}/vendor/ps3recomp', '-DPS3RECOMP_FFMPEG_RUNTIME=ON',
                f'-DPS3RECOMP_FFMPEG_INCLUDE_DIR={prefix}/include',
                '-DMOTORSTORM_CULL_SCOPE=ON', '-DMOTORSTORM_NEON_VMX=ON',
                f'-DMOTORSTORM_NEON_VMX_UNITS={vmx_units}',
                '-DMOTORSTORM_NEON_ARITHMETIC=' + ('ON' if ios_vmx else 'OFF'),
                '-DMOTORSTORM_JOB_ARITHMETIC=ON',
                '-DMOTORSTORM_VMX_BENCHMARK=' + ('ON' if os.environ.get(
                    'MOTORSTORM_BUILD_VMX_BENCHMARK') == '1' else 'OFF'),
                '-DMOTORSTORM_AUDIO_BENCHMARK=' + ('ON' if os.environ.get(
                    'MOTORSTORM_BUILD_AUDIO_BENCHMARK') == '1' else 'OFF'),
                '-DMOTORSTORM_JOB_BENCHMARK=' + ('ON' if os.environ.get(
                    'MOTORSTORM_BUILD_JOB_BENCHMARK') == '1' else 'OFF')], check=True)
            subprocess.run(['cmake', '--build', str(build), '--target', 'MotorStormRecomp',
                '-j', os.environ.get('BUILD_JOBS', '4')], check=True)
        else:
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
        if ios:
            values = environment(root)
            # iPhone-specific scheduling policy; keep the validated macOS
            # defaults independent. These are also recorded in offline logs.
            values.update(json.loads((root / 'port/ios/runtime-options.json').read_text()))
            for key in ('PS3RECOMP_FFMPEG_AVCODEC', 'PS3RECOMP_FFMPEG_AVUTIL', 'PS3RECOMP_MSL_CACHE'):
                values.pop(key, None)
            values['PS3_TITLE'] = 'MotorStorm'
            game = root / 'game'
            files = {p.relative_to(game).as_posix(): p.stat().st_size
                     for p in sorted(game.rglob('*')) if p.is_file()
                     and not any(part.startswith('.') for part in p.relative_to(game).parts)}
            config = {'environment': values, 'files': files,
                      'build_options': {'ppu_vmx_units': vmx_units.split(';'),
                                        'ppu_vmx_arithmetic': ios_vmx}}
        else:
            config = {'root': str(root), 'environment': environment(root)}
        write_changed(resources / 'MotorStormLaunch.json', json.dumps(config, indent=2, ensure_ascii=False) + '\n')
        print(f'Native runtime ready for {platform}; game data is not bundled into the app.')


if __name__ == '__main__':
    main()
