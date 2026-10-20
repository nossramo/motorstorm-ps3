#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d /private/tmp/motorstorm-remap.XXXXXX)
trap 'rm -rf "$test_dir"' EXIT
cd "$root/vendor/ps3recomp"
cc -std=gnu17 -O1 -g -fsanitize=address,undefined -ffunction-sections -fdata-sections \
  -Iinclude -Ilibs/video libs/video/tests/test_vertex_remap_dense.c \
  libs/video/rsx_vertex_compact.c libs/video/rsx_dispatch.c \
  -Wl,-dead_strip -lm -o "$test_dir/checked"
"$test_dir/checked"
cc -std=gnu17 -O2 -DREMAP_BENCH -ffunction-sections -fdata-sections \
  -Iinclude -Ilibs/video libs/video/tests/test_vertex_remap_dense.c \
  libs/video/rsx_vertex_compact.c libs/video/rsx_dispatch.c \
  -Wl,-dead_strip -lm -o "$test_dir/bench"
"$test_dir/bench"
