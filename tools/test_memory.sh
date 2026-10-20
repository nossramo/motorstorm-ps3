#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build logs
clang -std=c17 -Ivendor/ps3recomp/runtime/syscalls \
  tests/memory_container.c vendor/ps3recomp/runtime/syscalls/sys_memory.c \
  -o build/test_memory_container
./build/test_memory_container > logs/memory-container-tests.log 2>&1
cat logs/memory-container-tests.log
