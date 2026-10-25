#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
xcrun --sdk macosx clang++ -std=c++20 -O2 -fobjc-arc -framework Foundation \
    -DMOTORSTORM_IOS_DIAGNOSTICS=0 -DPS3_PASSIVE_METRICS=0 \
    'MotorStorm/MotorStorm iOS/OfflineDiagnostics.mm' \
    port/tests/test_ios_play_build.mm -o "$test_dir/play-test"
"$test_dir/play-test"
xcrun nm "$test_dir/play-test" > "$test_dir/symbols.txt"
if rg 'MSOfflineDiagnostics|MSDiagnostics(Start|Export)|g_ps3_(passive_metrics|metric_threads|waits)|_clock_gettime|_thread_info' "$test_dir/symbols.txt"; then
    printf '%s\n' 'Unexpected recording, clock sampling or CPU attribution in play build.' >&2
    exit 1
fi
printf '%s\n' 'PASS: play build has no recorder, ZIP exporter, passive counters, thread clocks or diagnostic argument evaluation.'
