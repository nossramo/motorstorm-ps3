#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -std=gnu17 -O2 -fno-strict-aliasing -ffp-contract=off -UNDEBUG \
  -fsanitize=address,undefined \
  -Ivendor/ps3recomp/include -Ivendor/ps3recomp/libs/video \
  vendor/ps3recomp/libs/video/tests/test_vertex_packed.c \
  vendor/ps3recomp/libs/video/rsx_vertex_packed.c \
  vendor/ps3recomp/libs/video/rsx_vertex_compact.c \
  vendor/ps3recomp/libs/video/rsx_dispatch.c -o "$test_dir/test"
"$test_dir/test"
