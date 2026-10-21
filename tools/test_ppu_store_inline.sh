#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang++ -std=c++20 -O2 -Iport -Iport/recompiled -Ivendor/ps3recomp/include \
  port/tests/test_ppu_store_inline.cpp -o "$test_dir/test"
for mode in plain size tiny PPU_WWATCH PPU_WVAL SPU_ATOM_EA PS3_NULL_SWEEP; do
  "$test_dir/test" "$mode"
done
