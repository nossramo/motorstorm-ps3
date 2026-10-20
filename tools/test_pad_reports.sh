#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -std=gnu17 ${CFLAGS:-} -Ivendor/ps3recomp/include $(sdl2-config --cflags) \
  -I"$(sdl2-config --prefix)/include" port/tests/test_pad_reports.c \
  vendor/ps3recomp/runtime/memory/guest_read_sync.c \
  -Wl,-dead_strip -o "$test_dir/pad"
"$test_dir/pad"
"$test_dir/pad" --script-stop
"$test_dir/pad" --keyboard
