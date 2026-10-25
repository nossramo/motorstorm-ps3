#!/usr/bin/env python3
"""Compare actual semaphore bodies and exercise Darwin wake/timeout races."""
import os
from pathlib import Path
import subprocess
import tempfile
import specialize_counting_wait as wait

root = wait.ROOT
os.environ.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
with tempfile.TemporaryDirectory(prefix='motorstorm-counting-wait-') as folder:
    work = Path(folder)
    source = wait.SOURCE.read_text()
    original = wait.bodies(source)
    generated = wait.generate(source)
    # Use the lifter's actual rotate helper as well as the audited bodies.
    pieces = [source[:source.index('void func_')]]
    for pc, body in original.items():
        start = generated.index('void func_' + pc + '(')
        end = generated.index('\n}', start) + 2
        pieces += [body.replace('func_' + pc, 'reference_' + pc), generated[start:end]]
    (work / 'counting_wait.inc').write_text('\n'.join(pieces))
    subprocess.run(['clang++', '-O3', '-std=c++20', '-UNDEBUG', '-fno-strict-aliasing',
        '-fwrapv', '-DMOTORSTORM_COUNTING_WAIT_TEST', '-mmacosx-version-min=14.4',
        '-I' + str(work), '-I' + str(root / 'port'), '-I' + str(root / 'port/recompiled'),
        '-I' + str(root / 'vendor/ps3recomp/runtime/ppu'),
        '-I' + str(root / 'vendor/ps3recomp/include'), str(root / 'port/tests/test_counting_wait.cpp'),
        '-o', str(work / 'test')], check=True)
    subprocess.run([str(work / 'test')], check=True, timeout=30)
