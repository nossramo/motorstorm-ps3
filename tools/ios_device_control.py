#!/usr/bin/env python3
"""Control the iOS test app on an explicitly selected, connected device.

Uses devicectl and the runtime's existing PAD_FILE interface. Screenshots and
transfers are recorded with host timestamps; neither is a free FPS probe.
This does not alter the macOS runtime or guess which connected phone to use.
"""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import time
import uuid

BUNDLE = 'com.lithium.motorstorm'
PAD = 'ios-test-pad.txt'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', required=True)
    parser.add_argument('--out', type=Path, required=True)
    sub = parser.add_subparsers(dest='action', required=True)
    launch = sub.add_parser('launch', help='Restart into the game with PAD_FILE enabled')
    launch.add_argument('--env', action='append', default=[], metavar='NAME=VALUE')
    pad = sub.add_parser('pad', help='Send one bounded PS3 input; report count is not seconds')
    pad.add_argument('mask', type=lambda s: int(s, 0))
    pad.add_argument('--reports', type=int, default=4)
    pad.add_argument('--axes', type=int, nargs=4, default=[128, 128, 128, 128],
                     metavar=('LX', 'LY', 'RX', 'RY'))
    pad.add_argument('--ack-timeout', type=float, default=10)
    sub.add_parser('screenshot', help='Capture the real device display, outside FPS timing')
    sub.add_parser('collect', help='Copy the app performance logs without restarting it')
    args = parser.parse_args()
    if args.action == 'pad' and (not 0 <= args.mask <= 65535 or
            not 1 <= args.reports <= 2000 or any(not 0 <= v <= 255 for v in args.axes) or
            not 0 < args.ack_timeout <= 60):
        parser.error('Use mask 0..65535, reports 1..2000, axes 0..255 and timeout 0..60 seconds')
    args.out.mkdir(parents=True, exist_ok=True)
    out = args.out.resolve()
    event = {'action': args.action, 'device': args.device, 'bundle': BUNDLE,
             'host_start_utc': datetime.now(timezone.utc).isoformat(),
             'host_start_monotonic_ns': time.monotonic_ns()}
    token = datetime.now(timezone.utc).strftime('%H%M%S') + '-' + uuid.uuid4().hex[:8]
    env = dict(os.environ)
    env.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
    commands = []

    def run(parts):
        number = len(commands)
        result_path = out / f'{token}-{number:02d}.json'
        cmd = ['xcrun', 'devicectl', 'device', *parts, '--device', args.device,
               '--timeout', '60', '--json-output', str(result_path)]
        begin = time.monotonic_ns()
        proc = subprocess.run(cmd, text=True, capture_output=True, env=env, timeout=75)
        (out / f'{token}-{number:02d}.txt').write_text(proc.stdout + proc.stderr)
        commands.append({'command': cmd, 'host_start_ns': begin, 'host_end_ns': time.monotonic_ns(),
                         'returncode': proc.returncode, 'result': result_path.name})
        if proc.returncode:
            raise RuntimeError(proc.stdout + proc.stderr)
        return proc

    def transfer(direction, source, destination):
        return run(['copy', direction, '--source', str(source), '--destination', str(destination),
                    '--domain-type', 'appDataContainer', '--domain-identifier', BUNDLE])

    try:
        if args.action == 'launch':
            settings = {}
            for setting in args.env:
                key, sep, value = setting.partition('=')
                if not sep or not key or key == 'PAD_FILE':
                    parser.error('--env requires NAME=VALUE; PAD_FILE is owned by this helper')
                settings[key] = value
            settings['PAD_FILE'] = PAD
            neutral = out / f'{token}-neutral.txt'
            neutral.write_text('0x0000 1 128 128 128 128\n')
            transfer('to', neutral, f'Documents/{PAD}')
            event['environment'] = settings
            # Put devicectl options before the bundle/argument terminator.
            result = out / f'{token}-launch.json'
            cmd = ['xcrun', 'devicectl', 'device', 'process', 'launch', '--device', args.device,
                   '--timeout', '60', '--json-output', str(result), '--terminate-existing',
                   '--environment-variables', json.dumps(settings), BUNDLE, '--', '--start-game']
            begin = time.monotonic_ns()
            proc = subprocess.run(cmd, text=True, capture_output=True, env=env, timeout=75)
            (out / f'{token}-launch.txt').write_text(proc.stdout + proc.stderr)
            commands.append({'command': cmd, 'host_start_ns': begin, 'host_end_ns': time.monotonic_ns(),
                             'returncode': proc.returncode, 'result': result.name})
            if proc.returncode:
                raise RuntimeError(proc.stdout + proc.stderr)
        elif args.action == 'pad':
            command = out / f'{token}-pad.txt'
            command.write_text(f'0x{args.mask:04x} {args.reports} ' + ' '.join(map(str, args.axes)) + '\n')
            event.update(mask=args.mask, reports=args.reports, axes=args.axes)
            transfer('to', command, f'Documents/{PAD}')
            deadline = time.monotonic() + args.ack_timeout
            acknowledged = False
            while time.monotonic() < deadline:
                received = out / f'{token}-pad-readback.txt'
                transfer('from', f'Documents/{PAD}', received)
                if received.read_bytes() == b'':
                    acknowledged = True
                    break
                time.sleep(0.15)
            event['consumed_by_runtime'] = acknowledged
            if not acknowledged:
                raise RuntimeError('Input was copied but not consumed before timeout; it may still be pending')
            # Consumption acknowledges acceptance, not the end of the held input.
        elif args.action == 'screenshot':
            path = out / f'{token}-screen.png'
            run(['capture', 'screenshot', '--destination', str(path)])
            event['image'] = str(path)
        elif args.action == 'collect':
            path = out / f'{token}-performance'
            transfer('from', 'Documents/logs/performance', path)
            event['performance'] = str(path)
        event['success'] = True
    except Exception as error:
        event.update(success=False, error=str(error))
        raise
    finally:
        event.update(host_end_utc=datetime.now(timezone.utc).isoformat(),
                     host_end_monotonic_ns=time.monotonic_ns(), commands=commands)
        with (out / 'events.jsonl').open('a') as f:
            f.write(json.dumps(event) + '\n')
        print(json.dumps({k: v for k, v in event.items() if k != 'commands'}, indent=2))


if __name__ == '__main__':
    main()
