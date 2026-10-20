#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
export PS3_TITLE='MotorStorm — arrows: drive/menu · Space: boost · Shift: handbrake'
export PS3_HLE_UNRESOLVED=fail
# Multistream loads DSP code into SPU local store at runtime. Continue through
# the interpreter for those unlifted overlays, then rejoin the compiled code.
export SPU_INTERP_UNLIFTED="${SPU_INTERP_UNLIFTED:-1}"
# MotorStorm submits work produced by SPU jobs. Honour RSX acquire waits.
export GCM_SEMA_ACQUIRE="${GCM_SEMA_ACQUIRE:-1}"
# The game's exposure calculation reads GPU colour data directly on the PPU.
export RSX_ENGINE_CPU_READBACK="${RSX_ENGINE_CPU_READBACK:-1}"
export PS3RECOMP_FFMPEG_AVCODEC="${PS3RECOMP_FFMPEG_AVCODEC:-/opt/homebrew/lib/libavcodec.dylib}"
export PS3RECOMP_FFMPEG_AVUTIL="${PS3RECOMP_FFMPEG_AVUTIL:-/opt/homebrew/lib/libavutil.dylib}"
mkdir -p logs
play_log="${MOTORSTORM_LOG:-logs/play.log}"
printf 'MotorStorm research build. Drive: arrows, W/S, I/J/K/L. Space: boost; Shift: handbrake.\n'
printf 'Menus: arrows navigate, Enter confirm/start, Escape back or pause/resume.\n'
printf 'Stop with Ctrl-C in this terminal. Output: %s\n' "$play_log"
exec ./build/game/MotorStormRecomp ./game/PS3_GAME/USRDIR/EBOOT.elf > "$play_log" 2>&1
