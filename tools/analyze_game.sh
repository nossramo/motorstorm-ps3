#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
elf=game/PS3_GAME/USRDIR/EBOOT.elf
sdk=vendor/ps3recomp/tools
test -f "$elf"
python3 "$sdk/ppu_loader.py" "$elf" -o research/ppu
python3 "$sdk/gen_imports.py" "$elf" -o research/imports-named.json
python3 "$sdk/find_functions.py" "$elf" \
  --seed-json research/ppu/EBOOT.functions.json -o research/functions.json
# BCES00006 03.10: end of the last SHF_EXECINSTR section, including import stubs.
python3 "$sdk/ppu_lifter.py" "$elf" --functions research/functions.json \
  --hle-stubs research/ppu/EBOOT.imports.json --code-end 0xdca5b4 \
  --chunks 96 --jobs 4 -o port/recompiled
python3 "$sdk/build_spu_workloads.py" --input "$elf" --lifted port/spu \
  --out port/spu/spu_workloads.c --constructor --title MotorStorm
python3 tools/lift_policy.py
python3 tools/lift_motorstorm_jobs.py
python3 tools/patch_job_wait.py
