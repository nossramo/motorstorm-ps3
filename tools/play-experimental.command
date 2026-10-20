#!/bin/sh
# Native performance paths through run 131; sustained 30 FPS still under test.
set -eu
cd "$(dirname "$0")/.."
export MOTORSTORM_NATIVE_JOBS=1
export RSX_ENGINE_CPU_READBACK=demand
export RSX_ENGINE_CPU_READBACK_CACHE=1
export RSX_METAL_DIRECT_STAGE=1
export RSX_METAL_GPU_VERTEX_FETCH=1
export SPU_MFC_OWNER_CACHE=1
export RSX_GPU_PACKED_DEPTH=1
export RSX_TEXTURE_XXH3=1
export RSX_GPU_COLOR_CROP=1
export RSX_GPU_COLOR_REINTERPRET=1
export RSX_VERTEX_DENSE_REMAP=1
export PS3_GAMEPLAY_QOS=1
export SPURS_TARGETED_WAKE=0
export MOTORSTORM_JOB_EVENT_WAIT=1
export MOTORSTORM_GPU_READ_YIELD=1
export RSX_METAL_EARLY_RECORDS=512
export RSX_FP_METADATA_CACHE=1
# Reuse lengths only after validating every shader instruction.
export RSX_FP_CACHED_SIZE="${RSX_FP_CACHED_SIZE:-1}"
# Reuse exact instruction metadata when the same shader moves in guest memory.
export RSX_FP_RELOCATE_CACHE="${RSX_FP_RELOCATE_CACHE:-1}"
# Initialize linear ARGB/B8 surfaces using their actual guest format and pitch.
export RSX_SURFACE_TYPED_SEED="${RSX_SURFACE_TYPED_SEED:-1}"
# Exact ARM64 arithmetic for the signature-checked culling block.
export MOTORSTORM_CULL_FAST_BOUNDS="${MOTORSTORM_CULL_FAST_BOUNDS:-1}"
# Ignore unused texture dimensions when identifying a fragment pipeline.
export RSX_FP_SAMPLED_DIMENSIONS="${RSX_FP_SAMPLED_DIMENSIONS:-1}"
export PS3RECOMP_METAL_FPS=1
# Preserve every display-clock tick independently of render-thread work.
# These are project launch defaults; the shared SDK keeps its legacy defaults.
export MOTORSTORM_SPU_SEMAPHORE_WAIT="${MOTORSTORM_SPU_SEMAPHORE_WAIT:-1}"
export GCM_VBLANK_COUNTED="${GCM_VBLANK_COUNTED:-1}"
export MOTORSTORM_VBLANK_THREAD="${MOTORSTORM_VBLANK_THREAD:-1}"
export MOTORSTORM_GCM_CALLBACK_THREAD="${MOTORSTORM_GCM_CALLBACK_THREAD:-1}"
export PS3RECOMP_MSL_CACHE="${PS3RECOMP_MSL_CACHE:-$PWD/logs/msl-translation-cache}"
mkdir -p "$PS3RECOMP_MSL_CACHE"
exec sh tools/play.command
