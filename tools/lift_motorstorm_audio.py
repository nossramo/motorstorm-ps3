#!/usr/bin/env python3
"""Generate checked AOT Multistream DSP overlays from the user's local game.

Game bytes and generated code stay in ignored port/spu/. Full live-code checks
reject another version or placement; unknown DSP code keeps the interpreter.
MOTORSTORM_NATIVE_AUDIO=0 disables these bodies. No JIT/executable-memory writes.
"""
from pathlib import Path
import hashlib
import json
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / 'vendor/ps3recomp/tools'
GAME_HASH = '983baf06f5357a66b3f97fd5ac20252b50a4161b6373ba290c8206a833250c0d'
# ELF offset, loaded text address, measured processing entry, text byte count.
MODULES = [(0xF85180, 0x30400, 0x30720, 8896),
           (0xF81900, 0x2F800, 0x2FEF0, 2960),
           (0xF83680, 0x32700, 0x32770, 1728)]
OUT = ROOT / 'port/spu'
REGISTER = OUT / 'motorstorm_audio_register.c'


def main():
    game = (ROOT / 'game/PS3_GAME/USRDIR/EBOOT.elf').read_bytes()
    if hashlib.sha256(game).hexdigest() != GAME_HASH:
        raise SystemExit('Unsupported MotorStorm executable: refusing audio overlay generation.')
    digest = hashlib.sha256()
    for path in [Path(__file__), *[SDK / name for name in
                 ('spu_lifter.py', 'spu_disasm.py', 'find_spu_functions.py')]]:
        digest.update(path.read_bytes())
    fingerprint = digest.hexdigest()
    stamp = OUT / 'motorstorm_audio.sha256'
    dirs = [OUT / ('motorstorm_audio_dsp' if n == 0 else f'motorstorm_audio_{entry:05X}')
            for n, (_, _, entry, _) in enumerate(MODULES)]
    products = [REGISTER, *[p / f for p in dirs for f in
                          ('spu_recomp.c', 'spu_recomp.h', 'report.json')]]
    if stamp.exists() and stamp.read_text() == fingerprint and all(p.exists() for p in products):
        return
    sys.path.insert(0, str(SDK))
    from find_spu_functions import detect_functions
    declarations, signatures, cases = [], [], []
    for out, (elf_offset, ls_base, entry, expected_size) in zip(dirs, MODULES):
        functions, (offset, base, size) = detect_functions(game[elf_offset:])
        expected_offset = 0x100 if entry == 0x2FEF0 else 0x80
        if (offset, base, size) != (expected_offset, 0x80, expected_size):
            raise SystemExit('Unexpected audio overlay layout.')
        delta = ls_base - base
        code = game[elf_offset+offset:elf_offset+offset+size]
        out.mkdir(parents=True, exist_ok=True)
        (out/'code.bin').write_bytes(code)
        (out/'bounds.json').write_text(json.dumps([
            {'start': hex(a+delta), 'end': hex(b+delta)} for a, b in functions]))
        prefix = f'ms_audio_{entry:05X}_'
        subprocess.run([sys.executable, str(SDK/'spu_lifter.py'), str(out/'code.bin'),
                        '--base', hex(ls_base), '--functions', str(out/'bounds.json'),
                        '--output', str(out), '--symbol-prefix', prefix,
                        '--merge-chunks', '--report', str(out/'report.json')], check=True)
        if json.loads((out/'report.json').read_text())['unsupported']:
            raise SystemExit('Unsupported DSP instructions: refusing native registration.')
        for path in out.glob('*.[ch]'):
            path.write_text(path.read_text().replace('../../runtime/spu/', ''))
        name = f'{prefix}spu_func_{entry:08X}'
        declarations.append(f'extern void {name}(spu_context*);')
        values = ',\n'.join('    '+','.join(f'0x{b:02x}' for b in code[i:i+24])
                             for i in range(0, len(code), 24))
        signatures.append(f'static const unsigned char code_{entry:X}[] = {{\n{values}\n}};')
        cases.append(f'    {{0x{entry:X}, 0x{ls_base:X}, sizeof code_{entry:X}, code_{entry:X}, {name}}}')
    REGISTER.write_text('''/* Generated from the user's local game; do not distribute. */
#include "spu_interp.h"
#include <stdlib.h>
#include <string.h>
extern spu_lifted_fn (*g_spu_port_overlay_lookup)(const spu_context*, uint32_t);
'''+ '\n'.join(declarations+signatures)+'''
static const struct {
    uint32_t entry, base, size;
    const unsigned char* code;
    spu_lifted_fn fn;
} overlays[] = {
'''+',\n'.join(cases)+'''
};
static int enabled, trace;
spu_lifted_fn motorstorm_audio_lookup(const spu_context* ctx, uint32_t pc) {
    if (!enabled || ctx->image_id != 286484578 || ctx->policy_mode) return NULL;
    for (unsigned i=0; i<sizeof overlays/sizeof overlays[0]; ++i) {
        if (pc != overlays[i].entry) continue;
        if (memcmp(ctx->ls+overlays[i].base, overlays[i].code, overlays[i].size)) return NULL;
        static SPU_THREAD_LOCAL unsigned reported;
        if (trace && !(reported & (1u<<i))) {
            reported |= 1u<<i;
            fprintf(stderr, "[audio-dsp] verified native overlay at LS 0x%05X\\n", pc);
        }
        return overlays[i].fn;
    }
    return NULL;
}
__attribute__((constructor)) static void install_audio_overlay(void) {
    const char* value = getenv("MOTORSTORM_NATIVE_AUDIO");
    enabled = !value || strcmp(value, "0");
    trace = getenv("MOTORSTORM_AUDIO_TRACE") != NULL;
    g_spu_port_overlay_lookup = motorstorm_audio_lookup;
}
''')
    stamp.write_text(fingerprint)
    print('Generated three checked native Multistream overlays (local game data only).')


if __name__ == '__main__':
    main()
