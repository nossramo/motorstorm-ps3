#!/usr/bin/env python3
"""Start a user-driven game; arm timing or Metal capture when the player is ready.
No automated buttons, screenshot readback, sampler, or automatic game shutdown.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('start', 'arm', 'report'))
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=60)
    parser.add_argument('--capture', action='store_true',
                        help='Start in Metal capture mode instead of performance timing')
    parser.add_argument('--frames', type=int, default=2,
                        help='Complete Metal frames for capture mode (1–4)')
    parser.add_argument('--note', default='', help='Player-selected vehicle/track and intended action')
    parser.add_argument('--env', action='append', default=[], metavar='NAME=VALUE')
    args = parser.parse_args()
    if not 5 <= args.seconds <= 120:
        parser.error('--seconds must be between 5 and 120')
    if not 1 <= args.frames <= 4:
        parser.error('--frames must be between 1 and 4')
    root = Path(__file__).resolve().parents[1]
    out = args.out.resolve()
    trigger = out / 'trigger.txt'
    if args.action == 'start':
        if out.exists():
            parser.error('Use a fresh output directory; existing evidence is preserved')
        env = dict(os.environ)
        # Clear previous automation/capture settings before installing this
        # run's own trace. The player's normal keyboard/controller is untouched.
        for key in list(env):
            if key.startswith(('PAD_', 'PS3_FTRACE', 'PS3RECOMP_METAL_FRAME_DUMP',
                               'PS3RECOMP_METAL_GPU_TRACE')):
                del env[key]
        for setting in args.env:
            key, sep, value = setting.partition('=')
            if not sep or not key.startswith(('RSX_', 'SPU_', 'SPURS_', 'MOTORSTORM_', 'GCM_')):
                parser.error('--env must be a named runtime comparison setting')
            env[key] = value
        out.mkdir(parents=True)
        trigger.write_text('0\n')
        env.update(MOTORSTORM_LOG=str(out / 'native.log'),
                   PS3_FTRACE=f'1:{args.seconds}:{out}/trace.txt',
                   PS3_FTRACE_TRIGGER=str(trigger), PS3_FTRACE_MIN_US='20',
                   RSX_ENGINE_PHASE_TRACE='0', RSX_SURFACE_SEED_TRACE='0',
                   AUDIO_RATE='1', PS3_HLE_TRACE='0')
        if args.capture:
            for key in list(env):
                if key.startswith('PS3_FTRACE'):
                    del env[key]
            (out / 'shaders').mkdir()
            env.update(MTL_CAPTURE_ENABLED='1',
                PS3RECOMP_METAL_GPU_TRACE=str(out / 'race.gputrace'),
                PS3RECOMP_METAL_GPU_TRACE_TRIGGER=str(trigger),
                PS3RECOMP_METAL_GPU_TRACE_AFTER='0',
                PS3RECOMP_METAL_GPU_TRACE_FRAMES=str(args.frames),
                RSX_ENGINE_SHADER_DUMP=str(out / 'shaders'))
        config = {'seconds': args.seconds, 'overrides': args.env,
                  'mode': 'capture' if args.capture else 'timing',
                  'capture_frames': args.frames, 'player_note': args.note,
                  'controls': 'player keyboard/controller; no automated input',
                  'quality': 'normal experimental launcher defaults',
                  'started_at': time.time()}
        with (root / 'build/game/MotorStormRecomp').open('rb') as source:
            config['binary_sha256'] = hashlib.file_digest(source, 'sha256').hexdigest()
        config['launcher_sha256'] = {name: hashlib.sha256((root / name).read_bytes()).hexdigest()
            for name in ('tools/play.command', 'tools/play-experimental.command')}
        with (out / 'launcher.log').open('w') as log:
            child = subprocess.Popen(['sh', 'tools/play-experimental.command'], cwd=root,
                env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        config['pid'] = child.pid
        (out / 'config.json').write_text(json.dumps(config, indent=2) + '\n')
        print(f'Game started (PID {child.pid}). Choose the vehicle and track; arm when ready. Output: {out}')
    elif args.action == 'arm':
        config = json.loads((out / 'config.json').read_text())
        os.kill(config['pid'], 0)
        if trigger.read_text().strip() != '0':
            parser.error('This run has already been armed; start a fresh run for another comparison')
        trigger.write_text('1\n')
        if config.get('mode') == 'capture':
            print(f'Armed {config["capture_frames"]} Metal frames. Capture can briefly stall the game; do not use it for FPS comparisons.')
        else:
            print(f'Armed one {config["seconds"]}-second trace. Keep driving; the game will remain open.')
    else:
        config = json.loads((out / 'config.json').read_text())
        if config.get('mode') == 'capture':
            capture = out / 'race.gputrace'
            log = out / 'native.log'
            if not capture.exists() or not log.exists() or \
                    f'[metal capture] saved {capture}\n' not in log.read_text(errors='replace'):
                parser.error('Metal capture has not completed; inspect native.log for any capture error')
            print(f'Metal capture saved: {capture}. The game remains open; this capture is not a performance benchmark.')
            return
        trace = out / 'trace.txt'
        completed = f'to {trace}\n'
        log = out / 'native.log'
        if not trace.exists() or not log.exists() or not any(
                line.startswith('[ftrace] wrote ') and line.endswith(completed)
                for line in log.open(errors='replace')):
            parser.error('The recording is not complete yet')
        with (out / 'report.txt').open('w') as report:
            subprocess.run(['python3', str(root / 'tools/report_gameplay_profile.py'),
                            str(trace), '--out', str(out / 'report.json')], check=True,
                           stdout=report)
        print(f'Report saved in {out}; the game remains open. Vehicle, track and route need player confirmation.')


if __name__ == '__main__':
    main()
