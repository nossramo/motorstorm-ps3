#!/usr/bin/env python3
"""Install native box/plane predicates at two complete WWS leaf entries.

The original lift, existing optimizations and fallback entry points survive
byte-for-byte. Generated game code remains in the per-SDK ignored build tree.
"""
import argparse
import hashlib
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]

MODULE = 'jobmod_be896a6374ba_e12230'
SOURCE = ROOT / 'port/spu' / MODULE / 'spu_recomp.c'
SHA256 = '2e5ea1971e8dee9edb493a4f96182a719dffae588ffeff15a378aa1d3f9cf7f9'
ENTRIES = {0x8120: 0, 0x8380: 1}
START = re.compile(r'^void (jobmod_\w+_spu_func_([0-9A-F]{8}))\(spu_context\* ctx\) \{\n', re.M)
TOKENS = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)


def definition_end(source, brace):
    depth = 0
    for token in TOKENS.finditer(source, brace):
        if token[0] == '{':
            depth += 1
        elif token[0] == '}':
            depth -= 1
            if depth == 0:
                return token.end()
    raise ValueError('Unclosed generated function')


def functions(source):
    result = {}
    for m in START.finditer(source):
        end = definition_end(source, m.end()-2)
        pc = int(m[2],16)
        if pc in result:
            raise ValueError('Duplicate generated entry')
        result[pc] = (source[m.start():end],m[1],m[2],source[m.end():end-1])
    return result


def checked_source():
    source = SOURCE.read_text()
    if hashlib.sha256(source.encode()).hexdigest() != SHA256:
        raise ValueError('Geometry module changed; re-audit the native contract')
    return source


def generate(source):
    checked_source()
    bodies = functions(source)
    out = source
    for pc,kind in ENTRIES.items():
        if pc not in bodies:
            raise ValueError('Missing or renamed geometry entry')
        m = bodies[pc]
        replacement = (f'void {m[1]}(spu_context* ctx) {{\n'
                '    unsigned geometry_mode = ms_geometry_mode();\n'
                f'    if (geometry_mode & 2) ms_geometry_capture(ctx, {kind});\n'
                f'    if ((geometry_mode & 1) && ms_geometry_try(ctx, {kind})) {{ SPU_RET(ctx); }}\n'
                + m[3] + '}')
        if out.count(m[0]) != 1:
            raise ValueError('Ambiguous generated entry')
        out = out.replace(m[0], replacement, 1)
    return '#include "native_geometry.h"\n' + out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.resolve() in (SOURCE.resolve(), args.input.resolve()):
        parser.error('Refusing to overwrite an input')
    out = generate(args.input.read_text())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(out)
    print('Native geometry: two complete box/plane predicates; originals retained')


if __name__ == '__main__':
    main()
