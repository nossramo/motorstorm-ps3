#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -std=gnu17 -Ivendor/ps3recomp/include port/tests/test_audio_output.c \
  -Wl,-dead_strip -o "$test_dir/audio-output"
"$test_dir/audio-output"
