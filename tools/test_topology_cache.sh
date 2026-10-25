#!/bin/sh
set -eu
cd "$(dirname "$0")/../vendor/ps3recomp"
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
for mode in checked bench; do
  if [ "$mode" = checked ]; then
    flags="-O1 -g -fsanitize=address,undefined"
  else
    flags="-O3 -DTOPOLOGY_BENCH"
  fi
  clang -std=gnu17 $flags -UNDEBUG -fno-strict-aliasing -ffp-contract=off \
    -Iinclude -Ilibs/video libs/video/tests/test_topology_cache.c \
    libs/video/rsx_dispatch.c libs/video/rsx_fp_decompiler.c \
    libs/video/rsx_vp_decompiler.c libs/video/rsx_texture_layout.c \
    libs/video/rsx_vertex_compact.c libs/video/rsx_vertex_packed.c \
    runtime/memory/guest_read_sync.c -lm -lpthread -o "$test_dir/$mode"
  "$test_dir/$mode"
done
