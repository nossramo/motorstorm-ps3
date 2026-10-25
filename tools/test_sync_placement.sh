#!/bin/sh
set -eu
cd "$(dirname "$0")/../vendor/ps3recomp"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
clang -std=gnu17 -O2 -UNDEBUG ${TEST_SANITIZE:+-fsanitize=$TEST_SANITIZE} -ffunction-sections -Wl,-dead_strip \
  -Iinclude -Iruntime/spu -Iruntime/platform \
  runtime/spu/tests/test_sync_mutex_park.c libs/sync/cellSync.c \
  runtime/spu/spu_drain.c runtime/spu/spu_lockstep.c runtime/spu/spu_coherency.c \
  runtime/spu/spu_workload.c runtime/spu/spurs_policy.c runtime/spu/spurs_job.c \
  runtime/spu/spu_tsp_weak.c runtime/spu/spu_vm_pagemap.c runtime/spu/spurs_policy_blob_weak.c \
  runtime/spu/spu_interp.c runtime/platform/win32_compat.c -lpthread -lm -o "$work/test"
for stripes in 1 64; do
  for park in 0 1; do
    SPU_LOCKLINE_HOST=1 SPU_LOCKLINE_STRIPES=$stripes CELL_SYNC_MUTEX_PARK=$park "$work/test"
  done
done
