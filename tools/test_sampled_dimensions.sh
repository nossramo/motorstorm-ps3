#!/bin/sh
set -eu
cd "$(dirname "$0")/../vendor/ps3recomp"
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
for test_name in test_sampled_dimensions test_shadow_engine test_rsx_draw_engine; do
clang -std=gnu17 -O1 -g ${CFLAGS:-} \
  -Iinclude -Ilibs/video \
  "libs/video/tests/$test_name.c" \
  libs/video/rsx_draw_engine.c libs/video/rsx_dispatch.c \
  libs/video/rsx_fp_decompiler.c libs/video/rsx_vp_decompiler.c \
  libs/video/rsx_texture_layout.c \
  libs/video/rsx_vertex_compact.c libs/video/rsx_vertex_packed.c \
  runtime/memory/guest_read_sync.c \
  -lm -lpthread -o "$test_dir/$test_name"
RSX_FP_SAMPLED_DIMENSIONS=1 "$test_dir/$test_name"
done
