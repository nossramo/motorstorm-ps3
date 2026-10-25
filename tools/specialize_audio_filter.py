#!/usr/bin/env python3
"""Generate iOS audio block/legacy variants, retaining the original DSP path."""
import argparse
import hashlib
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'port/spu/motorstorm_audio_32770/spu_recomp.c'
SHA256 = '545d3e6632a1261420a560bbeade99b2927b1747ade19b1546f4828a9e1c17ad'
START = 'loc_00032900:\n'
END = 'loc_00032AD8:\n'
SECOND_START = 'loc_00032B88:\n'
SECOND_END = 'loc_00032D68:\n'
ENTRY = 'void ms_audio_32770_spu_func_00032770(spu_context* ctx) {\n'


def checked_source():
    data = SOURCE.read_bytes()
    if hashlib.sha256(data).hexdigest() != SHA256:
        raise ValueError('Audio overlay changed: re-audit the native filter before building.')
    source = data.decode()
    assert source.count(START) == source.count(END) == 1
    return source


def loop_body(source):
    loop = source[source.index(START):source.index(END)]
    written, live = set(), set()
    for line in loop.splitlines():
        assignment = re.match(r'\s*ctx->gpr\[(\d+)\] = (.*)', line)
        rhs = assignment[2] if assignment else line
        live |= set(map(int, re.findall(r'ctx->gpr\[(\d+)\]', rhs))) - written
        if assignment:
            written.add(int(assignment[1]))
    assert live == {14, 15, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39}
    assert live & written == {14, 15, 29, 30, 36, 38, 39}
    return loop


def generate_legacy(source):
    loop_body(source)  # Keep the live-register audit tied to the checked body.
    return '#include "spu_audio_filter.h"\n' + source.replace(START,
        '        if (ms_audio_filter_enabled()) ms_audio_filter_prefix(ctx);\n' + START)


def second_loop_body(source):
    source = source[source.index(ENTRY):source.index('\n}\n', source.index(ENTRY))]
    assert source.count(SECOND_START) == source.count(SECOND_END) == 1
    return source[source.index(SECOND_START):source.index(SECOND_END)]


def generate(source):
    legacy = generate_legacy(source)
    # A complete entry variant keeps DMA/state publication and return order.
    # Only the two recurrence regions cross the native buffer/state API.
    start = source.index(ENTRY)
    end = source.index('\n}\n', start) + 3
    native = source[start:end].replace(ENTRY, ENTRY.replace(
        'void ms_audio_32770_spu_func_00032770', 'static void ms_audio_block_entry'))
    second_loop_body(source)
    native = native.replace(START, '        ms_audio_block_prepare(ctx, 0);\n' + START)
    native = native.replace(SECOND_START, '        ms_audio_block_prepare(ctx, 1);\n' + SECOND_START)
    wrapper = '''void ms_audio_32770_spu_func_00032770(spu_context* ctx) {
    if (ms_audio_block_enabled()) ms_audio_block_entry(ctx);
    else ms_audio_legacy_entry(ctx);
}
'''
    legacy = legacy.replace(ENTRY, ENTRY.replace(
        'void ms_audio_32770_spu_func_00032770', 'static void ms_audio_legacy_entry'))
    return '#include "spu_audio_block.h"\n' + legacy + '\n' + native + '\n' + wrapper


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generate', type=Path, required=True)
    args = parser.parse_args()
    args.generate.write_text(generate(checked_source()))


if __name__ == '__main__':
    main()
