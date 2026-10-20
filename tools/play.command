#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export PS3_TITLE='MotorStorm — Z: confirm · X: back · Enter: Start'
export PS3_HLE_UNRESOLVED=fail
# MotorStorm submits work produced by SPU jobs. Honour RSX acquire waits.
export GCM_SEMA_ACQUIRE="${GCM_SEMA_ACQUIRE:-1}"
# The game's exposure calculation reads GPU colour data directly on the PPU.
export RSX_ENGINE_CPU_READBACK="${RSX_ENGINE_CPU_READBACK:-1}"
export PS3RECOMP_FFMPEG_AVCODEC="${PS3RECOMP_FFMPEG_AVCODEC:-/opt/homebrew/lib/libavcodec.dylib}"
export PS3RECOMP_FFMPEG_AVUTIL="${PS3RECOMP_FFMPEG_AVUTIL:-/opt/homebrew/lib/libavutil.dylib}"
mkdir -p logs
printf 'MotorStorm research build. Arrows: navigate, Z: confirm, X: back, Enter: Start.\n'
printf 'Stop with Ctrl-C in this terminal. Output: logs/play.log\n'
exec ./build/game/MotorStormRecomp ./game/PS3_GAME/USRDIR/EBOOT.elf > logs/play.log 2>&1
