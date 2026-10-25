#!/usr/bin/env python3
"""Install the reversible whole-batch adapter in the checked WWS image 205."""
import argparse
from pathlib import Path
import re
import specialize_native_geometry as geometry

ENTRY = 0x88D8



def audit_liveness(source, starts, registers):
    """Fail closed on reads of changed volatile values in all five callers.

    Follow direct calls, both sides of conditional transfers and constant
    indirect continuations until overwritten or the enclosing ABI return.
    The retained routine is void; r11..r41 are caller-clobbered at that return.
    """
    f = {pc: [s.strip() for s in v[3].splitlines() if s.strip()] for pc, v in geometry.functions(source).items()}
    call = re.compile('host_depth\\+\\+; \\w+_spu_func_([0-9A-F]+)\\(ctx\\);')
    edge = re.compile('g_spu_trampoline_fn = \\w+_spu_func_([0-9A-F]+)')
    for root in starts:
        todo = [(root[0], root[1], tuple(registers), ())]
        seen = set()
        ends = set()
        while todo:
            pc, i, pending, stack = todo.pop()
            key = (pc, i, pending, stack)
            if key in seen or not pending:
                continue
            seen.add(key)
            assert len(seen) < 100000
            lines = f[pc]
            assert i < len(lines), (hex(pc), i)
            s = lines[i]
            m = re.match('ctx->gpr\\[(\\d+)\\] = (.*)', s)
            rhs = m[2] if m else s
            reads = set(map(int, re.findall('ctx->gpr\\[(\\d+)\\]', rhs)))
            bad = reads & set(pending)
            if bad:
                raise ValueError((root, hex(pc), i, bad, s))
            if m:
                pending = tuple((x for x in pending if x != int(m[1])))
            if not pending:
                continue
            if 'SPU_RET(ctx)' in s:
                if stack:
                    p, j = stack[-1]
                    todo.append((p, j, pending, stack[:-1]))
                else:
                    ends.add(('abi-return', pc, pending))
                continue
            m = call.search(s)
            if m:
                todo.append((int(m[1], 16), 0, pending, stack + ((pc, i + 1),)))
                continue
            m = edge.search(s)
            if m:
                todo.append((int(m[1], 16), 0, pending, stack))
                if s.startswith('if '):
                    todo.append((pc, i + 1, pending, stack))
                continue
            m = re.search('goto (loc_\\w+)', s)
            if m:
                label = m[1] + ':'
                j = lines.index(label)
                todo.append((pc, j + 1, pending, stack))
                if s.startswith('if '):
                    todo.append((pc, i + 1, pending, stack))
                continue
            if 'spu_indirect_branch' in s:
                m = re.search('pc = ctx->gpr\\[(\\d+)\\]', s)
                assert m, s
                target = None
                for prior in reversed(lines[:i]):
                    if f'ctx->gpr[{m[1]}] =' in prior:
                        a = re.search('spu_ila\\(0x([A-Fa-f0-9]+)\\)', prior)
                        if a:
                            target = int(a[1], 16)
                        break
                if target in f:
                    todo.append((target, 0, pending, stack))
                    continue
                raise ValueError(('indirect', root, hex(pc), pending, s))
            todo.append((pc, i + 1, pending, stack))


def audit_callers(source):
    starts = []
    for pc, value in geometry.functions(source).items():
        lines = [s.strip() for s in value[3].splitlines() if s.strip()]
        for i, line in enumerate(lines):
            if re.search(r'host_depth\+\+; \w+_spu_func_00008770\(ctx\);', line):
                starts.append((pc, i + 1))
    if len(starts) != 5:
        raise ValueError('WWS classification caller set changed')
    audit_liveness(source, starts, range(11, 42))


def audit_sort(source):
    # r1 is balanced, r87 is explicitly reconstructed by the adapter. Every
    # other value written by the original sort must die before any later read.
    audit_liveness(source, [(0xF010, 0)], [0, *range(2, 29), *range(80, 87)])

    # Prove the pairwise plane loop has no visible side effect, and every GPR
    # it writes dies before the enclosing function's return. Fail closed if a
    # new source introduces stores, channels, calls, exits or helper effects.
    functions = geometry.functions(source)
    todo, seen, written = [0xF044], set(), set()
    from specialize_job_merge import PURE
    while todo:
        pc = todo.pop()
        if pc in seen or pc == 0xF2F8:
            continue
        seen.add(pc)
        body = functions[pc][3]
        helpers = set(re.findall(r'\bspu_(\w+)\(', body))
        if helpers - (PURE - {'ls_write128'}) or 'host_depth++' in body or 'SPU_RET' in body:
            raise ValueError(f'Observable pairwise occluder effect at {pc:x}')
        if re.search(r'ctx->(?!gpr\[|pc\b)', body):
            raise ValueError(f'Unknown pairwise context effect at {pc:x}')
        written.update(map(int, re.findall(r'ctx->gpr\[(\d+)\] =', body)))
        todo.extend(int(m[1], 16) for m in re.finditer(r'g_spu_trampoline_fn = \w+_spu_func_([0-9A-F]+)', body))
    if len(seen) != 7:
        raise ValueError('Pairwise occluder loop changed')
    audit_liveness(source, [(0xF2F8, 0)], written)
    starts = []
    for pc, value in functions.items():
        lines = [s.strip() for s in value[3].splitlines() if s.strip()]
        for i, line in enumerate(lines):
            if re.search(r'host_depth\+\+; \w+_spu_func_0000D6F0\(ctx\);', line):
                starts.append((pc, i + 1))
    if len(starts) != 1:
        raise ValueError('Whole occluder caller changed')
    audit_liveness(source, starts, range(2, 29))


def generate(source):
    checked = geometry.checked_source()
    audit_callers(checked)
    audit_sort(checked)
    body, name, _, inner = geometry.functions(source)[ENTRY]
    replacement = (f'void {name}(spu_context* ctx) {{\n'
                   f'    ms_wws_chain_capture(ctx, 0x{ENTRY:X});\n'
                   '    if (ms_wws_chain_try(ctx)) {\n'
                   '        ctx->pc = 0x8BE0;\n'
                   f'        g_spu_trampoline_fn = {geometry.MODULE}_spu_func_00008BE0;\n'
                   '        return;\n'
                   '    }\n' + inner + '}')
    if source.count(body) != 1:
        raise ValueError('Ambiguous WWS chain entry')
    source = source.replace(body, replacement, 1)
    body, name, _, inner = geometry.functions(source)[0xDCBC]
    replacement = (f'void {name}(spu_context* ctx) {{\n'
                   '    ms_wws_chain_capture(ctx, 0xDCBC);\n'
                   '    int native = ms_wws_sort_try(ctx);\n'
                   '    if (native) {\n'
                   '        ctx->pc = native == 2 ? 0xF530 : 0xF010;\n'
                   f'        g_spu_trampoline_fn = native == 2 ? {geometry.MODULE}_spu_func_0000F530 : {geometry.MODULE}_spu_func_0000F010;\n'
                   '        return;\n'
                   '    }\n' + inner + '}')
    if source.count(body) != 1:
        raise ValueError('Ambiguous WWS sort entry')
    return '#include "wws_native_chain.h"\n' + source.replace(body, replacement, 1)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    args = p.parse_args()
    if args.input.resolve() == args.output.resolve():
        p.error('The retained input must not be overwritten')
    result = generate(args.input.read_text())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text() != result:
        args.output.write_text(result)


if __name__ == '__main__':
    main()
