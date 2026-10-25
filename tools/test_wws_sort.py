#!/usr/bin/env python3
"""Differential replay of the complete WWS occluder sort, including tie order."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import specialize_native_geometry as geometry
from specialize_wws_chain import audit_sort

ROOT = geometry.ROOT
STOP = {0xFAE0, 0xF020}


def closure(functions, stop=STOP):
    seen, todo = set(), [0xDCBC]
    while todo:
        pc = todo.pop()
        if pc in seen or pc in stop:
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
    p.add_argument('--whole-phase', action='store_true')
    p.add_argument('--production-baseline', type=Path)
    args = p.parse_args()
    source = geometry.checked_source()
    audit_sort(source)
    functions = geometry.functions(source)
    stop = {0xF530} if args.whole_phase else STOP
    pcs = closure(functions, stop)
    args.output.mkdir(parents=True, exist_ok=True)
    code = f'#include "{geometry.SOURCE.with_suffix(".h")}"\n'
    if args.whole_phase:
        code += functions[0xF530][0].replace(functions[0xF530][1], 'wws_sort_epilogue') + '\n'
    if args.production_baseline:
        source = args.production_baseline.read_text()
        functions = geometry.functions(source)
        first = geometry.START.search(source).start()
        prefix = source[:first].replace('#include "spu_recomp.h"', '')
        for pc in stop:
            prefix = prefix.replace(f'merged_{pc:X}: {{',
                f'merged_{pc:X}: {{ replay_stop(ctx, 0x{pc:X}); ' +
                ('' if args.whole_phase else 'return;'))
        code += prefix
        for match in re.finditer(r'^static [^\n]*\bms_\w+\(', source[first:], re.M):
            start = first + match.start()
            end = geometry.definition_end(source, source.index('{', start))
            code += source[start:end] + '\n'
        for pc, value in functions.items():
            if pc not in pcs and pc not in stop:
                code += f'void {value[1]}(spu_context* ctx) {{ (void)ctx; abort(); }}\n'
    code += '\n'.join(functions[pc][0] for pc in sorted(pcs))
    for pc in sorted(stop):
        finish = 'wws_sort_epilogue(ctx);' if args.whole_phase else ''
        code += f'\nvoid {geometry.MODULE}_spu_func_{pc:08X}(spu_context* ctx) {{ replay_stop(ctx, 0x{pc:X}); {finish} }}\n'
    (args.output / 'wws_sort_reference.inc').write_text(code)
    print(f'Complete original sort: {len(pcs)} blocks', flush=True)
    cmd = ['clang', '-O2', '-std=gnu17', '-UNDEBUG', '-fno-strict-aliasing', '-fwrapv',
           '-ffp-contract=off', '-DSPU_LS_NEON', '-DSPU_LS_DIAGNOSTICS=0',
           '-DMOTORSTORM_WWS_CHAIN_TEST', '-DMOTORSTORM_SPU_THREAD_RTZ',
           '-include', ROOT/'port/spu_thread_rtz.h', '-include', ROOT/'port/spu_shufb_neon.h',
           '-I'+str(args.output.resolve()), '-I'+str(ROOT/'port'),
           '-I'+str(ROOT/'vendor/ps3recomp/runtime/spu'), '-I'+str(ROOT/'vendor/ps3recomp/include'),
           ROOT/'port/tests/test_wws_sort.c', ROOT/'port/wws_native_chain.c',
           ROOT/'port/spu_thread_rtz.c', '-o', args.output/'replay']
    if args.sanitize:
        cmd[1:1] = ['-g', '-fsanitize=address,undefined']
    if args.thinlto:
        cmd[1:1] = ['-flto=thin']
    if args.whole_phase:
        cmd[1:1] = ['-DTEST_WHOLE_PHASE']
    os.environ.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
    subprocess.run(list(map(str, cmd)), check=True)
    env = dict(os.environ, MOTORSTORM_JOB_MERGE='1', MOTORSTORM_SPU_PIPELINE='2',
               MOTORSTORM_JOB_SCOPES='0xf1', MOTORSTORM_NATIVE_GEOMETRY='1',
               MOTORSTORM_CULL_FAST_BOUNDS='1', MOTORSTORM_WWS_CHAIN='7' if args.whole_phase else '3')
    subprocess.run([str((args.output/'replay').resolve()), str(args.capture.resolve())], check=True, env=env)


if __name__ == '__main__':
    main()
