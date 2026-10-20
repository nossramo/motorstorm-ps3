#!/usr/bin/env python3
"""HUD-gated native timing; renderer acknowledges screenshot pause before timing.
OCR/readback run only before arming and after the complete trace window.
The control plan is scheduled against the game's CLOCK_MONOTONIC ack.
"""
import argparse, hashlib, json, math, os, pathlib, re, shutil, signal, subprocess, time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--out', type=pathlib.Path, required=True)
p.add_argument('--ocr', type=pathlib.Path, required=True)
p.add_argument('--control-plan', type=pathlib.Path, required=True)
p.add_argument('--duration', type=float, default=20)
p.add_argument('--lead-in', type=float, default=2)
p.add_argument('--seconds', type=int, default=240)
p.add_argument('--cancel-menu-on-hud', action='store_true',
               help='Cancel remaining menu-script presses after the first racing HUD; require native acknowledgement')
p.add_argument('--race-time-min', type=float, help='Arm only after this OCR-verified race-clock time')
p.add_argument('--race-time-tolerance', type=float, default=1.5,
               help='Reject a race-time gate overshoot larger than this many seconds')
p.add_argument('--env', action='append', default=[], metavar='NAME=VALUE')
a = p.parse_args()
if not math.isfinite(a.duration) or not math.isfinite(a.lead_in) or a.duration <= 0 or a.lead_in < 1:
    p.error('Finite positive duration and at least one second lead-in required')
if a.seconds <= a.duration + a.lead_in:
    p.error('Probe limit must also leave time to boot and verify gameplay')
if (a.race_time_min is not None and (not math.isfinite(a.race_time_min) or a.race_time_min < 0)) or \
        not math.isfinite(a.race_time_tolerance) or a.race_time_tolerance <= 0:
    p.error('Race-clock gate must be finite and nonnegative, with positive tolerance')
plan = json.loads(a.control_plan.read_text())
if not plan or plan[0]['at'] != 0 or any(s['at'] < 0 or len(s['axes']) != 4 or
    any(not isinstance(v, int) or not 0 <= v <= 255 for v in s['axes']) for s in plan):
    p.error('Plan must start at zero with four valid stick bytes')
if [s['at'] for s in plan] != sorted(s['at'] for s in plan): p.error('Control times must be ordered')
root = pathlib.Path(__file__).resolve().parents[1]
out = a.out.resolve()
if out.exists(): p.error('Choose a fresh output directory')
out.mkdir(parents=True)
pad = out/'pad.txt'; pad.write_text('0x0000 0\n')
trigger = out/'trigger.txt'; trigger.write_text('0\n')
env = dict(os.environ, MOTORSTORM_NATIVE_JOBS='1', MOTORSTORM_GPU_READ_TIMEOUT_SECONDS='120',
    RSX_ENGINE_CPU_READBACK='demand', PS3RECOMP_METAL_FPS='1', PS3_HLE_TRACE='0')
for setting in a.env:
    k, sep, v = setting.partition('=')
    if not sep: p.error('--env requires NAME=VALUE')
    env[k] = v
# Paths and timing are owned by this harness, including if the caller inherited
# settings from a previous fixed-time benchmark.
env.pop('PS3RECOMP_METAL_FRAME_DUMP_AFTER', None)
for key in ('PAD_CLOCK', 'PAD_AUTOPRESS', 'PAD_STICK', 'PAD_SWEEP', 'PAD_SCRIPT_STOP_FILE'):
    env.pop(key, None)
env.update(PS3_FTRACE=f'{a.lead_in}:{a.duration}:{out}/trace.txt',
    PS3_FTRACE_TRIGGER=str(trigger), PS3_FTRACE_MIN_US='0', PAD_FILE=str(pad),
    PAD_SCRIPT='5:0x4000,15:0x4000,25:0x4000,35:0x4000,45:0x4000,49:0x4000,53:0x4000,57:0x20,61:0x20,65:0x4000,80:0x4000,100:0x4000',
    PS3RECOMP_METAL_FRAME_DUMP=str(out/'current.ppm'),
    PS3RECOMP_METAL_FRAME_DUMP_EVERY='15',
    PS3RECOMP_METAL_FRAME_DUMP_PAUSE_DURING_TRACE='1')
script_stop = out/'script-stop.txt'
if a.cancel_menu_on_hud:
    env['PAD_SCRIPT_STOP_FILE'] = str(script_stop)
(out/'config.json').write_text(json.dumps({k:v for k,v in env.items()
    if k.startswith(('MOTORSTORM_', 'RSX_', 'GCM_', 'PS3_', 'PS3RECOMP_', 'SPU_', 'SPURS_', 'PAD_', 'MTL_'))}, indent=2)+'\n')
