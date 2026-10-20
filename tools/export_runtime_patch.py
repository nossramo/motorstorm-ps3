#!/usr/bin/env python3
"""Preserve tracked edits and new runtime sources against the pinned SDK HEAD."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
sdk = root / 'vendor/ps3recomp'
target = root / 'patches/ps3recomp-local.patch'
parts = [subprocess.check_output(['git', 'diff', '--binary', 'HEAD', '--', '.'], cwd=sdk)]
new = subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard', '-z'], cwd=sdk)
for raw in new.split(b'\0'):
    if not raw:
        continue
    path = raw.decode()
    result = subprocess.run(['git', 'diff', '--no-index', '--binary', '--', '/dev/null', path], cwd=sdk, capture_output=True)
    if result.returncode not in (0, 1):
        raise SystemExit(result.stderr.decode())
    parts.append(result.stdout)
candidate = target.with_suffix('.patch.tmp')
candidate.write_bytes(b''.join(parts))
subprocess.run(['git', 'apply', '--reverse', '--check', str(candidate)], cwd=sdk, check=True)
candidate.replace(target)
print(f'Updated {target}: {target.stat().st_size:,} bytes; reverse-apply check passed.')
