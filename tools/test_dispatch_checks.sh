#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -O2 -std=gnu17 -Ivendor/ps3recomp/include -Ivendor/ps3recomp/runtime/spu \
  vendor/ps3recomp/runtime/spu/tests/test_dispatch_checks.c -Wl,-dead_strip \
  -o "$test_dir/test"
"$test_dir/test"
