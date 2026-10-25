#!/usr/bin/env python3
"""Checked arithmetic segments selected from the real iPhone CPU profile.

Generated files contain local game code and must stay in ignored build/logs.
Memory accesses and all original dispatch/callback boundaries remain outside
each scope. A helper not audited here terminates a segment, never enters it.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
# Captured-input component replay on iPhone 16 Pro Max: cases 0/4/5/6/7
# save 8.78/29.09/13.40/41.08/7.29% thread CPU. Cases 1/2/3 regressed.
# This is not a whole-game FPS claim. --mask 0xff enables capture of all
# candidates for future analysis; normal generation retains the losing code.
MEASURED_MASK = 0xf1
# Profile: ios-device-profile-03, UUID 5D3B5339-D255-3789-B733-EB1262602F5D.
HASHES = {
    'jobmod_e5d3c4d056fe_e12E10_spu_func_0000A87C': '792c33708410e8345d428df0c23d1c7f6c31af254db05aabc71ff1227870bf31',
    'jobmod_be896a6374ba_e12230_spu_func_000051B8': '5d61b9fe28aad2336132197ad2cf9e8e85ad6206747038bf256ebe8ca61f5895',
    'jobmod_be896a6374ba_e12230_spu_func_0000F0A0': '4763eeca3b41cf4937f05222ad39f6dbc17c3644baa602445d37f117bbe9e087',
    'jobmod_be896a6374ba_e12230_spu_func_00008908': 'cf24f630b37b1957a5c2299d1652c1e3aeb0a5dc34a6e5dfec8c91bf235dd5cd',
    'jobmod_be896a6374ba_e12230_spu_func_00007B30': 'cd89a88786f1cf945df9f05103da585fcc794a4843365dee5aa5d7b0263cdd60',
    'jobmod_be896a6374ba_e12230_spu_func_00008F70': '12e78c5481a4e051f9f688903a2cadd377df3b37385d7edc9b12be2685419613',
    'jobmod_be896a6374ba_e12230_spu_func_00008220': '6f5f5f2bd2a69613c221d862669683f84d3933c903aa72890ebd4d6fb21e96dc',
    'jobmod_be896a6374ba_e12230_spu_func_00009410': 'a05ccbe96d8718075a61f809de83e86dce322c0970d7cea42aea4b267e09da76',
    'jobmod_be896a6374ba_e12230_spu_func_00008C78': '192d776379eeee6a904c7e314ded78191cd9fbe9adb33f5c444142a7db08aba4',
    'jobmod_be896a6374ba_e12230_spu_func_00009530': 'e17b2abb0fa6dd59ff5d31ef473444c934a3503bcbf2f7469c8f1119a049da1d',
    'jobmod_be896a6374ba_e12230_spu_func_00008A40': '98a1b80a3df1511e4086d7f5ea54015fe74539eae91185b25da4725ff074e6cb',
}
FLOAT = {'fa', 'fs', 'fm', 'fma', 'fnms'}
# All non-arithmetic entries below use integer operations only, including
# table estimates/interpolation and SPU comparisons. No double/IEEE helpers.
PURE = FLOAT | {'a', 'ai', 'and', 'andi', 'andc', 'ceq', 'ceqi', 'ceqbi',
    'clgt', 'clgti', 'cwd', 'cwx', 'fcgt', 'fi', 'frest', 'frsqest', 'fsmbi',
    'il', 'ila', 'ilhu', 'iohl', 'nor', 'ori', 'rotqby', 'rotqbyi', 'selb',
    'sfi', 'shl', 'shli', 'shlqbyi', 'shufb', 'xor', 'xsbh'}
ASSIGN = re.compile(r'^\s*ctx->gpr\[\d+\] = spu_(\w+)\([^;{}]*\);$')


def segments(body):
    lines = body.splitlines(keepends=True)
    start = count = 0
    for i, line in enumerate(lines + ['END']):
        match = ASSIGN.fullmatch(line.rstrip('\n'))
        op = match.group(1) if match else None
        if op in PURE:
            count += op in FLOAT
        elif line.strip() in ('/* nop */;', '/* branch hint (ignored) */;'):
            continue
        else:
            if count >= 8:
                yield start, i, ''.join(lines[start:i]), count
            start, count = i + 1, 0


def arithmetic_scope(segment):
    lines = ['        {\n            const uint64_t job_fpcr = ms_rtz_enter();\n'
             '            const uint64_t job_math_fpcr = job_fpcr & ~0xc00000ull;\n']
    for line in segment.splitlines(keepends=True):
        match = ASSIGN.fullmatch(line.rstrip('\n'))
        if match and match.group(1) in FLOAT:
            op = match.group(1)
            helper = 'ms_job_' + op
            line = line.replace('spu_' + op + '(', helper + '(').replace(');', ', job_math_fpcr);')
        line = line.replace('spu_fcgt(', 'ms_cull_fcgt(').replace('spu_fi(', 'ms_job_fi(')
        lines.append(line)
    lines.append('            ms_rtz_leave(job_fpcr);\n        }\n')
    return ''.join(lines)


def scoped(segment):
    # Existing exact helpers assume nearest rounding. If an unusual caller
    # uses another mode, preserve its unmodified behavior for this whole
    # segment instead of mixing that mode with scoped native arithmetic.
    return ('{\n        uint64_t job_caller_fpcr;\n'
            '        __asm__ volatile("mrs %0, fpcr" : "=r"(job_caller_fpcr));\n'
            '        if (job_caller_fpcr & 0xc00000ull) {\n' + segment +
            '        } else {\n' + arithmetic_scope(segment) + '        }\n}\n')


def bodies():
    for name, digest in HASHES.items():
        module = name.split('_spu_func_')[0]
        source = (ROOT / 'port/spu' / module / 'spu_recomp.c').read_text()
        start = source.index('void ' + name + '(spu_context* ctx) {')
        end = source.index('\n}', start) + 2
        body = source[start:end]
        if hashlib.sha256(body.encode()).hexdigest() != digest:
            raise SystemExit('Re-audit changed native job function: ' + name)
        yield module, name, body


def test_source(destination):
    output = []
    entries = []
    manifest = []
    for module, name, body in bodies():
        for start, end, segment, count in segments(body):
            i = len(entries)
            for variant, code in [('reference', segment), ('scoped', scoped(segment))]:
                output.append(f'static __attribute__((noinline)) void job_{variant}_{i}(spu_context* ctx) {{\n' + code + '}\n')
            entries.append(f'{{job_reference_{i}, job_scoped_{i}}}')
            manifest.append(dict(index=i, name=name, start_line=start, end_line=end, float_ops=count))
    output.append('static void (*job_cases[][2])(spu_context*) = {' + ','.join(entries) + '};\n')
    destination.write_text(''.join(output))
    destination.with_suffix('.json').write_text(json.dumps(manifest, indent=2) + '\n')


def generate(source_path, destination, mask=MEASURED_MASK):
    source = source_path.read_text()
    index = 0
    found = False
    for module, name, body in bodies():
        parts = list(segments(body))
        if body not in source:
            if 'void ' + name + '(' in source:
                raise SystemExit('Re-audit changed input function: ' + name)
            index += len(parts)
            continue
        found = True
        lines = body.splitlines(keepends=True)
        selected = False
        for local_index, (start, end, segment, count) in reversed(list(enumerate(parts))):
            case = index + local_index
            if not (mask & (1 << case)):
                continue
            selected = True
            lines[start:end] = [
                f'        if (job_settings & 0x80000000u) ms_job_scope_capture(ctx, {case});\n'
                f'        if (job_settings & (1u << {case})) ' + scoped(segment).lstrip() +
                '        else {\n' + segment + '        }\n']
        if selected:
            lines.insert(1, '    const unsigned job_settings = ms_job_scope_settings();\n')
            source = source.replace(body, ''.join(lines), 1)
        index += len(parts)
    if not found:
        raise SystemExit('No audited job functions in input: ' + str(source_path))
    destination.write_text('#include "spu_job_scope.h"\n' + source)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--test-source', type=Path)
    parser.add_argument('--input', type=Path)
    parser.add_argument('--generate', type=Path)
    parser.add_argument('--mask', type=lambda value: int(value, 0), default=MEASURED_MASK)
    args = parser.parse_args()
    if args.test_source:
        test_source(args.test_source)
    elif args.input and args.generate:
        if args.mask < 0 or args.mask > 0xff:
            parser.error('--mask must be between 0 and 0xff')
        generate(args.input, args.generate, args.mask)
    else:
        parser.error('Specify --test-source or --input and --generate')
