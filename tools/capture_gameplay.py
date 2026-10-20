#!/usr/bin/env python3
"""Capture Metal only after recognizing the racing HUD in saved game frames."""
import argparse, json, os, pathlib, shutil, signal, subprocess, time

p = argparse.ArgumentParser()
p.add_argument('--out', type=pathlib.Path, required=True)
p.add_argument('--ocr', type=pathlib.Path, required=True, help='compiled ocr_frame.swift')
p.add_argument('--seconds', type=int, default=600)
p.add_argument('--frames', type=int, default=2, help='Complete Metal frames to capture (1–120)')
p.add_argument('--stationary', action='store_true', help='Leave controls neutral during capture')
a = p.parse_args()
if not 1 <= a.frames <= 120: p.error('--frames must be 1–120')
root = pathlib.Path(__file__).resolve().parents[1]
out = a.out.resolve()
if out.exists(): p.error('Use a fresh output directory')
out.mkdir(parents=True)
env = dict(os.environ, PS3RECOMP_METAL_FRAME_DUMP_EVERY='30')
child = subprocess.Popen(['python3', 'tools/profile_gameplay.py', '--capture',
    '--capture-frames', str(a.frames),
    '--capture-trigger', str(out/'trigger.txt'), '--seconds', str(a.seconds),
    '--out', str(out)], cwd=root, env=env)
started = time.monotonic()
last = None
evidence = []
armed = False
saved_at = None
try:
    while child.poll() is None:
        frame = out/'current.ppm'
        if frame.exists() and frame.stat().st_mtime_ns != last:
            stamp = frame.stat().st_mtime_ns
            png = out/'observed.png'
            converted = subprocess.run(['sips','-s','format','png',str(frame),'--out',str(png)], capture_output=True)
            if converted.returncode == 0 and frame.stat().st_mtime_ns == stamp:
                last = stamp
                text = subprocess.run([str(a.ocr.resolve()),str(png)], capture_output=True, text=True, timeout=15)
                words = text.stdout.upper()
                hud = all(word in words for word in ('LAP','POS','BOOST'))
                if hud or armed:
                    name = f'gameplay-{len(evidence):03d}.png'
                    shutil.copyfile(png, out/name)
                    evidence.append(dict(image=name, elapsed=time.monotonic()-started, ocr=text.stdout, hud=hud))
                if hud and not armed:
                    (out/'trigger.txt').write_text('1\n')
                    if not a.stationary: (out/'pad.txt').write_text('0x0200 2000\n')
                    armed = True
                    print('Racing HUD detected; Metal capture armed', flush=True)
        log = out/'native.log'
        if armed and saved_at is None and log.exists() and '[metal capture] saved ' in log.read_text(errors='replace'):
            saved_at = time.monotonic()
            print('Metal capture saved; collecting 10 seconds of follow-up images', flush=True)
        if saved_at and time.monotonic()-saved_at >= 10: break
        time.sleep(1)
finally:
    (out/'evidence.json').write_text(json.dumps(dict(hud_detected=armed, metal_saved=saved_at is not None, images=evidence), indent=2))
    pidfile = out/'native.pid'
    if child.poll() is None and pidfile.exists():
        try: os.killpg(int(pidfile.read_text()), signal.SIGTERM)
        except ProcessLookupError: pass
    try:
        child.wait(timeout=15)
    except subprocess.TimeoutExpired:
        if pidfile.exists():
            try: os.killpg(int(pidfile.read_text()), signal.SIGKILL)
            except ProcessLookupError: pass
        child.wait(timeout=15)
if not saved_at: raise SystemExit('No verified gameplay Metal capture was saved; see evidence.json and native.log')
