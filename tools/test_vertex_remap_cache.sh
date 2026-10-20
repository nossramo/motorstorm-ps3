#!/bin/sh
set -eu
cd "$(dirname "$0")/../vendor/ps3recomp"
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
for mode in checked bench; do
  if [ "$mode" = checked ]; then
    flags="-O1 -g -fsanitize=address,undefined"
  else
    flags="-O3 -DREMAP_BENCH"
  fi
  clang -std=gnu17 $flags -UNDEBUG -Iinclude -Ilibs/video \
    libs/video/tests/test_vertex_remap_cache.c libs/video/rsx_dispatch.c \
    -lm -o "$test_dir/$mode"
  "$test_dir/$mode"
done
