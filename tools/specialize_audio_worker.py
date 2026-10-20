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

ROOT = Path(__file__).resolve().parents[1]
IMAGE = 'spu_1136862334bb6208f794040f3a7f26e4991cd6b00eac6229331432566f1535d1'
INPUT = ROOT / 'port/spu' / IMAGE
ENTRIES = (0x5238, 0x5670, 0x5848, 0x98D8)
CODE_SHA = '97480975c296a627dca1edbeecd1a6114cdc960564338778600c6b61da9eb82e'


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
        wrapper = f'''void {name}(spu_context* ctx) {{
    ms_audio_worker_capture(ctx, 0x{entry:X}, {index});
    if (ms_audio_worker_mode() >= 2 && ms_audio_worker_scope_safe(ctx)) ms_audio_scoped_{entry:X}(ctx);
    else if (ms_audio_worker_mode()) ms_audio_merged_{entry:X}(ctx);
    else ms_audio_reference_{entry:X}(ctx);
}}'''
        source = source.replace(old_body, reference + '\n' + optimized + '\n' + scoped + '\n' + wrapper, 1)
        additions.append(f'{entry:05X}-{groups[entry]:05X}')
    if lifter.unsupported:
        raise SystemExit('Unsupported instructions in merged audio loops.')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text('#include "spu_audio_worker.h"\n' + source)
    print('Generated checked audio loop groups: ' + ', '.join(additions))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generate', type=Path, required=True)
    generate(parser.parse_args().generate)
