#!/usr/bin/env python3
"""Source safety checks; run test_job_merge.py --pipeline for actual blocks."""
import specialize_spu_pipeline as p

assignments = ''.join(f'    ctx->gpr[{i}] = spu_il({i});\n' for i in range(9))
body = assignments + '    if (ctx->gpr[8]._u32[0] != 0) return;\n    SPU_RET(ctx);\n'
out, report = p.rewrite(body)
assert report['written'] == 9
assert 'if (sr8._u32[0] != 0) do {' in out
assert 'ctx->gpr[0] = sr0;' in out
assert 'SPU_RET(ctx)' in out
callback = assignments + '    ctx->gpr[0] = spu_rdch(ctx, 3);\n    ctx->gpr[1] = ctx->gpr[12];\n'
out, report = p.rewrite(callback)
assert report['boundaries'] == 1
before, after = out.split('    ctx->gpr[0] = spu_rdch(ctx, 3);')
assert 'ctx->gpr[8] = sr8;' in before
assert 'sr0 = ctx->gpr[0];' in after and 'sr12 = ctx->gpr[12];' in after
assert p.rewrite(assignments + '    observer(ctx);\n')[0] is None
assert p.rewrite(assignments + '    if (1) observer(ctx);\n')[0] is None
for extra in ['    ctx->gpr[index] = spu_il(0);', '    ctx->gpr[0]._u32[0] = 1;']:
    try:
        p.rewrite(assignments + extra)
    except ValueError:
        pass
    else:
        raise AssertionError('Unknown register write accepted')
assert p.rewrite(assignments + '    use(&ctx->gpr[0]);')[0] is None
source = 'static __attribute__((noinline)) void ms_merged_jobmod_test_0(spu_context* ctx, uint32_t entry) {\n' + assignments + '{\n/* } */\n}\nSPU_RET(ctx);\n}\n'
result, report = p.generate(source, 'jobs')
assert len(report['selected']) == 1
assert len(list(p.functions(result))) == 3
print('PASS: balanced nested functions, callback publication/reload, conditional returns and fail-closed context/register access')
