#!/usr/bin/env python3
"""Replay every fused entry against its retained original WWS body.

Uses the actual production-generated functions, with a bounded drain loop.
Compares the whole context, local store, exit target and FPCR, including slow
dispatch/interrupt fallbacks. Synthetic inputs are not a gameplay benchmark.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

import specialize_job_merge as merge


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--pipeline', action='store_true')
    args = parser.parse_args()
    root = merge.ROOT
    with tempfile.TemporaryDirectory(prefix='motorstorm-job-merge-') as folder:
        work = Path(folder)
        def run(*argv):
            subprocess.run(list(map(str, argv)), cwd=root, check=True)
        includes, definitions, entries = [], [], []
        for module in merge.HASHES:
            original = root / 'port/spu' / module / 'spu_recomp.c'
            source = original
            if module.endswith('e12230'):
                source = work / 'cull.c'
                run('python3', root / 'tools/test_spu_culling_scope.py', '--generate', source)
            scopes = work / (module + '_scopes.c')
            run('python3', root / 'tools/specialize_job_arithmetic.py', '--input', source, '--generate', scopes)
            output = work / (module + '_merged.c')
            run('python3', root / 'tools/specialize_job_merge.py', '--module', module,
                '--input', scopes, '--generate', output)
            import json
            report = json.loads(output.with_suffix('.json').read_text())
            selected = {pc for group in report['groups'] for pc in group}
            generated = output.read_text()
            functions = merge.functions(generated)
            baseline = merge.functions(original.read_text())
            # Every unselected body, including all previous optimizations,
            # must survive the production transformation unchanged.
            for pc, before in merge.functions(scopes.read_text()).items():
                if pc not in selected:
                    assert functions[pc][0] == before[0]
            start = generated.index('static __attribute__((noinline)) void ms_merged_')
            end = generated.index('void ' + next(iter(baseline.values()))[1] + '(')
            merged_definitions = generated[start:end]
            if args.pipeline:
                from specialize_spu_pipeline import generate
                merged_definitions, native_report = generate(merged_definitions, 'jobs')
                print(f"Native registers: {module}: {len(native_report['selected'])} regions")
            definitions.append(merged_definitions)
            for pc in sorted(selected):
                definitions.append(functions[pc][0])
                entries.append(f'{{{functions[pc][1]}, 0x{pc:X}}}')
            # External edges end the replay, rather than executing an
            # unmodelled channel, call or previously specialized routine.
            external = set()
            for pc in selected:
                for edge in merge.EDGE.finditer(baseline[pc][3]):
                    if int(edge[1], 16) not in selected:
                        external.add(edge[2])
            definitions.extend(f'void {name}(spu_context* ctx) {{ abort(); }}' for name in sorted(external))
            includes.append(f'#include "{original.with_suffix(".h")}"')
        (work / 'job_merge.inc').write_text('\n'.join(includes + definitions) +
            '\nstatic const struct { void (*fn)(spu_context*); uint32_t pc; } cases[] = {\n' +
            ',\n'.join(entries) + '\n};\n')
        command = ['clang', '-O2', '-std=gnu17', '-UNDEBUG', '-fno-strict-aliasing',
                   '-fwrapv', '-ffp-contract=off', '-DSPU_LS_NEON',
                   '-DMOTORSTORM_JOB_MERGE_TEST', '-DMOTORSTORM_SPU_THREAD_RTZ',
                   '-include', root / 'port/spu_thread_rtz.h',
                   '-I' + str(work), '-I' + str(root / 'port'),
                   '-I' + str(root / 'vendor/ps3recomp/runtime/spu'),
                   '-I' + str(root / 'vendor/ps3recomp/include'),
                   root / 'port/tests/test_job_merge.c',
                   root / 'port/spu_thread_rtz.c', '-o', work / 'replay']
        if args.sanitize:
            command[1:1] = ['-g', '-fsanitize=address,undefined']
        if args.pipeline:
            command[1:1] = ['-DMOTORSTORM_SPU_PIPELINE_TEST', '-DSPU_LS_DIAGNOSTICS=0']
        os.environ.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
        run(*command)
        run(work / 'replay')


if __name__ == '__main__':
    main()
