#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
clang -O2 -std=gnu17 -fno-strict-aliasing -fwrapv \
    -Iport -Ivendor/ps3recomp/runtime/spu -Ivendor/ps3recomp/include \
    ${TEST_SANITIZE:+-fsanitize=$TEST_SANITIZE} \
    port/tests/test_spu_shufb_neon.c -o "$work/test"
"$work/test" "$@"
if [ -n "${SHUFB_ASM_OUT:-}" ]; then
    clang -O2 -std=gnu17 -fno-strict-aliasing -fwrapv -S \
        -Iport -Ivendor/ps3recomp/runtime/spu -Ivendor/ps3recomp/include \
        port/tests/test_spu_shufb_neon.c -o "$SHUFB_ASM_OUT"
fi
