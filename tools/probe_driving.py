#!/usr/bin/env python3
"""Bounded HUD-gated driving check; screenshots/OCR make this unsuitable for FPS claims."""
import argparse, json, os, pathlib, re, shutil, signal, subprocess, time, threading

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--out', type=pathlib.Path, required=True)
p.add_argument('--ocr', type=pathlib.Path, required=True)
p.add_argument('--seconds', type=int, default=240)
p.add_argument('--drive-polls', type=int, default=10000)
p.add_argument('--control-plan', type=pathlib.Path, help='JSON steps: at (seconds after HUD), mask, axes [lx,ly,rx,ry]')
p.add_argument('--drive-seconds', type=float, default=35)
p.add_argument('--cancel-menu-on-hud', action='store_true')
p.add_argument('--env', action='append', default=[])
a = p.parse_args()
if a.drive_polls <= 0: p.error('--drive-polls must be positive')
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
pad = out/'pad.txt'; pad.write_text('0 0\n')
env = dict(os.environ, MOTORSTORM_NATIVE_JOBS='1', RSX_ENGINE_CPU_READBACK='demand',
    MOTORSTORM_GPU_READ_TIMEOUT_SECONDS='120', PS3_HLE_TRACE='0',
    PAD_FILE=str(pad),
    PAD_SCRIPT='5:0x4000,15:0x4000,25:0x4000,35:0x4000,45:0x4000,49:0x4000,53:0x4000,57:0x20,61:0x20,65:0x4000,80:0x4000,100:0x4000',
    PS3RECOMP_METAL_FRAME_DUMP=str(out/'current.ppm'), PS3RECOMP_METAL_FRAME_DUMP_EVERY='30')
for setting in a.env:
    key, sep, value = setting.partition('=')
    if not sep: p.error('--env requires NAME=VALUE')
    env[key] = value
# The visual harness owns its files and must never arm an inherited trace or
# overwrite outputs from the measurement whose settings were supplied.
for key in list(env):
    if key.startswith('PS3_FTRACE') or key in ('PAD_CLOCK', 'PAD_AUTOPRESS', 'PAD_STICK',
        'PAD_SWEEP', 'PAD_SCRIPT_STOP_FILE', 'PS3RECOMP_METAL_FRAME_DUMP_AFTER',
        'PS3RECOMP_METAL_FRAME_DUMP_PAUSE_DURING_TRACE'):
        env.pop(key)
env.update(PAD_FILE=str(pad),
    PS3RECOMP_METAL_FRAME_DUMP=str(out/'current.ppm'), PS3RECOMP_METAL_FRAME_DUMP_EVERY='30')
script_stop = out/'script-stop.txt'
if a.cancel_menu_on_hud: env['PAD_SCRIPT_STOP_FILE'] = str(script_stop)
(out/'config.json').write_text(json.dumps({k:v for k,v in env.items()
    if k.startswith(('MOTORSTORM_', 'RSX_', 'GCM_', 'PS3_', 'PS3RECOMP_', 'SPU_', 'SPURS_', 'PAD_', 'MTL_'))},indent=2)+'\n')
child = subprocess.Popen(['python3', 'tools/run_probe.py', '--seconds', str(a.seconds),
    '--log', str(out/'native.log')], cwd=root, env=env)
started = time.monotonic(); stamp = None; drive_start = None; evidence = []
hud_seen = False
script_stop_requested = False; script_stop_acknowledged = False
stop_controls = threading.Event()
control_events = []
def controls():
    last_step = -1
    while not stop_controls.wait(.01):
        if drive_start is None: continue
        elapsed = time.monotonic() - drive_start
        index = max(i for i, s in enumerate(plan) if s['at'] <= elapsed)
        step = plan[index]
        if index != last_step:
            control_events.append(dict(step=index, elapsed=elapsed, **step)); last_step = index
        # Keep a short hold queued; no snapshot/OCR work interrupts controller
        # delivery. Atomic replacement avoids partially read command files.
        if pad.stat().st_size == 0:
            command = out/'pad.next'
            command.write_text(f"{step['mask']} 4 {' '.join(map(str, step['axes']))}\n")
            command.replace(pad)
