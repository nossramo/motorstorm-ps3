#!/usr/bin/env python3
"""Replay complete private WWS batches using the retained game as the oracle."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import specialize_native_geometry as geometry

ROOT = geometry.ROOT
STOP = {0x8BE0, 0x8F08}


def closure(functions):
    seen, todo = set(), [0x88D8]
    while todo:
        pc = todo.pop()
        if pc in seen or pc in STOP:
            continue
        seen.add(pc)
        body = functions[pc][3]
        for m in re.finditer(r'(?:g_spu_trampoline_fn = |host_depth\+\+; )'
                             r'\w+_spu_func_([0-9A-F]+)', body):
            todo.append(int(m[1], 16))
        if any(x in body for x in ('spu_wrch(', 'spu_rdch(', 'spu_indirect_branch', 'spu_stop(')):
            raise ValueError(f'Unmodelled external effect at {pc:x}')
    return seen


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--thinlto', action='store_true')
    p.add_argument('--play-build', action='store_true', help='Compile out passive diagnostics as in normal iOS builds')
    p.add_argument('--production-baseline', type=Path,
                   help='Generated iOS native_geometry source, before WWS adapter')
    args = p.parse_args()
    source = geometry.checked_source()
    from specialize_wws_chain import audit_callers
    audit_callers(source)
    functions = geometry.functions(source)
    pcs = closure(functions)
    args.output.mkdir(parents=True, exist_ok=True)
    code = f'#include "{geometry.SOURCE.with_suffix(".h")}"\n'
    epilogue = functions[0x8BE0][0]
    code += epilogue.replace(functions[0x8BE0][1], 'wws_epilogue') + '\n'
    if args.production_baseline:
        source = args.production_baseline.read_text()
        functions = geometry.functions(source)
        # All static helpers are retained exactly as compiled in the app.
        # Unreachable guest entry points fail closed if unexpectedly reached.
        first = geometry.START.search(source).start()
        prefix = source[:first].replace('#include "spu_recomp.h"', '')
        prefix = prefix.replace('merged_8BE0: {', 'merged_8BE0: { replay_stop(ctx, 0x8BE0);')
        code += prefix
        for match in re.finditer(r'^static [^\n]*\bms_\w+\(', source[first:], re.M):
            start = first + match.start()
            end = geometry.definition_end(source, source.index('{', start))
            code += source[start:end] + '\n'
        for pc, value in functions.items():
            if pc not in pcs and pc not in STOP:
                code += f'\nvoid {value[1]}(spu_context* ctx) {{ (void)ctx; abort(); }}\n'
    code += '\n'.join(functions[pc][0] for pc in sorted(pcs))
    for pc in sorted(STOP):
        finish = 'wws_epilogue(ctx);' if pc == 0x8BE0 else ''
        code += f'\nvoid {geometry.MODULE}_spu_func_{pc:08X}(spu_context* ctx) {{ replay_stop(ctx, 0x{pc:X}); {finish} }}\n'
    (args.output / 'wws_reference.inc').write_text(code)
    cmd = ['clang', '-O2', '-std=gnu17', '-UNDEBUG', '-fno-strict-aliasing', '-fwrapv',
           '-ffp-contract=off', '-DSPU_LS_NEON', '-DSPU_LS_DIAGNOSTICS=0',
           '-DMOTORSTORM_WWS_CHAIN_TEST', '-DMOTORSTORM_SPU_THREAD_RTZ',
           '-include', ROOT/'port/spu_thread_rtz.h', '-include', ROOT/'port/spu_shufb_neon.h',
           '-I'+str(args.output.resolve()), '-I'+str(ROOT/'port'),
           '-I'+str(ROOT/'vendor/ps3recomp/runtime/spu'), '-I'+str(ROOT/'vendor/ps3recomp/include'),
           ROOT/'port/tests/test_wws_chain.c', ROOT/'port/wws_native_chain.c',
           ROOT/'port/spu_thread_rtz.c', '-o', args.output/'replay']
    if args.sanitize:
        cmd[1:1] = ['-g', '-fsanitize=address,undefined']
    if args.thinlto:
        cmd[1:1] = ['-flto=thin']
    if args.play_build:
        cmd[1:1] = ['-DPS3_PASSIVE_METRICS=0']
    os.environ.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
    subprocess.run(list(map(str, cmd)), check=True)
    env = dict(os.environ, MOTORSTORM_JOB_MERGE='1', MOTORSTORM_SPU_PIPELINE='2',
               MOTORSTORM_JOB_SCOPES='0xf1', MOTORSTORM_NATIVE_GEOMETRY='1',
               MOTORSTORM_CULL_FAST_BOUNDS='1', MOTORSTORM_WWS_CHAIN='1')
    subprocess.run([str((args.output/'replay').resolve()), str(args.capture.resolve())], check=True, env=env)


if __name__ == '__main__':
    main()
