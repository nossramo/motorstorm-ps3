#!/usr/bin/env python3
"""Compatibility entry point for the isolated macOS Xcode runtime build."""
import os
from pathlib import Path
import subprocess
import sys


def main():
    if os.environ.get('PLATFORM_NAME') != 'macosx':
        sys.exit('error: This runtime integration currently supports macOS only.')
    root = Path(__file__).resolve().parents[1]
    # Older project files may still invoke this helper. Never export the
    # live development tree's stale build/game archive for macOS.
    subprocess.run([sys.executable, str(root / 'tools/build_macos_runtime.py')], check=True)


if __name__ == '__main__':
    main()
