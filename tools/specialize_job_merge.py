#!/usr/bin/env python3
"""Fuse checked WWS basic blocks after the existing arithmetic specializations.

Only the two measured iOS modules are accepted. Original function symbols,
reference bodies and all specialized bodies remain available. Call/channel/
interrupt boundaries are not crossed; direct edges in bounded regions become
guarded gotos. Generated game code belongs in the SDK-specific build directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
HASHES = {
    'jobmod_e5d3c4d056fe_e12E10': 'd452a79bd378b08975af8094d7189bf52eba8fe73232aecd107a5e54f24a6bb6',
    'jobmod_be896a6374ba_e12230': '2e5ea1971e8dee9edb493a4f96182a719dffae588ffeff15a378aa1d3f9cf7f9',
}
FUNCTION = re.compile(r'^void (jobmod_\w+_spu_func_([0-9A-F]{8}))\(spu_context\* ctx\) \{\n(.*?)^\}', re.M | re.S)
EDGE = re.compile(r'\{ ctx->pc = 0x([0-9A-F]+); g_spu_trampoline_fn = (jobmod_\w+_spu_func_([0-9A-F]{8})); return; \}')
# An allowlist is intentional: a new helper must be audited before fusion.
PURE = set('a ai and andbi andhi andi andc bg ceq ceqb ceqbi ceqh ceqhi ceqi cflts cfltu cgt cgtb cgtbi cgth cgthi cgti clgt clgtb clgtbi clgth clgthi clgti clz cntb csflt cuflt cbd cbx chd chx cwd cwx cdd cdx eqv fa fceq fcgt fcmgt fcmeq fi fm fma fms fnms frest frsqest fs fsmb fsmbi fsmh fsm gb gbb gbh il ilh ilhu ila iohl ls_read128 ls_write128 mpy mpyh mpyhh mpyhhu mpyi mpyu mpyui nand nor or orbi orhi ori orc orx rot roti roth rothi rothm rothmi rotm rotmi rotma rotmai rotmah rotmahi rotqbi rotqbii rotqby rotqbyi rotqbybi rotqmbi rotqmbii rotqmby rotqmbyi rotqmbybi selb sf sfi shl shli shlh shlhi shlqbi shlqbii shlqby shlqbyi shlqbybi shufb sumb xsbh xshw xswd xor xorbi xorhi xori'.split())


def functions(source):
    return {int(m[2], 16): m for m in FUNCTION.finditer(source)}


def regions(source, original, max_lines=800):
    current, baseline = functions(source), functions(original)
    eligible = {}
    for pc, m in current.items():
        # Preserve every previously validated specialization byte-for-byte.
        if pc not in baseline or m[0] != baseline[pc][0]:
            continue
        body = m[3]
        stripped = EDGE.sub('', body)
        helpers = set(re.findall(r'\bspu_(\w+)\(', stripped))
        if helpers - PURE or 'goto ' in body or 'loc_' in body:
            continue
        if re.search(r'\bjobmod_\w+\(', stripped) or re.search(r'ctx->(?!gpr\[|pc\b)', stripped):
            continue
        if any(int(e[1], 16) != int(e[3], 16) for e in EDGE.finditer(body)):
            raise ValueError('Direct branch PC and function disagree')
        eligible[pc] = m
    adjacent = {pc: set() for pc in eligible}
    for pc, m in eligible.items():
        for edge in EDGE.finditer(m[3]):
            target = int(edge[1], 16)
            if target in eligible:
                adjacent[pc].add(target)
                adjacent[target].add(pc)
    remaining = set(eligible)
    groups = []
    while remaining:
        todo, chosen, size = [min(remaining)], [], 0
        while todo:
            pc = todo.pop()
            if pc not in remaining:
                continue
            cost = len(eligible[pc][3].splitlines())
            if chosen and size + cost > max_lines:
                continue
            remaining.remove(pc)
            chosen.append(pc)
            size += cost
            todo.extend(sorted(adjacent[pc] & remaining, reverse=True))
        if len(chosen) > 1:
            groups.append(sorted(chosen))
    return current, groups


def merged_body(module, members, bodies, name):
    out = [f'static __attribute__((noinline)) void {name}(spu_context* ctx, uint32_t entry) {{',
           '    switch (entry) {']
    for pc in members:
        out.append(f'    case 0x{pc:X}: goto merged_{pc:X};')
    out += ['    default: abort();', '    }']
    for pc in members:
        def fuse(m):
            target = int(m[1], 16)
            if target not in members:
                return m[0]
            return ('{ ctx->pc = 0x' + m[1] + '; '
                    f'if (ms_job_merge_hop(ctx)) goto merged_{target:X}; '
                    f'g_spu_trampoline_fn = {m[2]}; return; }}')
        # Braces isolate any local declarations in the original block.
        out += [f'merged_{pc:X}: {{', EDGE.sub(fuse, bodies[pc][3]), '    return;', '}']
    out.append('}')
    return '\n'.join(out)


def generate(source, original, module):
    bodies, groups = regions(source, original)
    additions, replacements = [], {}
    for members in groups:
        name = f'ms_merged_{module}_{members[0]:X}'
        additions.append(merged_body(module, members, bodies, name))
        for pc in members:
            m = bodies[pc]
            replacements[pc] = (f'void {m[1]}(spu_context* ctx) {{\n'
                f'    if (ms_job_merge_enabled() && ctx->policy_mode) {{ {name}(ctx, 0x{pc:X}); return; }}\n'
                + m[3] + '}')
    rewritten = FUNCTION.sub(lambda m: replacements.get(int(m[2], 16), m[0]), source)
    # Insert after includes but before the first function, so the original
    # generated header has declared every fallback target.
    at = rewritten.index('void ' + next(iter(bodies.values()))[1] + '(')
    rewritten = rewritten[:at] + '\n'.join(additions) + '\n\n' + rewritten[at:]
    report = {'module': module, 'groups': groups,
              'merged_blocks': sum(map(len, groups)),
              'internal_edges': sum(int(e[1], 16) in members for members in groups
                                    for pc in members for e in EDGE.finditer(bodies[pc][3])),
              'source_sha256': hashlib.sha256(source.encode()).hexdigest()}
    return '#include "spu_job_merge.h"\n' + rewritten, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--module', choices=HASHES, required=True)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--generate', type=Path, required=True)
    args = parser.parse_args()
    original = (ROOT / 'port/spu' / args.module / 'spu_recomp.c').read_text()
    if hashlib.sha256(original.encode()).hexdigest() != HASHES[args.module]:
        raise SystemExit('WWS source changed: re-audit block fusion before building.')
    source, report = generate(args.input.read_text(), original, args.module)
    if not report['internal_edges']:
        raise SystemExit('No mergeable edges; refusing an ineffective candidate.')
    args.generate.write_text(source)
    args.generate.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f"WWS merge {args.module}: {len(report['groups'])} regions, "
          f"{report['merged_blocks']} blocks, {report['internal_edges']} internal edges")


if __name__ == '__main__':
    main()
