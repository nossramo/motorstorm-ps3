#!/usr/bin/env python3
"""iOS pilot: register locals and conservative may-dirty writeback.

Uses the pinned MIT DOD3 pass, without its ABI liveness, frame/memory rewrites,
flag splitting, or nonvolatile-register assumptions. Every converted register
is reloaded after a call. Unknown inputs fail closed; originals remain in the
generated unit behind MOTORSTORM_PPU_LOCALS=0. Never edits a lifted input.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / 'tools/third_party/ps3recomp/ppu_nv_dirty.py'
spec = importlib.util.spec_from_file_location('motorstorm_nv_dirty', UPSTREAM)
nv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(nv)

# Historical main/physics CPU samples. Validate each body independently so
# unrelated regeneration in these large units does not invalidate the pilot.
FUNCTIONS = {
    '075': {'00A2ECAC': 'c8fa05528f39eaf31846665f1b0b75b6bbb72274905f78a189c879b51801fdab',
            '00A3AD78': '9352c435a2b049529ef37ca6f471a67fe9b1b2c03d0bae36668aeba09fe04880'},
    '091': {'00D18624': 'caac6f143c9ba2008193448e35717014cfbb7c3d4ffb68a37bf0b528339acc8c'},
    '092': {'00D4CCC4': '03f7af940b905c7ec5ab876d90a9a683e2d181cfd96d9400ad321e3c570b581e'},
}
FUNCTION = re.compile(r'^void func_([0-9A-F]{8})\(ppu_context\* ctx\) \{\n(.*?)^\}', re.M | re.S)
PURE_HELPERS = {
    'if', 'for', 'switch', 'void', 'memcpy', 'sqrt', 'VM_RAW_ST',
    'ppc_rldic', 'ppc_rldicl', 'ppc_rldicr', 'ppc_rldimi', 'ppc_rlw64', 'ppc_srad',
    'ppu_fadd', 'ppu_fdiv', 'ppu_fmadd_core', 'ppu_fmul', 'ppu_fp_single',
    'ppu_frsp', 'ppu_fsub', 'ppu_timebase_now',
    'vm_raw_ld16', 'vm_raw_st16', 'vm_read8', 'vm_read16', 'vm_read32', 'vm_read64',
    'vm_write8', 'vm_write32', 'vm_write64',
    'vmx_vaddfp', 'vmx_vcmpgtfp', 'vmx_vcmpeqfp', 'vmx_vmaddfp',
    'vmx_vnmsubfp', 'vmx_vrsqrtefp', 'vmx_vsubfp',
}
TRANSFERS = {'DRAIN_TRAMPOLINE', 'ps3_indirect_call', 'ps3_hle_call', 'lv2_syscall'}


def body(source, address):
    found = [m for m in FUNCTION.finditer(source) if m[1] == address]
    if len(found) != 1:
        raise ValueError(f'Expected exactly one func_{address}')
    return found[0][2]


def rewrite(original, address, stats=None):
    # The upstream parser knows the lifter's statement grammar, not arbitrary
    # C++. Do not let a new context observer, escaped alias or helper slip by.
    masked = '\n'.join(nv._mask(line) for line in original.splitlines())
    names = set(re.findall(r'\b([A-Za-z_]\w*)\s*\(', masked))
    unknown = {n for n in names if n not in PURE_HELPERS | TRANSFERS
               and not re.fullmatch(r'func_[0-9A-F]{8}', n)}
    if unknown:
        raise ValueError(f'Unreviewed helpers in {address}: {sorted(unknown)}')
    residual = re.sub(r'ctx->(?:gpr|fpr|vr)\[\d+\]|ctx->(?:cr|xer|ctr|lr|vscr)\b', '', masked)
    residual = re.sub(r'\b(?:func_[0-9A-F]{8}|DRAIN_TRAMPOLINE|ps3_indirect_call|lv2_syscall)\(ctx\)', '', residual)
    residual = re.sub(r'ps3_hle_call\(0x[0-9A-Fa-f]+u?, ctx\)', '', residual)
    if re.search(r'\bctx\b', residual):
        raise ValueError(f'Unreviewed context use in {address}')
    if re.search(r'\b(?:r\d+|f\d+|v\d+|cr|xer|ctr)\b', residual):
        raise ValueError(f'Local name collision in {address}')
    # Every register is observable at transfers; no dead-register elimination.
    sets = [set(map(int, re.findall(r'ctx->' + kind + r'\[(\d+)\]', masked)))
            for kind in ('gpr', 'fpr', 'vr')]
    sprs = set(re.findall(r'ctx->(cr|xer|ctr)\b', masked))
    regs = nv.Regs(*sets, sprs)
    reload = ' '.join(f'{regs.local(k)} = {regs.file(k)};' for k in regs.keys)
    result, _ = nv.rewrite(original.splitlines(), int(address, 16), *sets, sprs,
                           '    ' + regs.decl(), reload, stats=stats, live=False)
    if result is None:
        raise ValueError(f'Unsupported control flow in {address}: {stats}')
    return '\n'.join(result) + '\n'


def generate(source, unit):
    selected = FUNCTIONS[unit]
    seen, report = set(), {}
    def replace(m):
        address, original = m[1], m[2]
        if address not in selected:
            return m[0]
        if hashlib.sha256(original.encode()).hexdigest() != selected[address]:
            raise ValueError(f'func_{address} changed; review and test before updating its hash')
        if address in seen:
            raise ValueError(f'Duplicate func_{address}')
        seen.add(address)
        stats = {}
        optimized = rewrite(original, address, stats)
        report[address] = stats
        return (f'static __attribute__((noinline)) void ms_original_{address}(ppu_context* ctx) {{\n'
                + original + '}\n\n'
                + f'static __attribute__((noinline)) void ms_locals_{address}(ppu_context* ctx) {{\n'
                + optimized + '}\n\n'
                + f'void func_{address}(ppu_context* ctx) {{\n'
                + f'    if (ms_ppu_locals_enabled()) ms_locals_{address}(ctx);\n'
                + f'    else ms_original_{address}(ctx);\n}}')
    output = FUNCTION.sub(replace, source)
    if seen != set(selected):
        raise ValueError(f'Missing functions: {set(selected) - seen}')
    return '#include "ppu_locals.h"\n' + output, report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--unit', required=True, choices=FUNCTIONS)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve():
        parser.error('Refusing to overwrite the original lift')
    output, report = generate(args.input.read_text(), args.unit)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(output)
    args.output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PPU locals: unit {args.unit}, {len(report)} functions; original fallback retained')


if __name__ == '__main__':
    main()
