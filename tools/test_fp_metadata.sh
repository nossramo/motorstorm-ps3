#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -std=gnu17 -O2 -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Ivendor/ps3recomp/include -Ivendor/ps3recomp/libs/video \
  vendor/ps3recomp/libs/video/tests/test_fp_metadata.c \
  vendor/ps3recomp/libs/video/rsx_fp_decompiler.c -o "$test_dir/test"
"$test_dir/test"
