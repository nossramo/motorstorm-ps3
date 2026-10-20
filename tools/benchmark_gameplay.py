#!/usr/bin/env python3
"""Bounded, repeatable race benchmark. Verify race.ppm before accepting metrics.
One screenshot precedes the timing window; CPU sampling follows it. No Metal
capture or repeated screenshot/OCR work runs inside the measurement window.
"""
import argparse, hashlib, json, os, pathlib, subprocess, time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--out', type=pathlib.Path, required=True)
p.add_argument('--start', type=int, default=170)
p.add_argument('--duration', type=int, default=30)
p.add_argument('--env', action='append', default=[], metavar='NAME=VALUE')
p.add_argument('--control-plan', type=pathlib.Path, help='Same JSON controller steps as probe_driving.py')
p.add_argument('--controls-start', type=float, default=108, help='Wall seconds after launch to begin the verified control plan')
a = p.parse_args()
plan = json.loads(a.control_plan.read_text()) if a.control_plan else None
if plan:
    if plan[0]['at'] != 0 or any(s['at'] < 0 or len(s['axes']) != 4 or
        any(not isinstance(v, int) or not 0 <= v <= 255 for v in s['axes']) for s in plan):
        p.error('Control plan must start at zero with four valid stick bytes per step')
    if [s['at'] for s in plan] != sorted(s['at'] for s in plan): p.error('Control times must be ordered')
root = pathlib.Path(__file__).resolve().parents[1]
out = a.out.resolve()
if out.exists(): p.error('Choose a fresh output directory')
out.mkdir(parents=True)
(out/'pad.txt').write_text('0x0000 0\n')
env = dict(os.environ, MOTORSTORM_NATIVE_JOBS='1',
    MOTORSTORM_GPU_READ_TIMEOUT_SECONDS='120', RSX_ENGINE_CPU_READBACK='demand',
    PS3RECOMP_METAL_FPS='1', PS3_HLE_TRACE='0',
    PS3_FTRACE=f'{a.start}:{a.duration}:{out}/trace.txt', PS3_FTRACE_MIN_US='0',
    PAD_FILE=str(out/'pad.txt'),
    PAD_SCRIPT='5:0x4000,15:0x4000,25:0x4000,35:0x4000,45:0x4000,49:0x4000,53:0x4000,57:0x20,61:0x20,65:0x4000,80:0x4000,100:0x4000',
    PS3RECOMP_METAL_FRAME_DUMP=str(out/'race.ppm'),
    PS3RECOMP_METAL_FRAME_DUMP_AFTER=str(a.start-5))
for setting in a.env:
    name, sep, value = setting.partition('=')
    if not sep: p.error('--env needs NAME=VALUE')
    env[name] = value
(out/'config.json').write_text(json.dumps({k: v for k, v in env.items()
    if k.startswith(('MOTORSTORM_', 'RSX_', 'PS3_', 'PS3RECOMP_', 'SPU_', 'SPURS_', 'PAD_'))}, indent=2)+'\n')
binary=root/'build/game/MotorStormRecomp'
with binary.open('rb') as f:
    binary_hash=hashlib.file_digest(f,'sha256').hexdigest()
cache=root/'build/game/CMakeCache.txt'
build_options=[line for line in cache.read_text().splitlines()
    if line.startswith(('MOTORSTORM_', 'CMAKE_BUILD_TYPE:', 'CMAKE_C_FLAGS:', 'CMAKE_CXX_FLAGS:',
                        'CMAKE_C_FLAGS_RELEASE:', 'CMAKE_CXX_FLAGS_RELEASE:'))]
(out/'build-info.json').write_text(json.dumps({'binary_sha256':binary_hash,
    'cmake_options':build_options},indent=2)+'\n')
child = subprocess.Popen(['python3', 'tools/run_probe.py', '--seconds',
    str(a.start+a.duration+30), '--log', str(out/'native.log')], cwd=root, env=env)
start = time.monotonic()
sampler = None
control_events = []; last_step = -1
while child.poll() is None:
    elapsed = time.monotonic()-start
    if plan and elapsed >= a.controls_start:
        index = max(i for i, s in enumerate(plan) if s['at'] <= elapsed-a.controls_start)
        step = plan[index]
        if index != last_step:
            control_events.append(dict(step=index, elapsed=elapsed, **step)); last_step = index
        pad = out/'pad.txt'
        # Initial neutral command has not been consumed because it holds no button.
        if pad.stat().st_size == 0 or pad.read_text().strip() == '0x0000 0':
            command = out/'pad.next'
            command.write_text(f"{step['mask']} 4 {' '.join(map(str, step['axes']))}\n")
            command.replace(pad)
    if time.monotonic()-start > a.start+a.duration+5 and sampler is None:
        sampler = subprocess.Popen(['sample', (out/'native.pid').read_text().strip(),
            '5', '1', '-file', str(out/'stacks.txt')], stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL)
    time.sleep(.02 if plan else 1)
if sampler: sampler.wait()
if plan: (out/'control-events.json').write_text(json.dumps(control_events, indent=2)+'\n')
if (out/'trace.txt').exists():
    with (out/'report.txt').open('w') as f:
        subprocess.run(['python3', 'tools/report_gameplay_profile.py',
            str(out/'trace.txt'), '--out', str(out/'report.json')], cwd=root,
            stdout=f, check=True)
else:
    raise SystemExit('No timing window was recorded; inspect native.log')
print(f'Benchmark saved to {out}; verify race.ppm is gameplay before comparing.')
