#!/usr/bin/env python3
"""Experimental WWS overlay lift; runtime opt-in: MOTORSTORM_NATIVE_JOBS=1.

Reuses ps3recomp's signature matching and job-module lifter. Extracts only
headers within the hash-verified retail executable; unsupported instruction
counts must be reviewed before promoting this experiment to the default.
"""
from pathlib import Path
import hashlib
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
data = (ROOT / 'game/PS3_GAME/USRDIR/EBOOT.elf').read_bytes()
assert hashlib.sha256(data).hexdigest() == '983baf06f5357a66b3f97fd5ac20252b50a4161b6373ba290c8206a833250c0d'
out = ROOT / 'port/spu/jobmods'
out.mkdir(exist_ok=True)
position = 0
count = 0
while True:
    position = data.find(bytes.fromhex('c0dec0de'), position)
    if position < 0:
        break
    base = position - 32
    position += 4
    if base < 0 or not all(data[base+i] in (0x42, 0x43) for i in (0, 4, 8, 12)):
        continue
    entry, size = struct.unpack_from('>II', data, base + 16)
    assert 0x30 <= entry < size < 0x3c000 and base + size <= len(data)
    raw = data[base:base+size]
    digest = hashlib.sha1(raw).hexdigest()[:12]
    (out / f'jobmod_{digest}_e{entry:04X}.bin').write_bytes(raw)
    count += 1
assert count == 10
registration = ROOT / 'port/spu/jobmods_register.c'
subprocess.run([sys.executable, str(ROOT / 'vendor/ps3recomp/tools/lift_jobmods.py'),
                '--jobmods', str(out), '--out', str(ROOT / 'port/spu'),
                '--register', str(registration)], check=True)
for path in (ROOT / 'port/spu').glob('jobmod_*/*.[ch]'):
    path.write_text(path.read_text().replace('../../runtime/spu/', ''))
source = registration.read_text().replace('#include <stdint.h>', '#include <stdint.h>\n#include <stdlib.h>')
source = source.replace('void port_jobmods_register_all(void)\n{',
                        'void port_jobmods_register_all(void)\n{\n    if (!getenv("MOTORSTORM_NATIVE_JOBS")) return;')
registration.write_text(source)
