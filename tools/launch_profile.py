#!/usr/bin/env python3
"""Shared native launch defaults for Terminal and the Xcode macOS app."""
import json
import os
from pathlib import Path
import shlex


def environment(root):
    values = json.loads((root / 'port/macos-launch.json').read_text())
    return {key: value.replace('${ROOT}', str(root)) for key, value in values.items()}


if __name__ == '__main__':
    root = Path(__file__).resolve().parents[1]
    # Values are shell-quoted, including project paths containing spaces.
    for key, value in environment(root).items():
        print(f'export {key}={shlex.quote(os.environ.get(key) or value)}')
