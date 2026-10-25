#!/usr/bin/env python3
"""Differential tests of the exact four production PPU bodies and the pass.

The bounded trace test mocks guest memory and callees to explore control flow;
it compares every memory operation and the complete context at opaque calls
and normal returns. Callees deliberately clobber nonvolatile registers too.
The copy test separately uses real production memory helpers and complete
memory/context comparisons. Host timings are not iPhone FPS measurements.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
import specialize_ppu_locals as p

MOCK = r'''
void ms_step(unsigned);
uint64_t ms_read(unsigned, uint64_t);
void ms_write(unsigned, uint64_t, uint64_t);
void ms_raw_load(void*, uint64_t);
void ms_raw_store(uint64_t, const void*);
uint64_t ms_timebase();
#undef vm_read8
#undef vm_read16
#undef vm_read32
#undef vm_read64
#define vm_read8(a) ((uint8_t)ms_read(1,a))
#define vm_read16(a) ((uint16_t)ms_read(2,a))
#define vm_read32(a) ((uint32_t)ms_read(4,a))
#define vm_read64(a) ms_read(8,a)
#define vm_write8(a,v) ms_write(1,a,(uint8_t)(v))
#define vm_write32(a,v) ms_write(4,a,(uint32_t)(v))
#define vm_write64(a,v) ms_write(8,a,(uint64_t)(v))
#define vm_raw_ld16(d,a) ms_raw_load(d,a)
#define vm_raw_st16(a,s) ms_raw_store(a,s)
#undef VM_RAW_ST
#define VM_RAW_ST(T,a,v) do { T ms_v=(T)(v); for (unsigned ms_i=0; ms_i<sizeof(T);++ms_i) ms_write(1,(a)+ms_i,((uint8_t*)&ms_v)[ms_i]); } while(0)
#define ppu_timebase_now ms_timebase
'''

SYNTHETIC = '''        uint64_t _cs_31 = ctx->gpr[31];
        ctx->gpr[31] = ctx->gpr[3];
        ctx->gpr[4] += 7;
        ctx->ctr = (ctx->gpr[5] & 7) + 1;
loc_00001000:
        ctx->gpr[4] += ctx->gpr[31];
        ctx->fpr[3] = ppu_fadd(ctx->fpr[3], ctx->fpr[1]);
        { uint64_t* d = (uint64_t*)&ctx->vr[3]; d[0] ^= ctx->gpr[4]; }
        if (--ctx->ctr) goto loc_00001000;
        if (ctx->gpr[6] & 1) return;
        if (ctx->gpr[6] & 2) { func_00002000(ctx); DRAIN_TRAMPOLINE(ctx); }
        ctx->gpr[7] = ctx->gpr[31] + ctx->gpr[4];
        switch (ctx->gpr[6] & 12) { case 0: goto loc_00001010; case 4: ctx->gpr[9] = 42; break; default: ctx->gpr[10]++; }
        ctx->gpr[8] = ppc_srad(&ctx->xer, ctx->gpr[7], 3);
loc_00001010:
        if (ctx->gpr[6] & 16) { g_trampoline_fn = (void(*)(void*))func_00002000; return; }
        ctx->gpr[31] = _cs_31;
        lv2_syscall(ctx);
        ctx->gpr[7] += ctx->gpr[31];
        return;
'''


def generate_tests(work):
    entries, calls = [], {'00002000'}
    for unit, selected in p.FUNCTIONS.items():
        source = (p.ROOT / f'port/recompiled/ppu_recomp_{unit}.cpp').read_text()
        p.generate(source, unit)  # Production guards must also pass.
        prelude = source[:p.FUNCTION.search(source).start()]
        bodies = [(address, p.body(source, address)) for address in selected]
        if unit == '091':
            bodies.append(('00001000', SYNTHETIC))
        code = prelude + '\n' + MOCK + '\nvoid func_00002000(ppu_context*);\n'
        for address, original in bodies:
            calls.update(re.findall(r'\bfunc_([0-9A-F]{8})\b', original))
            optimized = p.rewrite(original, address, {})
            for name, body in [('reference', original), ('candidate', optimized)]:
                # Test-only instruction budget, identical in both forms.
                body = re.sub(r'^(loc_([0-9A-F]{8}):)', r'\1 ms_step(0x\2);', body, flags=re.M)
                code += f'void {name}_{address}(ppu_context* ctx) {{\n{body}}}\n'
            entries.append(address)
        (work / f'trace_{unit}.cpp').write_text(code)
    (work / 'entries.inc').write_text('\n'.join(
        f'void reference_{a}(ppu_context*); void candidate_{a}(ppu_context*);' for a in entries)
        + '\nstatic const Entry entries[] = {\n' + '\n'.join(
        f'{{0x{a}, reference_{a}, candidate_{a}}},' for a in entries) + '\n};\n'
        + '\n'.join(f'void func_{a}(ppu_context* c) {{ ms_call(c, 0x{a}); }}' for a in sorted(calls)))
    # Production copy loop, no mocked memory and no injected step calls.
    source = (p.ROOT / 'port/recompiled/ppu_recomp_091.cpp').read_text()
    original = p.body(source, '00D18624')
    (work / 'copy.inc').write_text(source[:p.FUNCTION.search(source).start()]
        + '\nstatic __attribute__((noinline)) void reference_copy(ppu_context* ctx) {\n'
        + original + '}\nstatic __attribute__((noinline)) void candidate_copy(ppu_context* ctx) {\n'
        + p.rewrite(original, '00D18624') + '}\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--thinlto', action='store_true')
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    os.environ.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
    with tempfile.TemporaryDirectory(prefix='motorstorm-ppu-locals-') as folder:
        work = args.output or Path(folder)
        work.mkdir(parents=True, exist_ok=True)
        generate_tests(work)
        common = ['clang++', '-O2', '-std=gnu++20', '-fno-strict-aliasing', '-fwrapv',
                  '-ffp-contract=off', '-UNDEBUG', '-w', '-DMOTORSTORM_NEON_ARITHMETIC=1',
                  '-include', str(p.ROOT / 'port/ppu_vmx_neon.h')]
        if args.sanitize:
            common += ['-g', '-fsanitize=address,undefined']
        if args.thinlto:
            common += ['-flto=thin']
        for include in (work, p.ROOT/'port/recompiled', p.ROOT/'port',
                        p.ROOT/'vendor/ps3recomp/runtime/ppu', p.ROOT/'vendor/ps3recomp/include'):
            common += ['-I' + str(include)]
        for name, sources in [('trace', sorted(work.glob('trace_*.cpp'))), ('copy', [])]:
            binary = work / name
            subprocess.run([*common, *map(str, sources), str(p.ROOT / f'port/tests/test_ppu_locals_{name}.cpp'),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
    # Fail closed when an input changes or an opaque context observer appears.
    try:
        p.rewrite('        observer(ctx);\n        return;\n', '00001000')
    except ValueError:
        pass
    else:
        raise AssertionError('Unknown observer accepted')
    source = (p.ROOT / 'port/recompiled/ppu_recomp_091.cpp').read_text()
    source = source.replace(p.body(source, '00D18624'), '        return;\n')
    try:
        p.generate(source, '091')
    except ValueError:
        pass
    else:
        raise AssertionError('Changed production input accepted')
    print('PASS input guards; no iPhone gameplay or thermal claim.')


if __name__ == '__main__':
    main()
