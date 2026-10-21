#!/usr/bin/env python3
"""Merge only four measured Multistream loops, preserving the original lift.

Both paths stay in the same binary for controlled replay and gameplay A/B.
Generated source contains game code and belongs only in ignored build/.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
from specialize_audio_bounds import specialize as bounded_arithmetic

ROOT = Path(__file__).resolve().parents[1]
IMAGE = 'spu_1136862334bb6208f794040f3a7f26e4991cd6b00eac6229331432566f1535d1'
INPUT = ROOT / 'port/spu' / IMAGE
ENTRIES = (0x5238, 0x5670, 0x5848, 0x98D8)
CODE_SHA = '97480975c296a627dca1edbeecd1a6114cdc960564338778600c6b61da9eb82e'


def private_registers(body):
    """Keep loop registers separate from LS; publish at every host/guest exit.

    With diagnostics excluded, the only observers of guest registers are the
    calls and exits below. A private array also proves that LS stores cannot
    alias registers, allowing the compiler to keep live values in ARM64 SIMD
    registers instead of spilling them after each sample load/store.
    """
    save = 'memcpy(ctx->gpr, audio_registers, sizeof audio_registers);'
    load = 'memcpy(audio_registers, ctx->gpr, sizeof audio_registers);'
    lines = body.splitlines()
    result = [lines[0], '    u128 audio_registers[128];', '    ' + load]
    for line in lines[1:-1]:
        if 'spu_drain_call(' in line:
            # arithmetic_scope already restored host FPCR before this line.
            # Keep its link-register assignment in the published guest state.
            result += ['    ' + save, line, '    ' + load]
            continue
        line = line.replace('ctx->gpr', 'audio_registers')
        line = line.replace('SPU_RET(ctx);', save + ' SPU_RET(ctx);')
        line = line.replace('return;', save + ' return;')
        result.append(line)
    # Includes the fallthrough exit; FPCR was restored by arithmetic_scope.
    result += ['    ' + save, lines[-1]]
    return '\n'.join(result)


def arithmetic_scope(body, entry):
    """Keep RTZ inside arithmetic/LS code, restoring it at every host boundary."""
    import re
    allowed = {'a','ai','andi','ceq','ceqi','cflts','cgt','cgti','csflt','cwd',
               'fa','fm','fma','fms','fs','fsmbi','il','ila','ilhu','link',
               'ls_read128','ls_write128','ori','rotmai','rotqby','rotqbyi',
               'selb','shli','shlqbyi','shufb','drain_call','img_restore'}
    names = set(re.findall(r'\bspu_(\w+)\(', body))
    unknown = {n for n in names if n not in allowed and not n.startswith(IMAGE[4:] + '_spu_func_')}
    if unknown:
        raise SystemExit(f'Unaudited helper in audio scope: {unknown}')
    scoped = body.replace(f'ms_audio_merged_{entry:X}(', f'ms_audio_scoped_{entry:X}(', 1)
    lines = scoped.splitlines()
    result = [lines[0], '    uint64_t audio_fpcr = ms_rtz_enter();']
    for line in lines[1:-1]:
        # A guest call runs its helper, drain and image restore in host mode.
        if 'spu_drain_call(' in line:
            result += ['    ms_rtz_leave(audio_fpcr);', line, '    audio_fpcr = ms_rtz_enter();']
            continue
        for operation in ('fa','fm','fma','fms','fs'):
            if ' = spu_' + operation + '(' in line:
                helper = 'ms_audio_rtz_fms' if operation == 'fms' else 'ms_rtz_' + operation
                line = line.replace('spu_' + operation + '(', helper + '(').replace(');', ', audio_fpcr);')
        # A conditional exit must restore only if the branch is actually taken.
        line = line.replace('SPU_RET(ctx);', 'ms_rtz_leave(audio_fpcr); SPU_RET(ctx);')
        line = line.replace('return;', 'ms_rtz_leave(audio_fpcr); return;')
        result.append(line)
    result += ['    ms_rtz_leave(audio_fpcr);', lines[-1]]
    return '\n'.join(result)


def generate(output):
    code = (INPUT / 'code.bin').read_bytes()
    if hashlib.sha256(code).hexdigest() != CODE_SHA:
        raise SystemExit('Unexpected Multistream code: re-audit before specialization.')
    sys.path.insert(0, str(ROOT / 'vendor/ps3recomp/tools'))
    from spu_lifter import SPULifter, merge_chunk_groups, compute_bi_r0_jumps, compute_link_returns
    from spu_disasm import disassemble_spu
    bounds = [(b['start'], b['end']) for b in json.loads((INPUT / 'functions.json').read_text())]
    instructions = disassemble_spu(code, 0x3000)
    groups = merge_chunk_groups(instructions, bounds, 4096, {0x3000, *ENTRIES})
    lifter = SPULifter(prefix=IMAGE + '_')
    lifter.func_starts = {s for s, _ in bounds}
    lifter.bi_r0_jump = compute_bi_r0_jumps(instructions, bounds)
    lifter.link_return = compute_link_returns(instructions, bounds)
    source = (INPUT / 'spu_recomp.c').read_text()
    additions = []
    for index, entry in enumerate(ENTRIES):
        if entry not in groups:
            raise SystemExit(f'No mergeable group for {entry:X}')
        # Compare the existing generated body with a fresh identical lift.
        end = dict(bounds)[entry]
        check = SPULifter(prefix=IMAGE + '_')
        check.func_starts = lifter.func_starts
        check.bi_r0_jump = lifter.bi_r0_jump
        check.link_return = lifter.link_return
        check.lift_function(instructions, entry, end)
        name = f'{IMAGE}_spu_func_{entry:08X}'
        old_begin = source.index(f'void {name}(spu_context* ctx) {{')
        old_end = source.index('\n}', old_begin) + 2
        old_body = source[old_begin:old_end]
        fresh = check.emit_source('spu_recomp.h')
        fresh_begin = fresh.index(f'void {name}(spu_context* ctx) {{')
        fresh_end = fresh.index('\n}', fresh_begin) + 2
        if old_body != fresh[fresh_begin:fresh_end]:
            raise SystemExit(f'Existing lift differs at {entry:X}; refusing replacement.')
        lifter.lift_function(instructions, entry, groups[entry])
        fresh = lifter.emit_source('spu_recomp.h')
        fresh_begin = fresh.index(f'void {name}(spu_context* ctx) {{')
        fresh_end = fresh.index('\n}', fresh_begin) + 2
        optimized = fresh[fresh_begin:fresh_end].replace(f'void {name}(', f'static void ms_audio_merged_{entry:X}(', 1)
        reference = old_body.replace(f'void {name}(', f'static void ms_audio_reference_{entry:X}(', 1)
        scoped = arithmetic_scope(optimized, entry)
        compact = scoped.replace(f'ms_audio_scoped_{entry:X}(', f'ms_audio_compact_{entry:X}(', 1)
        for op in ('fa', 'fs', 'fm', 'fma'):
            compact = compact.replace('ms_rtz_' + op + '(', 'ms_audio_compact_' + op + '(')
        compact = compact.replace('ms_audio_rtz_fms(', 'ms_audio_compact_fms(')
        direct_ls = compact.replace(f'ms_audio_compact_{entry:X}(', f'ms_audio_direct_ls_{entry:X}(', 1)
        direct_ls = direct_ls.replace('spu_ls_read128(', 'ms_audio_ls_read128(')
        direct_ls = direct_ls.replace('spu_ls_write128(', 'ms_audio_ls_write128(')
        direct_ls = private_registers(direct_ls)
        bounded = bounded_arithmetic(direct_ls, entry)
        wrapper = f'''void {name}(spu_context* ctx) {{
    ms_audio_worker_capture(ctx, 0x{entry:X}, {index});
    if (ms_audio_worker_mode() >= 5 && ms_audio_worker_scope_safe(ctx)) ms_audio_bounded_{entry:X}(ctx);
    else if (ms_audio_worker_mode() >= 4 && ms_audio_worker_scope_safe(ctx)) ms_audio_direct_ls_{entry:X}(ctx);
    else if (ms_audio_worker_mode() >= 3 && ms_audio_worker_scope_safe(ctx)) ms_audio_compact_{entry:X}(ctx);
    else if (ms_audio_worker_mode() >= 2 && ms_audio_worker_scope_safe(ctx)) ms_audio_scoped_{entry:X}(ctx);
    else if (ms_audio_worker_mode()) ms_audio_merged_{entry:X}(ctx);
    else ms_audio_reference_{entry:X}(ctx);
}}'''
        source = source.replace(old_body, reference + '\n' + optimized + '\n' + scoped + '\n' + compact + '\n' + direct_ls + '\n' + bounded + '\n' + wrapper, 1)
        additions.append(f'{entry:05X}-{groups[entry]:05X}')
    if lifter.unsupported:
        raise SystemExit('Unsupported instructions in merged audio loops.')
    output.parent.mkdir(parents=True, exist_ok=True)
    # App launchers apply environment settings after C constructors. Re-read
    # these settings once at game boot, before any guest/audio worker starts.
    configure = '''#ifndef MOTORSTORM_AUDIO_WORKER_TEST
void port_audio_worker_configure(void) {
    ms_audio_worker_init();
    fprintf(stderr, "[audio-worker] configured mode=%d\\n", ms_audio_worker_mode());
}
#endif
'''
    output.write_text('#include "spu_audio_worker.h"\n' + configure + source)
    print('Generated checked audio loop groups: ' + ', '.join(additions))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generate', type=Path, required=True)
    parser.add_argument('--benchmark', action='store_true', help='Namespace the isolated developer replay')
    args = parser.parse_args()
    generate(args.generate)
    if args.benchmark:
        header = args.generate.with_suffix('.h')
        header.write_text((INPUT / 'spu_recomp.h').read_text().replace(IMAGE, 'benchmark_' + IMAGE))
        source = args.generate.read_text().replace(IMAGE, 'benchmark_' + IMAGE)
        args.generate.write_text(source.replace('#include "spu_recomp.h"', f'#include "{header.name}"'))
