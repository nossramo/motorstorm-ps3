#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang -std=gnu17 -O2 -Wall -Wextra ${TEST_SANITIZE:+-fsanitize=$TEST_SANITIZE} \
  vendor/ps3recomp/runtime/spu/tests/test_async_reuse.c -lpthread -lm -o "$test_dir/test"
"$test_dir/test"
