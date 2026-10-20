#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -std=gnu17 -Ivendor/ps3recomp/include $(sdl2-config --cflags) \
  -I"$(sdl2-config --prefix)/include" vendor/ps3recomp/libs/audio/tests/test_audio_ring.c \
  $(sdl2-config --libs) -Wl,-dead_strip -o "$test_dir/audio-ring"
"$test_dir/audio-ring"
