#!/usr/bin/env python3
"""Add iOS wait/wake scheduling to an audited guest counting semaphore.

No guest memory operation, CAS, register assignment or branch is removed.
Only a generated per-SDK copy is modified. Runtime switch defaults to off.
"""
import argparse
import hashlib
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'port/recompiled/ppu_recomp_083.cpp'
HASHES = {'00B9FBE4': '0146d6aed7dfdbf8cc7adec66cc50cb31817608e373dbbeeb1aff646c0265a1f', '00B9FC50': 'acedea7f70d0f6f8b9de957fc21618d4d5f54e913a2194ff7d3eff2be5d94e35'}


def bodies(source):
    result = {}
    for pc, digest in HASHES.items():
        m = re.search(r'^void func_' + pc + r'\(ppu_context\* ctx\) \{.*?^}', source, re.M | re.S)
        if not m or hashlib.sha256(m[0].encode()).hexdigest() != digest:
            raise ValueError('Counting semaphore changed; re-audit ' + pc)
        result[pc] = m[0]
    return result


def generate(source):
    original = bodies(source)
    for pc, body in original.items():
        if pc == '00B9FBE4':
            needle = '        ctx->gpr[9] = vm_read32(ctx->gpr[3] + 0x4);\nloc_00B9FC2C:'
            replacement = '        ms_counting_wait_park(ctx);\n' + needle
        else:
            needle = '        if ((--ctx->ctr != 0)) goto loc_00B9FC68;'
            replacement = '        ms_counting_wait_wake(ctx);\n' + needle
        assert body.count(needle) == 1
        source = source.replace(body, body.replace(needle, replacement), 1)
    return '#include "ppu_counting_wait.h"\n' + source


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, default=SOURCE)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if args.output.resolve() in (SOURCE.resolve(), args.input.resolve()):
        parser.error('Refusing to overwrite the original lift')
    result = generate(args.input.read_text())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text() != result:
        args.output.write_text(result)
    print('Counting semaphore: original wait/CAS/post preserved; iOS timed wait and wake added')
