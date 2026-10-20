#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -fobjc-arc -Ivendor/ps3recomp/include -Ivendor/ps3recomp/libs/video \
  vendor/ps3recomp/libs/video/tests/test_metal_depth_bias.m \
  vendor/ps3recomp/libs/video/rsx_texture_layout.c \
  vendor/ps3recomp/libs/video/rsx_vertex_packed.c -Wl,-dead_strip \
  -framework Metal -framework Cocoa -framework QuartzCore -o "$test_dir/test"
"$test_dir/test"
