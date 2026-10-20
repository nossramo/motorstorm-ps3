#!/usr/bin/env python3
"""Diagnostic for the 2026-10-20 strict MotorStorm capture, not a rendering fix.

Redirect only the exposure-history render target to unused scratch memory.
Later texture reads then use recorded guest bytes instead of the freshly
rendered GPU surface. All source texture bindings and bytes remain unchanged.
This deliberately changes the replay workload to test a hypothesis; its output
must never be treated as a faithful replay or a gameplay performance result.
"""
import argparse
import hashlib
from pathlib import Path

from rsx_capture import export, methods, read_capture


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=Path)
    p.add_argument('output', type=Path, help='Separate diagnostic MSRX output')
    a = p.parse_args()
    if a.capture.resolve() == a.output.resolve():
        p.error('Keep the original capture; choose a separate output')
    expected = '686d383e39df63359ba13d597ba40065cb1639dd87a42016f8481e796a035e2a'
    if hashlib.sha256(a.capture.read_bytes()).hexdigest() != expected:
        p.error('This probe is validated only for the recorded buggy-strict fixture')
    cap = read_capture(a.capture)
    commands, changed = [], []
    for i, method, value, ids, tile, display in methods(cap):
        if method == 0x210 and value == 0x01fb7700:
            value = 0x0ff00000
            changed.append(i)
        # Decode and re-encode complete single-method packets. This preserves
        # non-incrementing uploads and the location of every memory update.
        word = 0 if method is None else (1 << 18) | method
        commands.append((word, value, ids, tile, display))
    if changed != [368339]:
        p.error(f'Unexpected exposure commands: {changed}')
    cap['commands'] = commands
    export(cap, a.output)
    print(f'Diagnostic only: redirected exposure target at command {changed[0]}')


if __name__ == '__main__':
    main()
