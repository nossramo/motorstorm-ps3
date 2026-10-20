#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
clang++ -std=c++17 -Wall -Wextra port/keyboard_context.cpp port/tests/test_keyboard_context.cpp -o "$test_dir/context"
"$test_dir/context"
