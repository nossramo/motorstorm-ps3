#!/usr/bin/env python3
"""Re-lift the known missed Havok switch without regenerating unrelated code.

The fix is in the generic PPU lifter. This narrow integration verifies that
the old emitter reproduces every existing function byte for byte before
replacing it; it does not inject guessed branch targets or new game logic.
"""
import hashlib
from pathlib import Path
import re
import sys
from collections import Counter

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'vendor/ps3recomp/tools'))
from elf_parser import ELFFile
from ppu_disasm import decode
from ppu_lifter import PPULifter, discover_jump_tables

path = ROOT / 'game/PS3_GAME/USRDIR/EBOOT.elf'
assert hashlib.sha256(path.read_bytes()).hexdigest() == '983baf06f5357a66b3f97fd5ac20252b50a4161b6373ba290c8206a833250c0d'
elf = ELFFile(str(path)); elf.load()
segments = [(p.p_vaddr, elf.get_segment_data(i))
            for i, p in enumerate(elf.program_headers) if p.p_type == 1]

def read(address):
    for base, data in segments:
        if base <= address and address + 4 <= base + len(data):
            return int.from_bytes(data[address-base:address-base+4], 'big')
    return None

lo, hi = 0x10200, 0xDCA5B4
toc_counts = Counter()
for base, data in segments:
    if base <= lo < base + len(data):
        continue
    for offset in range(0, len(data)-7, 4):
        code = int.from_bytes(data[offset:offset+4], 'big')
        toc = int.from_bytes(data[offset+4:offset+8], 'big')
        if lo <= code < hi and not code & 3 and base <= toc < base+len(data) and not toc & 3:
            toc_counts[toc] += 1
tocs = [read(elf.elf_header.e_entry+4)] + [t for t, n in toc_counts.most_common(8) if n >= 64]
start, end = 0xD7A994, 0xD7B9E4
instructions = [decode(read(a), a) for a in range(start, end, 4)]
tables = discover_jump_tables(instructions, read, tocs, lo, hi, func_starts=[start])
assert set(tables) == {0xD7AADC}, tables
# Independently check all 25 offsets from the original switch table, including
# repeated cases. This catches accidental partial recovery or wrong TOC choice.
offsets = [read(0xD7AAE0 + i*4) for i in range(25)]
expected = sorted({0xD7AAE0 + (v if v < 0x80000000 else v-0x100000000) for v in offsets})
assert tables[0xD7AADC] == expected and len(expected) == 19
assert all(start <= a < end for a in expected)

def emit(entry, jump_tables):
    lifter = PPULifter(); lifter.code_hi = hi; lifter.jump_tables = jump_tables
    func = lifter.lift_function(instructions, entry, end)
    return '\n'.join(lifter._function_def_lines(func, {entry: func}, [entry], {entry: 0}))

source = ROOT / 'port/recompiled/ppu_recomp_094.cpp'
old_source = source.read_text(); new_source = old_source
for entry in (start, 0xD7AA34, 0xD7AAA0, 0xD7AAC0):
    pattern = r'void func_' + f'{entry:08X}' + r'\(ppu_context\* ctx\) \{.*?\n\}\n'
    match = re.search(pattern, new_source, re.S)
    assert match is not None
    before, after = emit(entry, {}), emit(entry, tables)
    assert match.group() in (before, after), f'Unrecognized local changes at {entry:08X}'
    for target in expected:
        if target >= entry:
            assert f'goto loc_{target:08X}' in after
            assert f'loc_{target:08X}:' in after
        else:
            assert f'g_trampoline_fn = (void(*)(void*))func_{target:08X}' in after
    new_source = new_source[:match.start()] + after + new_source[match.end():]
    print(f'func_{entry:08X}: {len(expected)} unique cases recovered from 25 table entries')
# Tail-entry fragments can jump to earlier cases. Materialize those entries
# exactly as the lifter's mid-function pass does; their epilogues recover the
# original caller's registers from the existing guest stack frame.
table_path = ROOT / 'port/recompiled/ppu_recomp_table.cpp'
old_table = table_path.read_text(); new_table = old_table
for entry in (0xD7AA50, 0xD7AAB8):
    definition = emit(entry, tables)
    assert 'uint64_t _cs_31 = vm_read64(ctx->gpr[1] + -0x8);' in definition
    prototype = f'void func_{entry:08X}(ppu_context* ctx);\n'
    if definition not in new_source:
        assert f'void func_{entry:08X}(ppu_context* ctx) {{' not in new_source
        new_source = new_source.replace('#include "ppu_recomp.h"\n',
                                        '#include "ppu_recomp.h"\n' + prototype, 1)
        new_source += '\n' + definition
    row = f'    {{ 0x{entry:08X}ULL, func_{entry:08X}, "func_{entry:08X}" }},\n'
    if row not in new_table:
        new_table = new_table.replace('#include "ppu_recomp.h"\n',
                                      '#include "ppu_recomp.h"\n' + prototype, 1)
        new_table = new_table.replace('    { 0, NULL, NULL }', row + '    { 0, NULL, NULL }')
        new_table = re.sub(r'function_table_count = (\d+);',
                           lambda m: f'function_table_count = {int(m[1])+1};', new_table)
if new_source != old_source:
    source.write_text(new_source)
if new_table != old_table:
    table_path.write_text(new_table)
print('Havok switch integration verified; unrelated generated functions preserved.')
