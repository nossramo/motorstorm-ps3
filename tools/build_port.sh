#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
# The pinned snapshot contains the original builder. iOS continues to build
# the development tree through build_xcode_runtime.py.
exec python3 tools/build_macos_runtime.py --build-only
