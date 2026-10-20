#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -Ivendor/ps3recomp/include -c vendor/ps3recomp/runtime/memory/guest_read_sync.c -o "$test_dir/sync.o"
clang -Ivendor/ps3recomp/include port/tests/test_gpu_dirty_ranges.c "$test_dir/sync.o" -o "$test_dir/ranges"
"$test_dir/ranges"
clang++ -std=c++20 -Ivendor/ps3recomp/include -Ivendor/ps3recomp/libs/video \
  port/tests/test_gpu_read_sync.cpp port/gpu_read_sync.cpp "$test_dir/sync.o" -o "$test_dir/test"
"$test_dir/test"
printf 'GPU read request queue: PASS\n'
python3 vendor/ps3recomp/tools/test_rsx_immediate_vertices.py
python3 vendor/ps3recomp/tools/test_gcm_flip_stream.py
