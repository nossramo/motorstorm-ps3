#!/usr/bin/env python3
"""Run existing CPU/registration correctness tests with actual ThinLTO links.

Runs on the Apple Silicon host, not the phone. It verifies compiler-sensitive
semantics; it does not measure iOS gameplay correctness or frame rate.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    env = os.environ.copy()
    env.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
    tests = (
        ('python3', 'tools/test_native_job_startup.py'),
        ('sh', 'tools/test_dispatch_checks.sh'),
        ('sh', 'tools/test_spu_thread_rtz.sh'),
        ('sh', 'tools/test_ppu_store_inline.sh'),
        ('sh', 'tools/test_spu_shufb_neon.sh'),
        ('python3', 'tools/test_job_merge.py'),
    )
    with tempfile.TemporaryDirectory(prefix='motorstorm-thinlto-tests-') as folder:
        work = Path(folder)
        # Existing test scripts invoke clang by name. A temporary wrapper adds
        # ThinLTO to both compile and link without changing their reference
        # inputs, expected values, arithmetic flags or the installed toolchain.
        sdk = subprocess.check_output(
            ['xcrun', '--sdk', 'macosx', '--show-sdk-path'], env=env, text=True).strip()
        for name in ('clang', 'clang++'):
            compiler = subprocess.check_output(
                ['xcrun', '--find', name], env=env, text=True).strip()
            wrapper = work / name
            wrapper.write_text('#!/bin/sh\nexec ' + shlex.quote(compiler) +
                               ' -isysroot ' + shlex.quote(sdk) +
                               ' -flto=thin "$@"\n')
            wrapper.chmod(0o755)
        env['PATH'] = str(work) + os.pathsep + env['PATH']
        for command in tests:
            print('ThinLTO:', command[-1], flush=True)
            subprocess.run(command, cwd=root, env=env, check=True)
        # Keep the armed-probe regression too, without the non-LTO script's
        # assembly check (clang -S -flto emits IR, not final ARM64 assembly).
        binary = work / 'ls'
        subprocess.run([
            'clang', '-O2', '-std=gnu17', '-ffp-contract=off',
            '-DSPU_LS_NEON', '-DSPU_LS_DIAGNOSTICS=0',
            '-Ivendor/ps3recomp/runtime/spu', '-Ivendor/ps3recomp/include',
            'port/tests/test_spu_ls_neon.c', '-o', str(binary)
        ], cwd=root, env=env, check=True)
        subprocess.run([str(binary)], cwd=root, env=env, check=True)
    print('PASS ThinLTO host correctness; phone FPS remains unmeasured.')


if __name__ == '__main__':
    main()
