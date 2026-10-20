#!/bin/sh
set -eu
cd "$(dirname "$0")/../vendor/ps3recomp"
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -std=gnu17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iinclude -Ilibs/video \
  libs/video/tests/test_surface_seed.c libs/video/rsx_dispatch.c \
  libs/video/rsx_fp_decompiler.c libs/video/rsx_vp_decompiler.c \
  libs/video/rsx_texture_layout.c libs/video/rsx_vertex_compact.c \
  libs/video/rsx_vertex_packed.c runtime/memory/guest_read_sync.c \
  -lm -lpthread -o "$test_dir/test"
"$test_dir/test"
