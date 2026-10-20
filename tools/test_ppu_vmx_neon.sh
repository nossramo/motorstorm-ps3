#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir=$(mktemp -d /private/tmp/motorstorm-vmx.XXXXXX)
trap 'rm -rf "$test_dir"' EXIT
cc -std=gnu17 -O3 -fno-strict-aliasing -ffp-contract=off \
  -I"$root/port" -I"$root/vendor/ps3recomp/runtime/ppu" \
  "$root/port/tests/test_ppu_vmx_neon.c" -o "$test_dir/test"
"$test_dir/test"