controller = threading.Thread(target=controls, daemon=True) if plan else None
if controller: controller.start()
try:
    while child.poll() is None:
        frame = out/'current.ppm'
        if frame.exists() and frame.stat().st_mtime_ns != stamp:
            observed = frame.stat().st_mtime_ns
            png = out/'observed.png'
            result = subprocess.run(['sips', '-s', 'format', 'png', str(frame), '--out', str(png)], capture_output=True)
            if result.returncode == 0 and frame.stat().st_mtime_ns == observed:
                stamp = observed
                text = subprocess.run([str(a.ocr.resolve()), str(png)], capture_output=True, text=True, timeout=15).stdout
                hud = all(word in text.upper() for word in ('LAP', 'POS', 'BOOST'))
                hud_seen = hud_seen or hud
                race_clock = re.search(r'\b(\d{2}):([0-5]\d)[.,](\d{2})\b', text)
                race_time = (int(race_clock[1])*60 + int(race_clock[2]) +
                             int(race_clock[3])/100) if race_clock else None
                if hud or drive_start is not None:
                    name = f'gameplay-{len(evidence):03d}.png'
                    shutil.copyfile(png, out/name)
                    evidence.append(dict(image=name, elapsed=time.monotonic()-started, hud=hud, ocr=text,
                        race_time_seconds=race_time,
                        drive_elapsed=time.monotonic()-drive_start if drive_start is not None else None))
                if hud and drive_start is None:
                    if a.cancel_menu_on_hud:
                        if not script_stop_requested:
                            script_stop.write_text('1\n'); script_stop_requested = True
        # The first HUD is sufficient for this visual-only check. Do not wait
        # for a second OCR success to notice an acknowledgement: partially
        # obscured HUD text otherwise delays initial throttle by several seconds.
        if hud_seen and drive_start is None:
            if script_stop_requested:
                ack = out/'script-stop.txt.ack'
                script_stop_acknowledged = ack.exists() and ack.read_text().strip() == '1'
            if not a.cancel_menu_on_hud or script_stop_acknowledged:
                pad.write_text('' if plan else f'0x0200 {a.drive_polls}\n')
                drive_start = time.monotonic()
                print('Race HUD verified; starting control plan.' if plan else
                      f'Race HUD verified; requesting R2 for {a.drive_polls} delivered input reports.', flush=True)
        if drive_start is not None and time.monotonic()-drive_start >= a.drive_seconds: break
        time.sleep(.5)
finally:
    stop_controls.set()
    if controller: controller.join(timeout=2)
    (out/'evidence.json').write_text(json.dumps(dict(hud_detected=drive_start is not None,
        drive_start_elapsed=drive_start-started if drive_start is not None else None,
        requested_input=plan or f'R2 for {a.drive_polls} delivered input reports',
        script_cancellation=dict(requested=script_stop_requested, acknowledged=script_stop_acknowledged),
        control_events=control_events, images=evidence,
        caveat='Visual driving check, not a performance benchmark; inspect frames for actual movement.'), indent=2))
    pidfile = out/'native.pid'
    if child.poll() is None and pidfile.exists():
        try: os.killpg(int(pidfile.read_text()), signal.SIGTERM)
        except ProcessLookupError: pass
    try: child.wait(timeout=10)
    except subprocess.TimeoutExpired:
        if pidfile.exists():
            try: os.killpg(int(pidfile.read_text()), signal.SIGKILL)
            except ProcessLookupError: pass
        child.wait(timeout=10)
if drive_start is None: raise SystemExit('No verified race HUD; inspect the last frame and log.')
print(f'Driving evidence saved to {out}; movement is not established until frames are reviewed.')
