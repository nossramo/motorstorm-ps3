#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <local directory containing worker *-before.bin snapshots>" >&2
    exit 2
fi
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
python3 tools/specialize_audio_worker.py --generate "$test_dir/worker.c"
clang -O2 -UNDEBUG -fno-strict-aliasing -ffp-contract=off \
    -DMOTORSTORM_AUDIO_WORKER_TEST=1 \
    -Iport -Ivendor/ps3recomp/runtime/spu -Ivendor/ps3recomp/include \
    -Iport/spu/spu_1136862334bb6208f794040f3a7f26e4991cd6b00eac6229331432566f1535d1 \
    tests/audio_worker_replay.c "$test_dir/worker.c" -o "$test_dir/replay"
"$test_dir/replay" "$1"/*-before.bin
