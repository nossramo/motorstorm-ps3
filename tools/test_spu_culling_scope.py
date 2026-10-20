#!/usr/bin/env python3
"""Differential test of the actual lifted hot block, before altering gameplay."""
from pathlib import Path
import argparse, hashlib, subprocess, tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT/'port/spu/jobmod_be896a6374ba_e12230/spu_recomp.c'
NAME = 'jobmod_be896a6374ba_e12230_spu_func_00008480'
SETUP_NAME = 'jobmod_be896a6374ba_e12230_spu_func_00008380'

def setup_bodies():
    source = SOURCE.read_text()
    begin = source.index('void '+SETUP_NAME+'(spu_context* ctx) {')
    end = source.index('\n}\n', begin) + 3
    original = source[begin:end]
    assert hashlib.sha256(original.encode()).hexdigest() == \
        '6879b04f063d9e40138850b63028931439fdce3d2d2f3278078e6e7aadc3b86d'
    optimized = original.replace(SETUP_NAME+'(', 'cull_setup_scope(')
    # All five local-store callbacks precede this arithmetic-only scope.
    optimized = optimized.replace('        ctx->gpr[11] =',
        '        const uint64_t old = ms_rtz_enter();\n        ctx->gpr[11] =', 1)
    lines=[]
    for line in optimized.splitlines():
        for op in ('fa','fs','fm'):
            if ' = spu_'+op+'(' in line:
                line=line.replace('spu_'+op+'(', 'ms_rtz_'+op+'(').replace(');', ', old);')
        if 'if (ctx->gpr[25]._u32[0] != 0)' in line:
            lines.append('        ms_rtz_leave(old);')
        lines.append(line)
    return original.replace(SETUP_NAME+'(', 'cull_setup_reference('), '\n'.join(lines)+'\n'

def bodies():
    source = SOURCE.read_text()
    begin = source.index('void '+NAME+'(spu_context* ctx) {')
    end = source.index('\n}\n', begin) + 3
    original = source[begin:end]
    assert hashlib.sha256(original.encode()).hexdigest() == \
        '9d556f68aa938fa75bd675a265b0ee9111fcb749d79eebdda4bec42646030ebb', \
        'Re-audit the bounded arithmetic proof before changing the lifted block'
    # The scope begins after the only LS read (which can invoke a diagnostic
    # callback); it ends before any branch or SPU_RET runtime dispatch.
    optimized = original.replace(NAME+'(', 'cull_scope(')
    optimized = optimized.replace('        ctx->gpr[14] =', '        const uint64_t old = ms_rtz_enter();\n        ctx->gpr[14] =', 1)
    lines=[]
    for line in optimized.splitlines():
        for op in ('fa','fm','fma'):
            if ' = spu_'+op+'(' in line:
                line=line.replace('spu_'+op+'(', 'ms_rtz_'+op+'(').replace(');', ', old);')
        if 'if (ctx->gpr[2]._u32[0] == 0)' in line:
            lines.append('        ms_rtz_leave(old);')
        lines.append(line)
    scoped = '\n'.join(lines)+'\n'
    marker = '        const uint64_t old = ms_rtz_enter();'
    prefix, tail = scoped.split(marker)
    # Both paths share the original load/callback, before changing FPCR.
    # Complete bodies preserve intermediate registers and both exit branches.
    suffix = marker + tail.rsplit('\n}', 1)[0]
    fast = suffix
    for op in ('fa', 'fm', 'fma'):
        fast = fast.replace('ms_rtz_'+op+'(', 'ms_cull_bounded_'+op+'(')
    bounded = prefix.replace('cull_scope(', 'cull_bounded(') + \
        '    if (ms_cull_bounds_inputs(ctx)) {\n' + fast + '\n    } else {\n' + suffix + '\n    }\n}\n'
    bounded = bounded.replace('spu_fcgt(', 'ms_cull_fcgt(')
    return original.replace(NAME+'(', 'cull_reference('), scoped, bounded

