#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
clang -O2 -std=gnu17 -UNDEBUG -fno-strict-aliasing -fwrapv -ffp-contract=off \
    -Iport -Ivendor/ps3recomp/runtime/spu -Ivendor/ps3recomp/include \
    ${TEST_SANITIZE:+-fsanitize=$TEST_SANITIZE} \
    port/tests/test_spu_thread_rtz.c port/spu_thread_rtz.c \
    -o "$work/test"
"$work/test"
