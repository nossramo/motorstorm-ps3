#!/usr/bin/env python3
"""Keep registers native across checked iOS audio and fused WWS regions.

No guest instruction is deleted/reordered and no arithmetic is changed.
Original bodies remain selectable. The input is the output of the existing
hash-checked audio / WWS generators; generated game code stays in build/.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

FUNCTION = re.compile(r'^(?P<signature>(?:static )?(?:__attribute__\(\(noinline\)\) )?void '
    r'(?P<name>\w+)\(spu_context\* ctx(?P<extra>, uint32_t entry)?\)) \{\n', re.M)
TOKENS = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
REGISTER = re.compile(r'(?:ctx->gpr|audio_registers)\[(\d+)\]')
ASSIGNMENT = re.compile(r'(?:ctx->gpr|audio_registers)\[(\d+)\]\s*=(?!=)')
SAVE = 'memcpy(ctx->gpr, audio_registers, sizeof audio_registers);'
LOAD = 'memcpy(audio_registers, ctx->gpr, sizeof audio_registers);'
CONTEXT_PURE = {'spu_ls_read128', 'spu_ls_write128', 'ms_audio_ls_read128',
                'ms_audio_ls_write128', 'ms_job_merge_hop', 'SPU_RET'}
CONTEXT_BOUNDARY = {'spu_rdch', 'spu_wrch', 'spu_rchcnt', 'spu_drain_call',
                    'spu_img_restore', 'spu_indirect_branch', 'spu_stop', 'spu_halt'}


def functions(source):
    for match in FUNCTION.finditer(source):
        depth = 1
        for token in TOKENS.finditer(source, match.end()):
            if token[0] == '{':
                depth += 1
            elif token[0] == '}':
                depth -= 1
                if not depth:
                    yield dict(match.groupdict(), start=match.start(), end=token.end(),
                               body=source[match.end():token.start()], text=source[match.start():token.end()])
                    break
        else:
            raise ValueError('Unclosed function: ' + match['name'])


def rewrite(body):
    # Existing audio variants have explicit publication/reload around calls.
    # Remove those array copies and use the same boundaries below instead.
    text = body.replace('    u128 audio_registers[128];\n', '')
    text = text.replace(SAVE, '').replace(LOAD, '')
    registers = sorted({int(m[1]) for m in REGISTER.finditer(text)})
    written = sorted({int(m[1]) for m in ASSIGNMENT.finditer(text)})
    if len(written) < 8:
        return None, 'small region'
    if re.search(r'&\s*(?:ctx->gpr|audio_registers)\[', text):
        return None, 'escaped register address'
    residual = REGISTER.sub('REGISTER', text)
    if 'ctx->gpr' in residual or 'audio_registers' in residual:
        raise ValueError('Dynamic or unknown register access')
    if re.search(r'REGISTER\.\w+(?:\[[^]\n]+\])?\s*(?:=(?!=)|[+*/&|^-]=|\+\+|--)', residual):
        raise ValueError('Partial register assignment needs a separate audit')
    # Only numeric whole-register assignments exist in this lifter grammar.
    if re.search(r'REGISTER\s*(?:\+\+|--|[+*/&|^-]=)', residual):
        raise ValueError('Unexpected register mutation')
    save = ' '.join(f'ctx->gpr[{r}] = sr{r};' for r in written)
    reload = ' '.join(f'sr{r} = ctx->gpr[{r}];' for r in registers)
    output = ['    ' + ' '.join(f'u128 sr{r} = ctx->gpr[{r}];' for r in registers)]
    boundaries = 0
    for line in text.splitlines():
        if not line.strip():
            continue
        helpers = set(re.findall(r'\b(\w+)\(ctx[,)]', line))
        boundary = helpers - CONTEXT_PURE
        unknown = {h for h in boundary if h not in CONTEXT_BOUNDARY and
                   not re.fullmatch(r'spu_[0-9a-f]+_spu_func_[0-9A-F]+', h)}
        if unknown:
            return None, 'context observer: ' + ','.join(sorted(unknown))
        if boundary:
            # Keep the complete original call statement, including link
            # assignments, callback results, host depth and image restore.
            # No conditional call/return or compound loop enters this rule.
            if re.search(r'\b(if|for|while|return)\b|SPU_RET', line):
                return None, 'compound context boundary'
            if 'audio_registers' in line:
                raise ValueError('Unpublished local register at callback')
            output += ['    ' + save, line, '    ' + reload]
            boundaries += 1
            continue
        # LS helpers only touch LS with diagnostics compiled out. The fused
        # hop helper only inspects dispatch/IRQ fields and increments steps.
        context = re.sub(r'ctx->(?:pc|status|stop_code)\b', 'FIELD', REGISTER.sub('REGISTER', line))
        context = re.sub(r'\b(?:' + '|'.join(sorted(CONTEXT_PURE)) + r')\(ctx(?=[,)])', 'HELPER(ARG', context)
        if re.search(r'\bctx\b', context):
            return None, 'unreviewed context use'
        line = REGISTER.sub(lambda m: 'sr' + m[1], line)
        # do/while keeps an unbraced conditional return conditional.
        line = line.replace('return;', 'do { ' + save + ' return; } while (0);')
        line = line.replace('SPU_RET(ctx);', 'do { ' + save + ' SPU_RET(ctx); } while (0);')
        output.append(line)
    output.append('    ' + save)
    return '\n'.join(output) + '\n', {'registers':len(registers), 'written':len(written), 'boundaries':boundaries}


def generate(source, kind):
    selected, skipped = {}, {}
    mask = 1 if kind == 'audio' else 2
    def transform(m):
        name = m['name']
        if kind == 'jobs':
            if not name.startswith('ms_merged_jobmod_'):
                return m['text']
        elif not (re.fullmatch(r'spu_[0-9a-f]+_spu_func_[0-9A-F]+', name) or
                  re.fullmatch(r'ms_audio_bounded_[0-9A-F]+', name)):
            return m['text']
        # Wrappers already select validated arithmetic variants. Transform
        # the bounded bodies, not these tiny dispatch wrappers.
        if 'ms_audio_worker_capture(' in m['body']:
            return m['text']
        native, report = rewrite(m['body'])
        if native is None:
            skipped[name] = report
            return m['text']
        selected[name] = report
        signature = m['signature']
        internal = signature if signature.startswith('static ') else 'static ' + signature
        old = internal.replace(name + '(', 'ms_pipeline_original_' + name + '(', 1)
        new = internal.replace(name + '(', 'ms_pipeline_native_' + name + '(', 1)
        args = 'ctx, entry' if m['extra'] else 'ctx'
        return (old + ' {\n' + m['body'] + '}\n\n' + new + ' {\n' + native + '}\n\n' +
            signature + ' {\n' + f'    if (ms_spu_pipeline_enabled({mask})) ms_pipeline_native_{name}({args});\n' +
            f'    else ms_pipeline_original_{name}({args});\n}}')
    parts, end = [], 0
    for fn in functions(source):
        parts.extend((source[end:fn['start']], transform(fn)))
        end = fn['end']
    output = ''.join(parts) + source[end:]
    if not selected:
        raise ValueError('No native register regions found')
    return '#include "spu_audio_pipeline.h"\n' + output, {
        'kind':kind, 'input_sha256':hashlib.sha256(source.encode()).hexdigest(),
        'selected':selected, 'skipped':skipped}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--generate', type=Path, required=True)
    parser.add_argument('--kind', choices=['audio', 'jobs'], required=True)
    args = parser.parse_args()
    if args.input.resolve() == args.generate.resolve():
        parser.error('Refusing to overwrite the input')
    output, report = generate(args.input.read_text(), args.kind)
    args.generate.parent.mkdir(parents=True, exist_ok=True)
    args.generate.write_text(output)
    args.generate.with_suffix('.json').write_text(json.dumps(report, indent=2)+'\n')
    print(f"SPU native registers: {args.kind}, {len(report['selected'])} regions; {len(report['skipped'])} unchanged")


if __name__ == '__main__':
    main()
