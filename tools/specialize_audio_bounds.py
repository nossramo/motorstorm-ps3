"""Prove a magnitude bound for pure arithmetic runs in checked audio loops.

No reassociation or approximation. A dynamic check covers every live input;
integer exponent bounds prove every intermediate below 2**125. Out-of-range
inputs retain the existing exact extended-range implementation. Callers must
already have entered the checked RTZ scope. No LS access or branch is moved.
"""
import re

ASSIGN = re.compile(
    r'\s*audio_registers\[(\d+)\] = ms_audio_compact_(fa|fs|fm|fma|fms)\('
    r'(audio_registers\[\d+\], audio_registers\[\d+\](?:, audio_registers\[\d+\])?), audio_fpcr\);')


def bounded_run(lines):
    bounds, inputs = {}, set()
    for line in lines:
        match = ASSIGN.fullmatch(line)
        destination, op, args = match.groups()
        operands = [int(x) for x in re.findall(r'\[(\d+)\]', args)]
        assert len(operands) == (3 if op in ('fma', 'fms') else 2)
        for register in operands:
            if register not in bounds:
                bounds[register] = 16  # dynamic guard: abs(value) <= 65536
                inputs.add(register)
        x, y = (bounds[r] for r in operands[:2])
        exponent = max(x, y) + 1 if op in ('fa', 'fs') else x + y
        if len(operands) == 3:
            exponent = max(exponent, bounds[operands[2]]) + 1
        if exponent > 125:
            return lines  # no proven range: leave the entire run unchanged
        bounds[int(destination)] = exponent
    result = ['        {', '            uint32x4_t audio_magnitude = vdupq_n_u32(0);']
    for register in sorted(inputs):
        result.append(f'            audio_magnitude = vmaxq_u32(audio_magnitude, '
                      f'vandq_u32(vld1q_u32(audio_registers[{register}]._u32), vdupq_n_u32(0x7fffffffu)));')
    result += ['            if (vmaxvq_u32(audio_magnitude) <= 0x47800000u) {']
    result += [line.replace('ms_audio_compact_', 'ms_audio_bounded_') for line in lines]
    result += ['            } else {', *lines, '            }', '        }']
    return result


def specialize(body, entry):
    body = body.replace(f'ms_audio_direct_ls_{entry:X}(', f'ms_audio_bounded_{entry:X}(', 1)
    lines, result, run = body.splitlines(), [], []
    for line in lines + [None]:
        if line is not None and ASSIGN.fullmatch(line):
            run.append(line)
            continue
        result.extend(bounded_run(run) if len(run) >= 8 else run)
        run = []
        if line is not None:
            result.append(line)
    return '\n'.join(result)


def test_source(source, destination):
    """Emit differential cases from the actual generated candidate groups."""
    output, cases = [], []
    for entry in (0x5238, 0x5670, 0x5848, 0x98d8):
        start = source.index(f'static void ms_audio_direct_ls_{entry:X}(')
        end = source.index('\n}', start)
        run = []
        for line in source[start:end].splitlines() + ['END']:
            if ASSIGN.fullmatch(line):
                run.append(line)
                continue
            if len(run) >= 8 and bounded_run(run) != run:
                index = len(cases)
                for name, lines in [('reference', run), ('bounded', bounded_run(run))]:
                    output.append(f'static void group_{name}_{index}(u128* audio_registers, uint64_t audio_fpcr) {{\n'
                                  + '\n'.join(lines) + '\n}\n')
                cases.append(f'{{group_reference_{index}, group_bounded_{index}}}')
            run = []
    assert cases
    output.append('static void (*audio_groups[][2])(u128*, uint64_t) = {' + ','.join(cases) + '};\n')
    destination.write_text(''.join(output))


if __name__ == '__main__':
    import argparse
    from pathlib import Path
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--test-source', type=Path, required=True)
    args = parser.parse_args()
    test_source(args.input.read_text(), args.test_source)
