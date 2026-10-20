#!/usr/bin/env python3
"""Lift MotorStorm 03.10's raw WWS policy missed by ELF-only SPU discovery."""
import hashlib
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SDK = ROOT / 'vendor/ps3recomp/tools'
sys.path.insert(0, str(SDK))
from elf_parser import ELFFile
from wrap_spu_elf import wrap

path = ROOT / 'game/PS3_GAME/USRDIR/EBOOT.elf'
assert hashlib.sha256(path.read_bytes()).hexdigest() == '983baf06f5357a66b3f97fd5ac20252b50a4161b6373ba290c8206a833250c0d'
elf = ELFFile(str(path))
elf.load()
ea, size = 0x100DD080, 11136
raw = None
for i, ph in enumerate(elf.program_headers):
    if ph.p_type == 1 and ph.p_vaddr <= ea and ea + size <= ph.p_vaddr + ph.p_filesz:
        raw = elf.get_segment_data(i)[ea-ph.p_vaddr:ea-ph.p_vaddr+size]
assert raw is not None
# Match the runtime's historical offset basis (not the standard FNV basis).
fingerprint = 1469598103934665603
for byte in raw:
    fingerprint = ((fingerprint ^ byte) * 0x100000001b3) & 0xffffffffffffffff
assert fingerprint == 0x46EAABFB56B7B1B5, hex(fingerprint)
out = ROOT / 'port/spu/wws_policy'
out.mkdir(parents=True, exist_ok=True)
(out / 'policy.bin').write_bytes(raw)
(out / 'policy.elf').write_bytes(wrap(raw, base=0xA00, entry=0xA00))
subprocess.run([sys.executable, str(SDK / 'spu_lifter.py'), '--auto-functions',
                str(out / 'policy.elf'), '--symbol-prefix', 'motorstorm_wws_',
                '--return-entries', '-o', str(out)], check=True)
for p in out.glob('*.[ch]'):
    text = p.read_text().replace('../../runtime/spu/', '')
    p.write_text(text)
(ROOT / 'port/spu/wws_policy_register.c').write_text('''
#include "spu_workload.h"
extern void spu_begin_image(int);
extern void motorstorm_wws_spu_recomp_register(void);
extern void motorstorm_wws_spu_func_00000A00(spu_context*);
__attribute__((constructor)) static void register_wws_policy(void) {
    spu_begin_image(900);
    motorstorm_wws_spu_recomp_register();
    spu_workload_register_img(0x46EAABFB56B7B1B5ULL,
        motorstorm_wws_spu_func_00000A00, 900, "MotorStorm WWS policy");
}
''')
print(f'WWS policy: {size} bytes, fingerprint={fingerprint:016X}')
