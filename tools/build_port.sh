#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
cmake -S port -B build/game -G Ninja \
  -DPS3RECOMP_DIR="$PWD/vendor/ps3recomp" -DCMAKE_BUILD_TYPE=Release \
  -DPS3RECOMP_FFMPEG_RUNTIME=ON
cmake --build build/game -j "${BUILD_JOBS:-4}"
