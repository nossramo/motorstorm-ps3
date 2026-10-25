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
    # Separate object/cache tree keeps the known non-LTO archive available for
    # comparison. =0 selects it without rebuilding every generated game unit.
    thin_lto = os.environ.get('MOTORSTORM_BUILD_IOS_THINLTO') != '0'
    # iPhone caller samples: historical full trace + current lock-candidate
    # fragment. Bit-exact in the device lane test; =0 selects the previous
    # five-unit/FMA-only configuration for comparison.
    ios_vmx = os.environ.get('MOTORSTORM_BUILD_IOS_VMX') != '0'
    inline_stores = os.environ.get('MOTORSTORM_BUILD_IOS_INLINE_STORES') != '0'
    # Measurement only: approximate SPU floats, switched at run time.
    float_probe = os.environ.get('MOTORSTORM_BUILD_IOS_SPU_FLOAT_PROBE') == '1'
    # Exact native RTZ SPU floats for the duration of each SPU run.
    thread_rtz = os.environ.get('MOTORSTORM_BUILD_IOS_SPU_THREAD_RTZ') != '0'
    # Byte-identical NEON local-store accessors. iOS only.
    ls_neon = os.environ.get('MOTORSTORM_BUILD_IOS_SPU_LS_NEON') != '0'
    # Exact table-based SHUFB; =0 rebuilds the previous helper for comparison.
    shufb_neon = os.environ.get('MOTORSTORM_BUILD_IOS_SPU_SHUFB_NEON') != '0'
    detailed_diagnostics = os.environ.get('MOTORSTORM_BUILD_IOS_DETAILED_DIAGNOSTICS') == '1'
    # Isolate renderer wall-time accounting from the costly SPU local-store
    # observers and job-chain sampling in the full diagnostic build.
    render_diagnostics = (detailed_diagnostics or
                          os.environ.get('MOTORSTORM_BUILD_IOS_RENDER_DIAGNOSTICS') == '1')
    # Legacy observer hooks otherwise add loads/branches to every SPU LS128
    # access even with all run-time probes disabled. Keep an isolated A/B flag.
    ls_diagnostics = (detailed_diagnostics or
                      os.environ.get('MOTORSTORM_BUILD_IOS_SPU_LS_DIAGNOSTICS') == '1')
    # Bounded WWS block fusion; runtime MOTORSTORM_JOB_MERGE=0 retains the
    # previous bodies in the same binary for controlled device comparisons.
    job_merge = os.environ.get('MOTORSTORM_BUILD_IOS_JOB_MERGE') != '0'
    # One bit-exact native audio recurrence; =0 compiles the original overlay.
    # The candidate also retains its original loop behind a runtime switch.
    audio_filter = os.environ.get('MOTORSTORM_BUILD_IOS_AUDIO_FILTER') != '0'
    # Four sampled PPU functions; originals remain selectable at runtime.
    ppu_locals = os.environ.get('MOTORSTORM_BUILD_IOS_PPU_LOCALS') != '0'
    native_geometry = os.environ.get('MOTORSTORM_BUILD_IOS_NATIVE_GEOMETRY') != '0'
    # Units holding ~90% of out-of-line store/coherence-check time in the
    # iPhone gameplay trace (logs/ios-vmx-01, main and physics threads).
    store_units = ('083;065;091;082;092;063;075;089;064;060;070;000;055;002;076;056;081;062;069;068'
                   if inline_stores else '')
    vmx_units = ('001;002;034;035;036;042;051;055;057;058;060;064;065;068;069;070;074;075;076;081;082;092'
                 if ios_vmx else '002;057;075;076;082')
    for required in ('vendor/ps3recomp/CMakeLists.txt', 'port/recompiled/ppu_recomp.h',
                     'game/PS3_GAME/USRDIR/EBOOT.elf', 'port/spu/spu_workloads.c'):
        if not (root / required).is_file():
            sys.exit(f'error: Missing {required}. Restore the local runtime/game and generate sources as described in BACKUP.md.')
    platform_build = root / ('build/ios-game/' + platform if ios else 'build/game')
    build = platform_build / 'thinlto' if thin_lto else platform_build
    build.mkdir(parents=True, exist_ok=True)
    with (platform_build / '.xcode-build.lock').open('w') as lock:
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
                '-DMOTORSTORM_IOS_THINLTO=' + ('ON' if thin_lto else 'OFF'),
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
                '-DMOTORSTORM_JOB_MERGE=' + ('ON' if job_merge else 'OFF'),
                '-DMOTORSTORM_AUDIO_FILTER_NATIVE=' + ('ON' if audio_filter else 'OFF'),
                '-DMOTORSTORM_PPU_LOCALS=' + ('ON' if ppu_locals else 'OFF'),
                '-DMOTORSTORM_NATIVE_GEOMETRY=' + ('ON' if native_geometry else 'OFF'),
                '-DMOTORSTORM_INLINE_STORES=' + ('ON' if inline_stores else 'OFF'),
                f'-DMOTORSTORM_INLINE_STORE_UNITS={store_units}',
                '-DMOTORSTORM_SPU_FLOAT_PROBE=' + ('ON' if float_probe else 'OFF'),
                '-DMOTORSTORM_SPU_THREAD_RTZ=' + ('ON' if thread_rtz else 'OFF'),
                '-DMOTORSTORM_SPU_LS_NEON=' + ('ON' if ls_neon else 'OFF'),
                '-DMOTORSTORM_SPU_LS_DIAGNOSTICS=' + ('ON' if ls_diagnostics else 'OFF'),
                '-DMOTORSTORM_SPU_SHUFB_NEON=' + ('ON' if shufb_neon else 'OFF'),
                '-DMOTORSTORM_RSX_PHASE_ACCOUNT=' + ('ON' if render_diagnostics else 'OFF'),
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
        if thin_lto:
            # The File API loop above exports libraries only. Xcode performs
            # the real link, so pass ThinLTO there too, and override its Debug
            # -O0/Release -Os for the LTO backend without changing app sources.
            cache = build / 'thinlto-cache'
            cache.mkdir(exist_ok=True)
            args += ['-flto=thin', '-O2', '-Xlinker', '-cache_path_lto',
                     '-Xlinker', str(cache)]
        # Clang response-file quoting (paths may contain spaces).
        response = '\n'.join('"' + arg.replace('\\', '\\\\').replace('"', '\\"') + '"' for arg in args) + '\n'
        write_changed(platform_build / 'xcode-link.rsp', response)
        resources = Path(os.environ['TARGET_BUILD_DIR']) / os.environ['UNLOCALIZED_RESOURCES_FOLDER_PATH']
        if ios:
            values = environment(root)
            # iPhone-specific scheduling policy; keep the validated macOS
            # defaults independent. These are also recorded in offline logs.
            values.update(json.loads((root / 'port/ios/runtime-options.json').read_text()))
            if detailed_diagnostics:
                values['MOTORSTORM_JOB_CHAIN'] = '1'
            for key in ('PS3RECOMP_FFMPEG_AVCODEC', 'PS3RECOMP_FFMPEG_AVUTIL', 'PS3RECOMP_MSL_CACHE'):
                values.pop(key, None)
            values['PS3_TITLE'] = 'MotorStorm'
            game = root / 'game'
            files = {p.relative_to(game).as_posix(): p.stat().st_size
                     for p in sorted(game.rglob('*')) if p.is_file()
                     and not any(part.startswith('.') for part in p.relative_to(game).parts)}
            config = {'environment': values, 'files': files,
                      'build_options': {'thin_lto': thin_lto,
                                        'ppu_vmx_units': vmx_units.split(';'),
                                        'ppu_vmx_arithmetic': ios_vmx,
                                        'ppu_inline_store_units': store_units.split(';') if store_units else [],
                                        'spu_float_probe': float_probe,
                                        'spu_thread_rtz': thread_rtz,
                                        'spu_ls_neon': ls_neon,
                                        'spu_ls_diagnostics': ls_diagnostics,
                                        'spu_shufb_neon': shufb_neon,
                                        'detailed_diagnostics': detailed_diagnostics,
                                        'render_phase_diagnostics': render_diagnostics,
                                        'spu_job_merge': job_merge,
                                        'audio_filter_native': audio_filter}}
            config['build_options']['ppu_locals'] = ppu_locals
            config['build_options']['native_geometry'] = native_geometry
        else:
            config = {'root': str(root), 'environment': environment(root)}
        write_changed(resources / 'MotorStormLaunch.json', json.dumps(config, indent=2, ensure_ascii=False) + '\n')
        print(f'Native runtime ready for {platform}; game data is not bundled into the app.')


if __name__ == '__main__':
    main()
