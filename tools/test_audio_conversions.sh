#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -O2 -UNDEBUG -fsanitize=undefined -fno-sanitize-recover=all \
  -Iport -Ivendor/ps3recomp/runtime/spu -Ivendor/ps3recomp/include \
  tests/audio_conversions.c -o "$test_dir/check"
"$test_dir/check"
