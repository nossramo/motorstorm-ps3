#!/usr/bin/env python3
"""Run the native port for a bounded interval and retain its complete output."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--seconds', type=int, default=30)
parser.add_argument('--max-log-mb', type=int, default=64)
parser.add_argument('--binary', type=Path, default=ROOT / 'build/game/MotorStormRecomp')
parser.add_argument('--elf', type=Path, default=ROOT / 'game/PS3_GAME/USRDIR/EBOOT.elf')
parser.add_argument('--log', type=Path, default=ROOT / 'logs/native-boot.log')
args = parser.parse_args()
args.log.parent.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, PS3_TITLE='MotorStorm — native port research')
env.setdefault('PS3_HLE_UNRESOLVED', 'fail')
env.setdefault('PS3_HLE_TRACE', '200')
env.setdefault('GCM_SEMA_ACQUIRE', '1')
env.setdefault('RSX_ENGINE_CPU_READBACK', '1')
for component in ('AVCODEC', 'AVUTIL'):
    library = Path('/opt/homebrew/lib') / f'lib{component.lower()}.dylib'
    if library.exists():
        env.setdefault(f'PS3RECOMP_FFMPEG_{component}', str(library))
start = time.monotonic()
with args.log.open('w') as out:
    proc = subprocess.Popen([str(args.binary.resolve()), str(args.elf.resolve())],
                            cwd=ROOT, env=env, stdout=out, stderr=subprocess.STDOUT,
                            start_new_session=True)
    args.log.with_suffix('.pid').write_text(str(proc.pid))
    try:
        stop_reason = 'probe timeout'
        while proc.poll() is None:
            if args.log.stat().st_size >= args.max_log_mb * 1024 * 1024:
                stop_reason = 'log size limit reached'
                raise subprocess.TimeoutExpired(proc.args, args.seconds)
            if time.monotonic() - start >= args.seconds:
                raise subprocess.TimeoutExpired(proc.args, args.seconds)
            time.sleep(0.25)
        result = f'exit={proc.returncode}'
    except subprocess.TimeoutExpired:
        os.killpg(proc.pid, signal.SIGTERM)
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            os.killpg(proc.pid, signal.SIGKILL)
            proc.wait()
        result = f'{stop_reason}; process group stopped (not a success verdict)'
    out.write(f'\n[probe] {result}; elapsed={time.monotonic()-start:.2f}s\n')
print(f'{result}; log={args.log}')
