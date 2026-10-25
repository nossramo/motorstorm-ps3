#!/bin/sh
set -eu
cd "$(dirname "$0")/../vendor/ps3recomp"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
clang -std=gnu17 -O2 ${TEST_SANITIZE:+-fsanitize=$TEST_SANITIZE} -I include -I runtime/spu -I runtime/platform \
  "${TEST_SOURCE:-runtime/spu/tests/test_spu_lifted_start.c}" \
  runtime/spu/spu_channels.c runtime/spu/spu_drain.c \
  runtime/spu/spu_lockstep.c runtime/spu/spu_coherency.c \
  runtime/spu/spu_workload.c runtime/spu/spurs_policy.c \
  runtime/spu/spurs_job.c runtime/spu/spu_tsp_weak.c \
  runtime/spu/spu_vm_pagemap.c runtime/spu/spurs_policy_blob_weak.c \
  runtime/spu/spu_interp.c runtime/spu/spu_lifted_thread.c \
  runtime/syscalls/spu_fallback.c runtime/syscalls/sdk_version_weak.c \
  runtime/platform/win32_compat.c -lpthread -o "$work/test"
"$work/test"
