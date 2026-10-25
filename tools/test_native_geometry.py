#!/usr/bin/env python3
"""Replay complete native geometry calls against the actual lifted functions."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
import specialize_native_geometry as geometry


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--thinlto', action='store_true')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--capture', type=Path, help='Directory of pointer-free iOS geometry fixtures')
    parser.add_argument('--production-baseline', action='store_true', help='Compare with the existing cull/scopes/merge pipeline')
    args = parser.parse_args()
    os.environ.setdefault('DEVELOPER_DIR','/Applications/Xcode.app/Contents/Developer')
    source=geometry.checked_source()
    changed=geometry.generate(source)
    original=geometry.functions(source)
    functions=geometry.functions(changed)
    for pc,before in original.items():
        if pc in geometry.ENTRIES:
            assert functions[pc][3].split('\n',3)[3]==before[3]
        else:
            assert functions[pc][0]==before[0]
    pcs=(0x8120,0x81C0,0x81C8,0x821C,0x8220,0x8380,0x8420,0x8428,0x847C,0x8480)
    with tempfile.TemporaryDirectory(prefix='motorstorm-native-geometry-') as temporary:
        work=args.output or Path(temporary)
        work.mkdir(parents=True,exist_ok=True)
        additions=''
        if args.production_baseline:
            def generate(*command):
                subprocess.run(list(map(str,command)),check=True)
            generate('python3',geometry.ROOT/'tools/test_spu_culling_scope.py','--generate',work/'cull.c')
            generate('python3',geometry.ROOT/'tools/specialize_job_arithmetic.py','--input',work/'cull.c','--generate',work/'scopes.c')
            generate('python3',geometry.ROOT/'tools/specialize_job_merge.py','--module',geometry.MODULE,
                     '--input',work/'scopes.c','--generate',work/'merged.c')
            source=(work/'merged.c').read_text()
            changed=geometry.generate(source)
            functions=geometry.functions(changed)
            for pc,before in geometry.functions(source).items():
                if pc in geometry.ENTRIES:assert functions[pc][3].split('\n',3)[3]==before[3]
                else:assert functions[pc][0]==before[0]
            additions='#include "spu_cull_profile.h"\n#include "spu_job_scope.h"\n#include "spu_job_merge.h"\n'
            names=['ms_cull_settings','ms_cull_scoped','ms_cull_bounded','ms_cull_profiled_scope','ms_cull_profiled_bounds']
            names += [f'ms_merged_{geometry.MODULE}_{pc:X}' for pc in geometry.ENTRIES]
            for name in names:
                start=re.search(r'^static [^\n]*\b'+name+r'\(',source,re.M)
                if not start:raise ValueError('Missing production helper: '+name)
                end=geometry.definition_end(source,source.index('{',start.start()))
                additions+=source[start.start():end]+'\n'
        code=f'#include "{geometry.SOURCE.with_suffix(".h")}"\n'
        code+=additions+'\n'.join(functions[pc][0] for pc in pcs)
        (work/'geometry.inc').write_text(code)
        cmd=['clang','-O2','-std=gnu17','-UNDEBUG','-fno-strict-aliasing','-fwrapv','-ffp-contract=off',
             '-DSPU_LS_NEON','-DSPU_LS_DIAGNOSTICS=0','-DMOTORSTORM_GEOMETRY_TEST',
             '-DMOTORSTORM_SPU_THREAD_RTZ','-include',geometry.ROOT/'port/spu_thread_rtz.h',
             '-include',geometry.ROOT/'port/spu_shufb_neon.h',
             '-I'+str(work),'-I'+str(geometry.ROOT/'port'),
             '-I'+str(geometry.ROOT/'vendor/ps3recomp/runtime/spu'),
             '-I'+str(geometry.ROOT/'vendor/ps3recomp/include'),
             geometry.ROOT/'port/tests/test_native_geometry.c',
             geometry.ROOT/'port/spu_thread_rtz.c','-o',work/'replay']
        if args.thinlto:cmd[1:1]=['-flto=thin']
        if args.sanitize:cmd[1:1]=['-g','-fsanitize=address,undefined']
        subprocess.run(list(map(str,cmd)),check=True)
        env=dict(os.environ,MOTORSTORM_JOB_MERGE='1',MOTORSTORM_JOB_SCOPES='0xf1')
        capture=args.capture
        if capture is None:
            capture=work/'self-capture'
            env.update(MOTORSTORM_GEOMETRY_SELF_CAPTURE='1',MOTORSTORM_GEOMETRY_CAPTURE=str(capture))
        subprocess.run([str(work/'replay'),str(capture)],check=True,env=env)


if __name__=='__main__':main()
