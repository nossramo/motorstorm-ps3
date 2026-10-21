#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
# Dead-strip unrelated CRT functions. Generate only the runtime ABI header;
# this regression does not need any copyrighted game code.
python3 - "$test_dir" <<'PY'
import sys
from pathlib import Path
sys.path.insert(0, 'vendor/ps3recomp/tools')
from ppu_lifter import HEADER_PREAMBLE
Path(sys.argv[1], 'ppu_recomp.h').write_text(HEADER_PREAMBLE)
PY
clang -O2 -Ivendor/ps3recomp/include -c \
  vendor/ps3recomp/runtime/platform/win32_compat.c -o "$test_dir/compat.o"
clang++ -std=c++20 -O2 -Wl,-dead_strip \
  -I"$test_dir" -Ivendor/ps3recomp/include \
  port/tests/test_lwmutex_contention.cpp "$test_dir/compat.o" -o "$test_dir/test"
"$test_dir/test"
