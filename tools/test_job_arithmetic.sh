#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
if [ "$#" -gt 1 ]; then
    echo "Usage: $0 [local directory containing segment-N-NN.bin snapshots]" >&2
    exit 2
fi
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
python3 tools/specialize_job_arithmetic.py --test-source "$test_dir/job_arithmetic.inc"
clang -O2 -UNDEBUG -fno-strict-aliasing -fwrapv -ffp-contract=off \
    -Iport -Ivendor/ps3recomp/runtime/spu -Ivendor/ps3recomp/include \
    -I"$test_dir" port/tests/test_job_arithmetic.c -o "$test_dir/replay"
"$test_dir/replay" "$@"
