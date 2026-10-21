#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
xcrun --sdk macosx clang -std=gnu17 -O2 -fsanitize=address,undefined \
    -c port/tests/offline_cpu_worker.c -o "$test_dir/cpu-worker.o"
xcrun --sdk macosx clang++ -std=c++20 -O2 -fobjc-arc -framework Foundation \
    -fsanitize=address,undefined \
    'MotorStorm/MotorStorm iOS/OfflineDiagnostics.mm' \
    port/tests/test_offline_diagnostics.mm "$test_dir/cpu-worker.o" -o "$test_dir/offline-test"
"$test_dir/offline-test" "$test_dir/documents"
printf '%s\n' 'Offline diagnostics: counters, live/exited C threads, timer, JSON, rotation, retention and console cap passed.'