with (root/'build/game/MotorStormRecomp').open('rb') as f:
    binary_hash = hashlib.file_digest(f, 'sha256').hexdigest()
(out/'build-info.json').write_text(json.dumps({'binary_sha256':binary_hash,
    'cmake_options':[line for line in (root/'build/game/CMakeCache.txt').read_text().splitlines()
    if line.startswith(('MOTORSTORM_', 'CMAKE_BUILD_TYPE:', 'CMAKE_C_FLAGS:', 'CMAKE_CXX_FLAGS:',
                        'CMAKE_C_FLAGS_RELEASE:', 'CMAKE_CXX_FLAGS_RELEASE:'))]}, indent=2)+'\n')
clock = lambda: time.clock_gettime_ns(time.CLOCK_MONOTONIC)
child = subprocess.Popen(['python3', 'tools/run_probe.py', '--seconds', str(a.seconds),
    '--log', str(out/'native.log')], cwd=root, env=env)
launched = clock()
stamp = None; hud_count = 0; armed = False; ack = None; paused_stamp = None
capture_violation = False; trace_complete = False; post_image = None; last_step = -1
events = []; observations = []; last_ocr_end = None; pre_window_ocr_end = None
ack_observed_ns = None; final_menu_event_ns = None; error = None
race_time_at_arm = None
script_stop_requested_ns = None; script_stop_ack_ns = None
host_load = {'before':os.getloadavg()}

def observe(name):
    """Return an unchanged, complete image and OCR text; never called while armed."""
    global stamp, last_ocr_end
    frame = out/'current.ppm'
    if not frame.exists() or frame.stat().st_mtime_ns == stamp: return None
    observed = frame.stat().st_mtime_ns
    png = out/'observed.png'
    result = subprocess.run(['sips', '-s', 'format', 'png', str(frame), '--out', str(png)], capture_output=True)
    if result.returncode or frame.stat().st_mtime_ns != observed: return None
    stamp = observed
    text = subprocess.run([str(a.ocr.resolve()), str(png)], capture_output=True,
                          text=True, timeout=15, check=True).stdout
    last_ocr_end = clock()
    hud = all(word in text.upper() for word in ('LAP', 'POS', 'BOOST'))
    race_clock = re.search(r'\b(\d{2}):([0-5]\d)[.,](\d{2})\b', text)
    race_time = (int(race_clock[1])*60 + int(race_clock[2]) +
                 int(race_clock[3])/100) if race_clock else None
    if hud or name == 'after':
        saved = f'{name}-{len(observations):03d}.png'
        shutil.copyfile(png, out/saved)
        observations.append(dict(image=saved, hud=hud, ocr=text, race_time_seconds=race_time,
                                 monotonic_ns=last_ocr_end,
                                 elapsed=(last_ocr_end-launched)/1e9))
        return observations[-1]
    return dict(hud=False)

def log_tail():
    path = out/'native.log'
    if not path.exists(): return b''
    with path.open('rb') as f:
        f.seek(max(0, f.seek(0, 2)-65536))
        return f.read()

