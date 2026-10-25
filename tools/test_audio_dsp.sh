#!/bin/sh
# Replay a captured input, channel transcript, and expected output without boot.
set -eu
cd "$(dirname "$0")/.."
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
if [ "$#" -ne 1 ]; then
  echo "Usage: $0 <local capture directory containing 30720-{before,after,channels}.bin>" >&2
  exit 2
fi
python3 tools/lift_motorstorm_audio.py
test_dir=$(mktemp -d)
trap 'rm -rf "$test_dir"' EXIT
# Optional production thread-RTZ arithmetic, including its retained integer
# fallback (select MOTORSTORM_SPU_WIDE_RTZ=0 before launching this script).
rtz_flags=""
rtz_source=""
if [ "${TEST_THREAD_RTZ:-0}" = 1 ]; then
  rtz_flags="-DMOTORSTORM_SPU_THREAD_RTZ -Iport -include port/spu_thread_rtz.h"
  rtz_source="port/spu_thread_rtz.c"
fi
for module in motorstorm_audio_dsp motorstorm_audio_2FEF0 motorstorm_audio_32770; do
  clang -O2 -UNDEBUG -fno-strict-aliasing -ffp-contract=off \
    -Ivendor/ps3recomp/runtime/spu -Ivendor/ps3recomp/include \
    -include port/spu_audio_conversions.h $rtz_flags \
    -c "port/spu/$module/spu_recomp.c" -o "$test_dir/$module.o"
done
clang -O2 -UNDEBUG -fno-strict-aliasing -ffp-contract=off \
  $rtz_flags \
  -Ivendor/ps3recomp/runtime/spu -Ivendor/ps3recomp/include \
  tests/audio_dsp_replay.c "$test_dir"/*.o \
  port/spu/motorstorm_audio_register.c \
  vendor/ps3recomp/runtime/spu/spu_interp.c $rtz_source -lm -o "$test_dir/replay"
for entry in 30720 2FEF0 32770; do
  MOTORSTORM_NATIVE_AUDIO=1 "$test_dir/replay" "$1/$entry-before.bin" "$1/$entry-after.bin" "$1/$entry-channels.bin" 0
  MOTORSTORM_NATIVE_AUDIO=1 "$test_dir/replay" "$1/$entry-before.bin" "$1/$entry-after.bin" "$1/$entry-channels.bin" 1
done
