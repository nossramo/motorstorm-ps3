#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
python3 tools/specialize_audio_worker.py --generate "$test_dir/worker.c"
python3 tools/specialize_audio_bounds.py --input "$test_dir/worker.c" --test-source "$test_dir/audio_bounds.inc"
clang -O2 -UNDEBUG -fno-strict-aliasing -fwrapv -ffp-contract=off \
    -DMOTORSTORM_AUDIO_WORKER_TEST=1 \
    -Iport -Ivendor/ps3recomp/runtime/spu -Ivendor/ps3recomp/include \
    -I"$test_dir" tests/audio_bounds_replay.c -o "$test_dir/replay"
"$test_dir/replay"