def profile_bodies(optimized, bounded):
    scoped = optimized.replace('void cull_scope(', 'static void ms_cull_profiled_scope(').replace(
        '        const uint64_t old = ms_rtz_enter();',
        '        (void)ms_cull_probe_inputs(ctx);\n        const uint64_t old = ms_rtz_enter();')
    guarded = bounded.replace('void cull_bounded(', 'static void ms_cull_profiled_bounds(').replace(
        'ms_cull_bounds_inputs(ctx)', 'ms_cull_probe_inputs(ctx)')
    return scoped, guarded

if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generate',type=Path,help='write opt-in build source, leaving the original lift intact')
    parser.add_argument('--sanitize', action='store_true')
    args=parser.parse_args()
    reference, optimized, bounded=bodies()
    profiled_scope, profiled_bounds = profile_bodies(optimized, bounded)
    if args.generate:
        source=SOURCE.read_text()
        original=reference.replace('cull_reference(',NAME+'(')
        assert source.count(original)==1
        replacement = optimized.replace('void cull_scope(', 'static void ms_cull_scoped(') + \
            bounded.replace('void cull_bounded(', 'static void ms_cull_bounded(') + \
            profiled_scope + profiled_bounds + \
            'void '+NAME+'(spu_context* ctx) {\n' + \
            '    const int mode = ms_cull_settings();\n' + \
            '    if ((mode & 2) && ms_cull_current_probe.active) {\n' + \
            '        if (mode & 1) ms_cull_profiled_bounds(ctx); else ms_cull_profiled_scope(ctx);\n' + \
            '    } else if (mode & 1) ms_cull_bounded(ctx); else ms_cull_scoped(ctx);\n}\n'
        gate = '''
#include <stdatomic.h>
static int ms_cull_settings(void) {
    static atomic_int enabled = -1;
    int value = atomic_load_explicit(&enabled, memory_order_relaxed);
    if (value < 0) {
        const char* e = getenv("MOTORSTORM_CULL_FAST_BOUNDS");
        value = e && e[0] == '1';
        e = getenv("MOTORSTORM_CULL_PROFILE");
        if (e && e[0] == '1') value |= 2;
        atomic_store_explicit(&enabled, value, memory_order_relaxed);
    }
    return value;
}
'''
        # The complete original call/drain/restore sequence stays intact.
        call = '        ctx->gpr[0] = spu_link(0x9644); { int32_t _si = (int32_t)ctx->image_id; ctx->host_depth++; jobmod_be896a6374ba_e12230_spu_func_00008380(ctx); spu_drain_call(ctx, 0x9644); ctx->host_depth--; spu_img_restore(ctx, _si); }'
        assert source.count(call) == 1, 'Re-audit the culling diagnostic call boundary'
        measured_call = '''        {
            ms_cull_probe probe = {0};
            if (ms_cull_settings() & 2) ms_cull_probe_begin(&probe);
''' + call + '''
            ms_cull_probe_end(&probe, ctx->spu_id);
        }'''
        source = source.replace(original,replacement).replace(call,measured_call)
        args.generate.write_text('#include "'+str(ROOT/'port/spu_cull_profile.h')+'"\n'+gate+source)
        raise SystemExit(0)
    # Retain the rejected setup-scope experiment as a reproducible comparison;
    # it is deliberately absent from generated production code.
    setup_reference, setup_optimized = setup_bodies()
    with tempfile.TemporaryDirectory(prefix='motorstorm-cull-') as d:
        path=Path(d)
        (path/'culling.inc').write_text('\n'.join((reference,optimized,bounded,profiled_scope,profiled_bounds,setup_reference,setup_optimized)))
        command=['clang','-O2','-fno-strict-aliasing','-fwrapv','-ffp-contract=off',
            '-I'+str(path),'-I'+str(ROOT/'port'),
            '-I'+str(ROOT/'vendor/ps3recomp/runtime/spu'),
            '-I'+str(ROOT/'vendor/ps3recomp/include'),
            str(ROOT/'port/tests/test_spu_culling_scope.c'),'-o',str(path/'test')]
        if args.sanitize: command[1:1] = ['-g', '-fsanitize=address,undefined']
        subprocess.run(command,check=True)
        subprocess.run([str(path/'test')],check=True,timeout=45)
