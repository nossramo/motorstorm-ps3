#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
for test in test_metal_vertex_snapshot test_metal_shared_stage; do
  xcrun clang -O2 -UNDEBUG -fobjc-arc -ffp-contract=off -DPS3_PASSIVE_METRICS="${TEST_PASSIVE_METRICS:-1}" \
    -Ivendor/ps3recomp/include -Ivendor/ps3recomp/libs/video \
    "vendor/ps3recomp/libs/video/tests/$test.m" \
    vendor/ps3recomp/libs/video/rsx_texture_layout.c \
    vendor/ps3recomp/libs/video/rsx_vertex_packed.c -Wl,-dead_strip \
    -framework Metal -framework Cocoa -framework QuartzCore -o "$test_dir/$test"
  "$test_dir/$test"
done
xcrun clang -O2 -UNDEBUG -fobjc-arc -ffp-contract=off \
  -Ivendor/ps3recomp/include -Ivendor/ps3recomp/libs/video \
  vendor/ps3recomp/libs/video/tests/test_metal_vertex_packed.m \
  vendor/ps3recomp/libs/video/rsx_vertex_packed.c \
  vendor/ps3recomp/libs/video/rsx_vertex_compact.c \
  vendor/ps3recomp/libs/video/rsx_dispatch.c \
  -framework Metal -framework Foundation -o "$test_dir/formats"
"$test_dir/formats"