try:
    while child.poll() is None:
        now = clock()
        if not armed:
            if final_menu_event_ns is None and b'PAD_SCRIPT t=100.0s press' in log_tail():
                final_menu_event_ns = now
            image = observe('before')
            if image:
                hud_count = hud_count+1 if image['hud'] else 0
                if a.cancel_menu_on_hud and image['hud'] and script_stop_requested_ns is None:
                    script_stop.write_text('1\n')
                    script_stop_requested_ns = clock()
            if a.cancel_menu_on_hud and script_stop_requested_ns is not None and script_stop_ack_ns is None:
                stop_ack = out/'script-stop.txt.ack'
                if stop_ack.exists() and stop_ack.read_text().strip() == '1':
                    script_stop_ack_ns = clock()
            phase_ready = a.race_time_min is None
            if (a.race_time_min is not None and image and image.get('hud') and
                    image.get('race_time_seconds') is not None):
                phase_ready = image['race_time_seconds'] >= a.race_time_min
            # Either explicitly cancel the script with native acknowledgement,
            # or retain the legacy last-press/drain guard. Merely seeing the
            # HUD must not allow a later scripted button into the timed window.
            menu_ready = (script_stop_ack_ns is not None) if a.cancel_menu_on_hud else (
                final_menu_event_ns is not None and (clock()-final_menu_event_ns)/1e9 >= 3)
            if phase_ready and hud_count >= 2 and menu_ready:
                race_time_at_arm = image.get('race_time_seconds') if image else None
                if (a.race_time_min is not None and
                        race_time_at_arm > a.race_time_min + a.race_time_tolerance):
                    raise RuntimeError(f'Race-clock gate overshot: {race_time_at_arm:.2f}s; timing rejected')
                pre_window_ocr_end = last_ocr_end
                command = out/'trigger.next'
                command.write_text('1\n')
                command.replace(trigger)
                armed = True
                print('Racing HUD verified in two frames; arming trace and pausing readback.', flush=True)
        elif ack is None:
            ack_path = out/'trigger.txt.ack'
            if ack_path.exists():
                ack = json.loads(ack_path.read_text())
                ack_observed_ns = clock()
                if not ack['end_ns'] > ack['start_ns'] > ack_observed_ns > pre_window_ocr_end:
                    raise RuntimeError('Late or invalid renderer acknowledgement; control window cannot be aligned')
                paused_stamp = (out/'current.ppm').stat().st_mtime_ns
                print('Renderer acknowledged; controller follows exact trace clock.', flush=True)
        else:
            now = clock()
            if now < ack['end_ns']:
                if (out/'current.ppm').stat().st_mtime_ns != paused_stamp: capture_violation = True
            if now >= ack['start_ns']:
                elapsed = (now-ack['start_ns'])/1e9
                index = max(i for i,s in enumerate(plan) if s['at'] <= elapsed)
                step = plan[index]
                if index != last_step:
                    events.append(dict(step=index, relative_to_trace_seconds=elapsed, **step))
                    last_step = index
                if pad.stat().st_size == 0 or pad.read_text().strip() == '0x0000 0':
                    command = out/'pad.next'
                    command.write_text(f"{step['mask']} 4 {' '.join(map(str,step['axes']))}\n")
                    command.replace(pad)
            if now > ack['end_ns']:
                # The completion line is emitted after fclose(trace), avoiding
                # accepting or parsing a partially written trace.
                trace_complete = trace_complete or b'[ftrace] wrote ' in log_tail()
                # The last pre-window screenshot may be newer than the last
                # OCR. Do not mistake that paused image for post-window proof.
                if trace_complete and (out/'current.ppm').stat().st_mtime_ns != paused_stamp:
                    post_image = observe('after') or post_image
                    if post_image and post_image.get('hud'): break
        time.sleep(.02 if armed else .25)
except BaseException as exc:
    error = repr(exc)
    raise
finally:
    host_load['after'] = os.getloadavg()
    evidence = dict(hud_observations=observations, trace_ack=ack, control_events=events,
        last_pre_window_ocr_ns=pre_window_ocr_end, ack_observed_ns=ack_observed_ns,
        final_menu_event_observed_ns=final_menu_event_ns,
        script_cancellation=dict(enabled=a.cancel_menu_on_hud,
                                 requested_ns=script_stop_requested_ns, acknowledged_ns=script_stop_ack_ns),
        race_time_gate=dict(min_seconds=a.race_time_min, tolerance_seconds=a.race_time_tolerance,
                            observed_seconds=race_time_at_arm, lead_in_seconds=a.lead_in),
        screenshot_changed_while_armed=capture_violation, trace_complete=trace_complete,
        post_window_image=post_image, host_load=host_load, error=error,
        caveat='Identical control timing does not make collisions or host load deterministic. Review before/after images; no lap or temporal-quality claim.')
    (out/'evidence.json').write_text(json.dumps(evidence,indent=2)+'\n')
    pidfile = out/'native.pid'
    if child.poll() is None and pidfile.exists():
        try: os.killpg(int(pidfile.read_text()),signal.SIGTERM)
        except ProcessLookupError: pass
    try: child.wait(timeout=4)
    except subprocess.TimeoutExpired:
        if pidfile.exists():
            try: os.killpg(int(pidfile.read_text()),signal.SIGKILL)
            except ProcessLookupError: pass
        child.wait(timeout=4)
if (not ack or capture_violation or not trace_complete or not post_image or not post_image.get('hud') or
        (a.cancel_menu_on_hud and script_stop_ack_ns is None)):
    raise SystemExit('Timing not accepted: inspect HUD/ack/readback evidence.')
with (out/'report.txt').open('w') as f:
    subprocess.run(['python3','tools/report_gameplay_profile.py',str(out/'trace.txt'),
        '--out',str(out/'report.json')],cwd=root,stdout=f,check=True)
print(f'HUD-gated timing saved to {out}; inspect the visual evidence before interpreting performance.')
