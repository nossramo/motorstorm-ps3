# MotorStorm macOS port investigation

Target: MotorStorm BCES00006, `APP_VER=03.10`, Apple Silicon macOS first.
This is a work in progress, not a playable port. Boot, menus and one playable
track are separate milestones and must be demonstrated with the actual game.

## Audio repair — 20 October 2026

The user confirmed audible game audio after the repair. The existing SDL/CoreAudio
output was opening correctly, but the Multistream SPU task terminated on a call
into runtime-loaded DSP code at LS `0x2F810`. Both launchers now enable
`SPU_INTERP_UNLIFTED=1` through `tools/play.command`: those unlifted overlays run
through the existing interpreter and return to compiled SPU code. This is still
a compatibility path, not a claim that all audio code is native ARM64.
`SPU_INTERP_UNLIFTED=0` explicitly disables the fallback.

Audio output queries also used incorrect guest struct layouts, making the game
read zero channels/zero Hz and reject the reported modes. The corrected device
header, sound-mode stride/layout and 20-byte output state follow the ABI
documented in [RPCS3's audio output definitions](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellAudioOut.h).
Configure/GetConfiguration/GetState now agree on the selected LPCM mode.

`sh tools/test_audio_output.sh` checks guest byte offsets, buffer boundaries,
48 kHz capabilities and configuration round-trips; it failed before the fix and
passes afterward. Two bounded runs are saved locally under `logs/audio-01` and
`logs/audio-02`. The first had silent output and a terminated audio worker.
The fallback-enabled run produced nonzero stereo samples through CoreAudio,
and the user confirmed hearing sound. Extended race coverage and the audio
fallback's performance cost have not been measured.

On macOS, `AUDIO_PEAK=1` now reports mixed-buffer levels; `AUDIO_WAV=<path>`
records raw float32 stereo 48 kHz output (despite the legacy variable name,
it is not a WAV container). These diagnostics remain opt-in and recordings
must stay under ignored `logs/`. A missing optional firmware ATRAC module is
still reported; complete codec coverage is not established.

## Source backup and paused status

This is a small source-only backup. See [BACKUP.md](BACKUP.md) for the exact
scope and restoration steps. Game files, generated game code, runtime
checkouts, binaries and captures are intentionally excluded. Restore the
pinned runtime with `python3 tools/bootstrap_runtime.py` before regenerating.

Automatic optimization and gameplay tests were stopped at the user's request.
The last normal eight-second probe (run 137) measured **24.75 guest FPS**,
40.544 ms mean, 49.413 ms p95 and 89.108 ms maximum frame time; RSX CPU averaged
27.973 ms/frame and GPU execution 14.078 ms/frame. These overlap and must not
be added. A subsequent comparison run was stopped, so this is not a controlled
regression result. Stable 30 FPS, a complete lap, 60 FPS and iOS remain unproven.

The newer B8 mip-feedback candidate passed targeted tests and a diagnostic
Metal-validation run, but remains opt-in (`RSX_B8_SURFACE_FEEDBACK=1`) and is
not enabled by the launcher. Shadow/foliage temporal quality remains
unverified. The dated entries below are research history, including unfinished
experiments; they do not indicate currently running tests.

## Current status — 20 October 2026, afternoon

The native ARM64/Metal executable reaches a real race, renders the buggy,
track and HUD, and responds to throttle, steering and braking. Two HUD-gated
20-second probes measure **29.85 guest FPS at 1280×720**; the newer has
33.51 ms mean, 34.86 ms p95 and 42.00 ms maximum frame time
(`logs/profile-106-clock-dispatcher`). A subsequent 30-second window starting
at the opening grid measures **28.43 FPS**, 35.17 ms mean, 46.55 ms p95 and
121.19 ms maximum (`logs/profile-107-early-rolling`). Its largest hitch includes
92.81 ms of previously uncached shader translation.

A recent **eight-second opening-grid window measures 26.25 FPS**,
38.16 ms mean, 47.56 ms p95 and 81.79 ms maximum
(`logs/profile-111-sampled-dimensions-grid`). It has zero shader-cache misses,
but RSX CPU costs 29.26 ms/frame versus 13.52 ms of GPU execution. Shortening
the window to concentrate on the busy grid exposes work hidden by a longer
average. The first eight seconds of run 107 also average 38.58 ms over its
fully contained frame intervals; these are not controlled regression/speedup
comparisons. Sustained 30 FPS on the grid remains an explicit gap.
An opt-in reference-remap cache subsequently measures 25.5 FPS in run 114;
its paired cache-off run encounters a cold shader hitch. Phase attribution
shows a modest geometry saving, but no robust whole-frame gain is established,
so this additional option remains disabled in the experimental launcher.
An exact, guarded ARM64 culling specialization subsequently measures
27.25 FPS in run 117 versus 26.125 FPS with the previous arithmetic in run
118. Both have warm shader caches, but race timing/traffic differ. This is
promising short-run evidence, not an established whole-game speedup.
Follow-up run 119 finds that 98.944% of sampled culling blocks qualify for
this exact path. The experimental launcher now enables it together with
validated fragment-program length reuse (`RSX_FP_CACHED_SIZE=1`).
Run 120 with both options measures **27.25 FPS**, 36.70 ms mean, 46.08 ms p95,
84.69 ms maximum, RSX CPU 27.03 ms/frame and GPU execution 13.84 ms/frame.
The same binary with only length reuse disabled measures 26.25 FPS and RSX
CPU 29.06 ms/frame in a warm-cache repeat (run 122). Traffic and collisions
still differ; these short runs do not establish a precise whole-game speedup.
The newer instruction-validated relocation cache measures **28.0 FPS** in
run 125 versus 27.0 FPS disabled in run 126. RSX CPU averages 26.66 versus
28.36 ms/frame, with warm shader caches and identical executables/settings
except this option. The experimental launcher now enables it as
`RSX_FP_RELOCATE_CACHE=1`. Post-window car positions/traffic still differ;
this supports retaining the optimization, not claiming stable 30 FPS.
An additional exact vertex-program metadata cache is tested but remains
disabled: run 128 measures 27.25 FPS enabled versus 28.25 disabled in run 129.
Its small RSX CPU saving does not establish a whole-game benefit. The next
correctness/performance investigation separates guest-surface initialization
from Metal allocation; a 4096×4096 B8 target coincides with a large hitch and
unsupported texture feedback.
The surface investigation now confirms an incorrect ARGB decode of B8
initialization data. Correct typed/pitched initialization reduces the nine
surface conversions in that hitch from 30.22 to 5.20 ms CPU. The same-binary
warm comparison measures **28.625 FPS** in run 131, versus 28.25 in run 130;
maximum frame time falls from 83.52 to 52.45 ms. The experimental launcher
enables `RSX_SURFACE_TYPED_SEED=1`. These are still eight-second background
probes with different traffic; B8 self-feedback and temporal quality remain
unresolved.

These runs stay behind other apps, so guest throughput is not a visible
interactive-FPS result. The authored controls eventually reverse or collide
with the cliff, and do not establish a completed lap or sustained 30 FPS
through a full race. The later window has different traffic and collisions,
so its lower throughput is not a controlled regression claim. Temporal
shadow/foliage artifacts, 60 FPS and an iOS build remain unverified.

The display-clock repair separates the 59.94 Hz vblank producer from rendering,
preserves every queued tick and uses the existing serialized callback
dispatcher. Long vblank intervals disappear in these probes. An earlier
guarded native SPU wait also removes most CPU work in one semaphore-delay loop.
The experimental launcher now includes these measured paths and the shader
translation disk cache; the baseline launcher remains available for comparison.
It also enables filtering unused fragment texture dimensions from the pipeline
key, after CPU tests, recorded-frame comparison and live Metal validation.

The largest recent repairs replace an incorrect CPU flip-wait with the actual
firmware's ordered RSX semaphore commands, restore the game's five SPURS lanes and
keep packed-depth and RGBA8-to-RG16F texture aliases on the GPU. Shared Metal
staging, GPU vertex conversion, MFC ownership caching and exact ARM64 arithmetic
specializations are also available. The executable is compiled ARM64 code with
PS3 runtime semantics and RSX translation still present; it is not a reconstruction
of the original game engine without a compatibility layer.

Earlier run 79 reports 15.63 ms RSX CPU, 63.58 ms SPU-policy CPU across concurrent
workers, 0.083 ms interpreter CPU, 13.02 ms job-wait wall time and 11.12 ms GPU
execution per frame. These overlapping/thread-aggregate figures **must not be
added** to obtain frame time. The remaining interpreter is not the primary
bottleneck. CPU flip-wait time falls from 18.20 ms to 0.00025 ms; the graphics
queue still waits in order for flip completion. GPU-read dependency is 1.95 ms.
`tools/play-experimental.command` launches the current experimental options.
The exact settings and executable hash are saved alongside each run. Earlier
sections below are chronological research history, not current success claims.

## Layout

- `game/`: local game files, including the original SELF and decrypted ELF.
- `vendor/ps3recomp/`: upstream runtime and recompiler; pinned commit recorded
  in `research/ps3recomp-commit.txt`.
- `vendor/MSMV-unpacker/`: archive format reference. Its documented full
  filename extraction supports 1.00, not this 3.10 build without cache_ps3.txt.
- `port/`: native runner, CMake project and generated PPU/SPU code.
- `research/`: input hashes, function/import inventories and archive index.
- `logs/`: build, validation and bounded boot attempt output.

Generated game code, binaries, local game data and external tools are ignored
by `.gitignore`. The source-only backup preserves runtime changes as a patch
against the pinned upstream commit.

## Reproduce

Requirements: Xcode, CMake, Ninja, Python 3, SDL2, glslang, spirv-cross and FFmpeg.
The scripts select Xcode for this project without changing the machine's
global developer-directory setting.

After obtaining `game/PS3_GAME/USRDIR/EBOOT.elf`, regenerate the analysis and
native sources with:

```sh
python3 tools/bootstrap_runtime.py
sh tools/analyze_game.sh
sh tools/build_port.sh
python3 tools/run_probe.py --seconds 30
```

Do not regenerate sources while a build is running. The analysis script's
code boundary is specific to the input identified in `research/input-manifest.json`.
The runner needs access to the macOS window server. The probe stops its own
process group after the requested time; a timeout is not evidence of success.

For an interactive run, open `tools/play.command`. The game window must have
focus. During races, arrows steer and control throttle/brake, W/S lean, and I/J/K/L
move the right stick. Space is boost (Cross), left Shift is handbrake (Circle),
C changes camera (Triangle), Tab looks back (R3), R resets (Select), F performs
the action button (Square), and Escape pauses or resumes (Start). In menus,
arrows navigate, Enter confirms (Cross), and Escape goes back (Circle); Enter
starts the initial Press Start screen. An attached SDL controller takes
precedence over the keyboard. Stop the process with Ctrl-C in its Terminal
window. Closing the rendering window alone does not yet guarantee that every
guest thread exits. This remains a research build.

The keyboard bridge uses the game's front-end FSM and OSD pause state
to switch between menu navigation and race controls. The keyboard report tests
cover throttle pressure, independent stick lean, pause navigation, held-key
transitions, modifier releases and loss of focus. `sh tools/test_keyboard_context.sh`
checks the pinned guest layouts and invalid-pointer guards;
`sh tools/test_pad_reports.sh` checks the resulting guest controller packets.

The launch scripts locate FFmpeg in `/opt/homebrew/lib`; override
`PS3RECOMP_FFMPEG_AVCODEC` and `PS3RECOMP_FFMPEG_AVUTIL` for other installations.

## Latest demonstrated progress

- The actual language menu renders in the native Metal window. The user also
  confirmed left/right navigation works. Return maps to Start, not Cross.
- Cross advances beyond language selection into the intro/video startup.
- Profile startup previously aborted with “Memory container failure”. Fixed
  retail syscalls 341/342 (incorrectly bound to mmapper), syscall 343 argument
  order, and the available-memory field. AutoLoad2 now completes its callback.
- Added the raw WWS SPU policy at ELF address `0x100dd080`, missed by the embedded
  ELF scanner. `tools/lift_policy.py` verifies the input hash and policy fingerprint
  before lifting; `tools/analyze_game.sh` includes this step.
- FFmpeg now decodes the intro at 1280×720. Missing elementary-stream PTS/DTS are
  reconstructed from the stream frame rate (3003 ticks/frame for this intro).
  Video backgrounds still render white; correct playback is not demonstrated.
- Cross skips the intro and reaches the main menu (PLAY / ONLINE / WRECKREATION /
  EXTRAS). A subsequent selection reaches SELECT A BIKE / MOHAWK INDIANA 1.
  Native frame readbacks are saved as `logs/menu-12-after-intro.png` and
  `logs/menu-12-play.png`. See the later vehicle milestone below.
- Vehicle loading exposed the missing syscall 350, which returned success
  without allocating memory and caused a Havok loader fatal error. Added real
  container allocation with capacity checks, accounting on free, and rejection
  of destroying an in-use container. This is tested through retail syscall IDs.
  The underlying allocator still uses a monotonically increasing guest address
  and does not reclaim freed address ranges; long sessions are not validated.
- Latest bounded run, `logs/native-menu-14.log`: reached event A Taste of
  Violence / The Grizzly / Anything, selected the Buggy class and Beelzebuggy
  Carabid 1, then displayed the race-loading advice screen. The previous Havok
  fatal error did not recur. Final output remained black; no playable race,
  visible 3D vehicle, or one-opponent/solo configuration is demonstrated.
- Earlier graphics evidence: repeated `GcKernel::Present()` timeouts, then
  FIFO CALL to unmapped IO `0x1F18E38C` (word `0xBF18E38E`) at GET `0x00844A84`,
  resynchronised to PUT `0x00010058`. This is evidence of incorrect command-stream
  handling or corrupted/reused command data, not yet a proven root cause.
  `logs/native-stacks-14.txt` shows active FIFO/texture processing rather than a
  stopped rendering thread. Runs 15, 16 and 18 with real RSX semaphore acquires
  did not reproduce this error; launchers now enable `GCM_SEMA_ACQUIRE=1`.
- Traced the white output through 96 render passes (`logs/gfx-18/passes`).
  The exposure/bloom chain first saturates at pass 33. Vertex constant 73
  decayed by 0.6 each frame to the smallest float, causing reciprocal overflow.
  The game was reading stale CPU-visible bytes of its GPU exposure target.
  Readback of the actual FP16 target at local offset 0x01FB7700 restored the
  constant to approximately 1 and the full-colour main-menu background in
  `logs/gfx-21/frame.ppm`. No exposure value or shader was hardcoded.
- Added optional `RSX_ENGINE_CPU_READBACK=1`: at frame boundaries, completed
  dirty RGBA8/FP16 colour targets are copied to guest memory in write order.
  This replaces the diagnostic fixed-address readback. Run 22 confirms stable
  exposure and coloured race-selection UI; launchers now enable it. It is a
  correctness fallback with readback overhead, not a general
  implementation of coherent CPU/GPU memory during a frame. Neutral engine
  regressions, including a full-row FP16 CPU-readback check, pass in
  `logs/graphics-coherence-tests.log`.
- Run 22 renders Beelzebuggy Carabid 1 in the vehicle-selection scene, including
  the driver and surrounding rocks: `logs/gfx-22/buggy-visible.png`. This is the
  first confirmed visible 3D vehicle. Surface/shadow artefacts remain; this
  milestone does not yet establish a playable race.
- The race transition reached the old 64-colour-target capacity, at 2382 draws
  per frame. Raised colour/depth target capacities to 256 and added a regression
  that creates and clears 80 distinct targets across a scene transition. This
  avoids silently dropping new targets at 64; it does not implement eviction.
- Removed an unused signed profiling counter whose overflow flooded a long
  probe with diagnostics. That run is retained as `logs/native-menu-13.log.gz`.
  Probes now stop at 64 MiB of output by default, as well as the time limit.

Local runtime changes are preserved in `patches/ps3recomp-local.patch` against
the pinned upstream commit. Apply with `git -C vendor/ps3recomp apply
../../patches/ps3recomp-local.patch` on a clean checkout, before building.

Regression checks: `sh tools/test_memory.sh` exercises retail syscall dispatch,
allocation capacity, free accounting and big-endian memory information.
The complete runtime suite passed 19/19 (`logs/runtime-tests-final.log`);
the codec suite passes 10/10, including real
H.264/MPEG-2 pixel comparisons, missing timestamps and sequence resets. Logs:
`logs/vdec-tests-02.log`, `logs/native-menu-09.log`, `logs/native-menu-12.log`.

ABI references (used to validate behaviour, not linked into the port):
[RPCS3 syscall table](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/lv2/lv2.cpp),
[memory information](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/lv2/sys_memory.cpp),
[video timestamps](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellVdec.cpp).

## macOS runner fix

The upstream template runs the guest on the main thread while its graphics
thread synchronously dispatches Cocoa work to the main queue. This deadlocks
window creation. Our runner moves guest execution to a dedicated thread with
a 16 MiB host stack and services the Core Foundation run loop on the main thread.

The same runner was built against the upstream synthetic smoke ELF and passed:
all 11 markers, guest flips, vblank callback and exit status checks. This test
does not validate MotorStorm gameplay.

## Validation so far

- Runtime builds for arm64 with real HLSL-to-Metal shader translation enabled.
- Runtime CTest suite: 10/10 passed.
- Shader translator tests: 117 passed, 0 failed.
- Upstream unmodified graphics boot harness failed; the modified runner's
  smoke test passed. Logs preserve both outcomes.
- Decrypted ELF: 27,238 OPD functions; broader scan: 29,631 function candidates.
- Initial lift: 30,896 entries including discovered tails/wrappers, 97 C++ files.
- Eight valid embedded SPU images lifted and registered from the EBOOT only.
  This does not prove that the resource archive contains no additional SPU code.
- Resource index: 19,148 entries, 6,582,407,263-byte uncompressed extent.
- Firmware import registration inventory is approximate static analysis;
  registered handlers may still be incomplete or incorrect.

## Upstream comparison and current candidate (2026-10-20)

Compared the current ps3recomp pin with robbe1912/motorstorm-mv-recomp's
Windows/D3D12 research. That project reproduces the same stale FP16 exposure
read and contains a page-tracked CPU-read seam. Its backend is different;
negative-W clipping and AA sample extents are already handled in our newer
register-file engine, so those older changes were not copied blindly.

- Adapted ps3recomp PR 382's preservation of a larger colour target when only
  its clip shrinks at unchanged pitch. Tests cover both reuse and changed-pitch
  reallocation (`logs/graphics-comparison-tests.log`).
- Corrected `cellGcmGetLastFlipTime` from internal nanoseconds to API
  microseconds, following RPCS3. This title does not appear to import the
  canonical NID, so this is **not evidence of a camera-speed fix**.
- Added experimental `RSX_ENGINE_CPU_READBACK=demand`. Generated scalar/raw
  PPU reads check marked pages; a request queue asks the render thread to copy
  the requested rows of the newest matching RGBA8/FP16 surface. It does not
  copy every dirty target on flip. It deliberately remains opt-in pending
  native validation. Direct host-pointer/HLE and SPU DMA reads are outside
  this hook; it is not complete emulated memory coherence. Existing `=1`
  frame-boundary readback remains available.
- Demand readback regression covers FP16 row visibility without changing its
  previous row; the thread test in `port/tests/test_gpu_read_sync.cpp` verifies
  a cross-page read is served on the render owner, and clean reads bypass it.
- The runner services requests between FIFO drains and polls at 1 ms rather
  than adding 4 ms to every pass-completion wait. Actual Metal guest-frame
  notifications are now connected to the optional FPS display.

## Race slowdown investigation (20 October)

Run 25 still rendered the race at roughly 3–6 FPS with repeated/sheared geometry.
On-demand readback alone did not fix this. `logs/native-race-stacks-25.txt`
shows a busy WWS worker interpreting job code, a PPU thread spinning waiting for
job completion, and considerable render-thread waiting for guest work.

`tools/lift_motorstorm_jobs.py` extracts ten C0DEC0DE modules from the
hash-verified retail executable and applies the upstream `lift_jobmods.py`.
Unlike the initial ELF-only scan, this includes runtime-streamed WWS jobs.
Enable only for experiments with `MOTORSTORM_NATIVE_JOBS=1`; the normal launcher
still uses the established path. The lifter reports unsupported words in five
modules, including apparent data tails, so this is not a completeness claim.
The shared runtime now identifies registered WWS signatures independently of
policy image ID and refreshes the selection before using a stale resident
module at a reused LS entry. The compiled dispatch path also uses this check.
The SPU regression suite passes 97 checks, including module replacement at the
same address (`logs/spu-tests-26.log`).

Run 26 reached the race countdown and active race HUD. Race-intro readings
were typically 10–15 FPS, then about 9–11 FPS in the active race, versus 3–6 FPS
in run 25. These are different moving views, not a controlled benchmark.
The SPU interpreter bottleneck shrank in `logs/native-race-stacks-26.txt`;
render encoding, texture hashing/uploads and depth readback now dominate more
of the sampled work. Some intro views looked much cleaner, but repeated/sheared
geometry returned during the active race. Driving and graphics are NOT verified.
Use `sh tools/play-experimental.command` to reproduce the opt-in configuration.

Run 27 captured 256 passes. Scene colour is already damaged before the
postprocessing pass, so the final display scaling alone cannot explain it.
The shadow pass explicitly sets polygon offset slope=2 and bias=0, previously
ignored by our Metal engine. The engine now records and applies that state
per draw and resets it on disabled draws. `sh tools/test_metal_bias.sh` verifies
real GPU depth changes and reset (`logs/metal-bias-test.log`). The neutral
renderer regression also covers forwarding/reset and explicit zero depth
clears, which previously incorrectly became 1.0. A depth-format-dependent
constant bias equivalence is not established; the observed shadow bias is zero.

Apple Metal references relevant to the next measurements:
- https://developer.apple.com/documentation/xcode/optimizing-gpu-performance
- https://developer.apple.com/documentation/xcode/capturing-a-metal-workload-in-xcode
- https://developer.apple.com/documentation/metal/optimizing-texture-data
- https://developer.apple.com/documentation/metal/resource-synchronization

This is an ARM64 executable with recompiled PPU/SPU code, native system glue
and a Metal backend. It still translates PS3 services and RSX commands and has
SPU interpreter fallbacks; it is not a complete native rewrite. macOS is the
current tested target, not a validated iOS build.

## Gameplay profiling, before further optimization (20 October)

Run 30 uses the experimental native WWS modules, demand readback and RSX
semaphore acquire. It records 25 seconds at t=140–165 after scripted menu
navigation and throttle input, with no screenshot/pass dumps. The later CPU
sample (t=168–173) is outside the timing window. The trace contains 272,029
records, zero dropped, and 208 complete guest flip-to-flip frames.

Measured average: **8.33 FPS**, **120.07 ms/frame**, median 115.89 ms,
p95 143.79 ms. These are instrumented measurements, not an uninstrumented
performance guarantee. Run 29 measured 6.95 FPS in a different moving view;
this difference is not evidence of an optimization because only probes changed.

Average CPU work per frame, summed across threads:
- RSX FIFO processing: 71.01 ms. This includes command/state processing,
  resource conversion and some Metal encoding, not just opcode parsing.
- SPU policy execution: 58.43 ms, including native jobs and host policy work.
- Named Havok PPU workers: 11.17 ms, including worker overhead. This is NOT
  all game physics; main-thread and SPU physics are not separately identified.
- Guest job-counter polling at 0x00CF7358: 10.20 ms CPU / 10.43 ms wall.
- Unattributed CPU: 66.73 ms (guest main code, other SPU work, runtime/driver).
- Total process CPU: 217.54 ms. Parallel threads make this larger than elapsed
  frame time. Interpreter fallback is inside the SPU policy total: 0.068 ms.

GPU execution union averages 11.93 ms/frame. Render GPU-wait spans average
16.83 ms, RSX semaphore-acquire spans 14.02 ms, and recorded colour readback
0.88 ms. These overlap CPU/each other and are not additive frame slices.
Metal record encoding averages 2.80 ms CPU and overlaps the RSX category.
Depth-readback CPU work is not separately isolated by the colour-readback probe.

A complete representative frame, t=146471.621–146587.536 ms, took 115.915 ms:
195.156 ms total CPU, comprising 72.492 RSX, 47.870 SPU policy, 10.183 named
Havok workers, 8.839 job polling and 55.773 other CPU. Interpreter fallback
was 0.057 ms within policy work. GPU execution was 12.317 ms; GPU waits
16.680 ms and RSX acquire waits 10.380 ms overlap these lanes.

Only WWS policy image 900 at LS entry 0xA68 was observed entering the
interpreter during this window: 53,736 calls / 14.216 ms total CPU. This is
an interior entry in the 0xA60 policy block, not a newly discovered large
physics interpreter workload. Return probes miss calls escaping by longjmp;
this is a coverage limitation, not proof that all other interpretation is gone.
The earlier non-policy fallback before gameplay remains unresolved. Existing
compiled SPU helpers already emit ARM64 NEON instructions (see run 29's
neon-evidence.txt); floating-point substitutions must preserve SPU semantics.

The separate CPU sample confirms native job module
jobmod_be896a6374ba_e12230 is hot, especially the 0x4238/0x44F8 path and
subroutines such as 0x8480. RSX work repeatedly hashes/decodes/uploads textures
and converts vertex elements. Highest-priority experiments are resource
reuse/invalidation and vertex conversion caching/direct GPU reads, followed
by profiling the native job kernels. A direct Metal pass path is feasible
for proven stable passes, but bypassing the parser alone will not remove
resource conversion or PS3 memory-layout requirements. Do not remove
barriers or semaphore dependencies blindly; the polling loop has a real
producer dependency. Event-based completion could reduce CPU waste after
its ordering is established, but cannot erase the producer's execution time.

30 FPS requires 33.33 ms/frame, about 3.6x less elapsed time than run 30.
Removing the measured interpreter fallback cannot deliver that. GPU execution
alone is below budget in this scene, while RSX CPU and native SPU work need
attention. No stable-30-FPS or playable-race claim is justified yet. Shared
C/C++ resource handling, AOT ARM64/NEON kernels and Metal work can benefit iOS;
macOS-only profiling/UI glue is separate. Run 30's sample reports 5.7 GB
physical footprint (as reported by sample), another issue to address before an iOS port.

The user's smoother lower-left region remains unconfirmed. Captured passes
include 320x180 intermediates, but no temporal capture identifies that region
as one of them. Scene corruption exists before final postprocessing, so final
composition alone is not a demonstrated explanation. No graphics algorithm
was changed during this profiling stage.

Reproduce with `python3 tools/profile_gameplay.py --out logs/profile-new`,
then `python3 tools/report_gameplay_profile.py logs/profile-new/trace.txt
--out logs/profile-new/report.json` (one command line). The probe is bounded
at 185 seconds and stops only its own process group. The generated PPU probe
in `port/recompiled/ppu_recomp_090.cpp` wraps func_00CF7358; regenerating PPU
sources removes it and requires reapplying this measurement-only wrapper.
All probes are opt-in through PS3_FTRACE. Frame-boundary allocation of CPU
intervals is estimated by wall overlap; GPU intervals use completed command
buffer timestamps. Cumulative Havok worker counters are interpolated between
syscall observations. Full details: `logs/profile-30/report.json`,
`trace.txt`, `stacks.txt`. Build passes; SPU suite: 97 passed, zero failed.

## RSX resource reuse and GPU-depth experiments (20 October)

The retained changes correct depth-snapshot ownership and sampling:
- Release replaced, resized and shutdown snapshots through the backend's
  deferred retirement queue; queued draws retain valid resources.
- Reuse snapshots only for matching extent/representation and unchanged depth.
  Invalidate after depth-writing draws, including writes without a clear.
- Read the requested pixel rectangle in the Metal resolve instead of sampling
  the entire padded backing texture with normalized coordinates.
- Store GPU-only depth snapshots privately; retain the existing CPU-readback
  fallback for unsupported alias layouts.

An exact-input vertex cache was tested and removed. Run 31 achieved 7.71 FPS,
not an overall improvement. Run 32 (cache plus depth fixes) achieved 8.60 FPS.
The final retained configuration, without that cache and without packed GPU
depth enabled, is run 34: **8.40 FPS**, **119.03 ms/frame**, p95 **138.22 ms**,
209 complete measured frames. RSX CPU is 69.46 ms/frame, native SPU policy
56.26 ms, job polling 10.14 ms, named Havok workers 10.54 ms, GPU execution
12.52 ms. The previous run 30 was 8.33 FPS / 120.07 ms: this small difference
is not evidence of a meaningful speedup in moving scenes. Physical footprint
reported by sample fell from 5.7G (run 30) to **2.0G** (run 34), consistent with
fixing the unretired depth snapshots. These are not deterministic frame replays.

An opt-in `RSX_GPU_PACKED_DEPTH=1` path replaces the Z24S8 -> guest memory ->
A8R8G8B8 upload round trip with an ordered Metal conversion. It is limited to
matching linear 2D layouts with the established zero-stencil readback semantics;
other cases keep the prior path. It does not mark guest memory CPU-coherent.
Run 33 reduced RSX CPU to 47.40 ms/frame but measured **7.32 FPS** overall,
with more guest job polling. It therefore remains **off by default**, including
in play-experimental.command. No overall performance win or complete CPU/SPU
coherence is claimed. The Metal shader's integer quantization reproduces the
CPU double-precision 24-bit quantizer; its sampled pixels match exactly.

Run 32 still shows repeated bands/sheared geometry. Run 34, after removing
the vertex-cache experiment, shows the car and track much more clearly, but
residual geometry/shadow artifacts remain. These are different moving views,
not proof of a complete graphics fix. Driving is still not verified. This stage establishes a large memory reduction,
not a playable-race or 30-FPS result. See logs/profile-31 through profile-34
for timing JSON, complete logs, separate CPU samples and captured race images.

Validation: neutral renderer suite passes with packed depth both off and on;
real Metal tests cover slope-bias reset, pixel-exact cropped depth and all 128
pixels of packed-depth conversion versus the CPU reference. Sources include
snapshot replacement, reuse, write invalidation and no-readback packed-path
checks. Build and patch are current. `sh tools/test_metal_bias.sh` runs the GPU
checks; `python3 vendor/ps3recomp/tools/test_rsx_immediate_vertices.py` runs the
neutral checks (also run it with `RSX_GPU_PACKED_DEPTH=1`).

## Reusable Metal capture versus RSX replay

`python3 tools/profile_gameplay.py --capture --out logs/capture-new` boots the
scripted navigation once and saves two full present-to-present Metal frames as
`race.gputrace`. Capture mode enables MTL_CAPTURE_ENABLED and disables CPU
trace/sampling; it is not an FPS benchmark. Open the trace in Xcode and choose
Replay to inspect passes, resources and shaders without booting MotorStorm.
The backend can also use PS3RECOMP_METAL_GPU_TRACE, _GPU_TRACE_AFTER (seconds
since first present, default 140), and _GPU_TRACE_FRAMES (1–120, default 1).
Intermediate submissions between presents are included. Use a fresh output
path; capture errors are reported once rather than retried on every frame.

The first saved capture is `logs/capture-35/race.gputrace` (382 MB), containing
two complete **vehicle-selection frames**, not gameplay: visual inspection in
Xcode caught this despite the filename and original timing assumption.
Xcode recognizes the trace and its Apple M5
capture device. Replay was verified in Xcode after the game process exited:
24 command buffers, 136 render encoders, 8 blit encoders, 830 draw calls,
1280x720 BGRA8 output, 325.13 MiB of captured textures. Xcode reports API and
resource insights, including CPU-updated Managed textures that should be
evaluated for Shared storage on Apple GPUs; those are recommendations, not
yet validated fixes. The replay is open in Xcode.

For a verified scene, use `--capture --capture-trigger logs/capture-new/trigger.txt
--seconds 360 --out logs/capture-new`. Inspect `current.ppm`, navigate through
`pad.txt` if needed, and only then write `1` into the fresh trigger file. The
backend option is `PS3RECOMP_METAL_GPU_TRACE_TRIGGER`; it gates capture after
the configured delay. Capture mode extends the GPU CPU-read watchdog to 120
seconds for instrumented shader compilation via
`MOTORSTORM_GPU_READ_TIMEOUT_SECONDS`. Ordinary play retains the 10-second
watchdog, and neither mode continues with an unfulfilled GPU read.

The colour-alias extent regression now checks a 256x256 rendered surface
sampled as a 4x4 texture. Directly binding the full backing texture stretches
the sampled region. Mismatched extents now materialize guest-layout bytes and
decode the requested texture dimensions; exact matches retain the direct GPU
path. The new check fails against the previous implementation and passes
with this fix. This correctness fallback can add readback cost; a GPU crop
should replace it only for proven compatible layouts. This test alone is not
proof that the remaining racing artifacts are resolved.

Follow-up validation: run 38 actually exercised these aliases in vehicle
selection (1440x1280 -> 1396x1241 at 0x00D30000 and 720x1280 -> 698x1241 at
0x01C30000, with sizes changing on later frames). The car renders, but surface
noise/stripes remain. No new verified race trace or gameplay FPS result was
obtained: run 36 hit the normal GPU-read watchdog during loading; run 37
stalled while a worker repeatedly read an audio file, with the guest main
thread joining it; run 38 reached vehicle selection but its bounded run ended
before a verified race. See `logs/capture-36` through `logs/capture-38`.
No trigger was armed for these incomplete scenes, so they do not contain a
new `race.gputrace`. `logs/rsx-extent-before.log` records the regression failing
without the fix; `logs/rsx-extent-test.log` checks matching-size direct reuse,
cropped dimensions, pixel/channel/row-pitch preservation, and cache reuse.

This is a Metal workload capture, not a video clip or a game save state. It
cannot benchmark changed RSX command parsing, PPU/SPU work or physics. An RSX
replay fixture would additionally need command order, initial render state,
resource contents and guest-memory changes at the points they are consumed.
Existing pass images and shader dumps do not contain that complete state.
Ten seconds of full game continuation would require a broader save-state
system; first use one or two captured frames for the rendering defects.
Apple reference: https://developer.apple.com/documentation/xcode/replaying-a-gpu-trace-file

### Verified racing capture (run 40)

`logs/capture-40/race.gputrace` now contains a capture requested for two
complete presentation frames during actual racing. It was successfully
opened, replayed and profiled in Xcode after the native game process exited.
`gameplay-000.png` shows the player buggy, opponents, LAP/POS/BOOST HUD and
the race clock at 00:09.79. The track still has pronounced stripes/noise;
this capture is evidence for debugging, not a claim of playable correctness.

Xcode reports 13 command buffers, 159 render encoders, 4 blit encoders,
3,667 draw calls and 1.63 GiB of texture memory for this captured workload.
Its GPU profile reports 16.64 ms, Overlapping execution, Medium performance
state. Xcode fell back to Lite profiling because the workload exceeded its
full-profiling limit; detailed shader counters are unavailable. This number
is the replay workload's GPU time, not the game's complete frame time, and
must not be converted into a gameplay FPS claim or divided blindly by two.
Before capture, diagnostic one-second FPS samples were mostly 4–7 FPS;
capture, frame dumping and OCR add overhead, so this is not a clean benchmark.

Concrete investigation targets from the trace:

- CPU-uploaded textures use Managed storage; Xcode specifically identifies
  texture 322 (4096x4096 R8Unorm) and recommends Shared on Apple GPUs.
  `eng_texture_create` and its 3D counterpart leave storageMode implicit.
  Validate an explicit Shared path for CPU uploads separately from private
  GPU render targets; do not change all resources indiscriminately.
- Captured API calls include an 86,302,712-byte newBufferWithBytes allocation,
  repeated 7,536,640-byte buffers, and 1440x1280 texture readback/re-upload.
  These are concrete candidates for allocation reuse and compatible GPU-only
  copies. Their CPU cost has not been isolated by this GPU replay.
- The large gap between replay GPU time and live frame intervals points back
  to CPU processing/synchronization, but does not quantify their individual
  contributions. In run 39's later stall, the sampled main guest thread was
  in sys_cond_wait while another guest worker remained active and the render
  thread mostly slept. This is not proof of GPU saturation or a confirmed
  deadlock. Run 40 also stopped advancing after the capture was saved.
- The first incorrect racing draw/pass has not yet been isolated in this
  new trace. Memory/performance warnings alone do not explain the stripes.

The automatic observer waits for LAP, POS and BOOST in a saved frame before
arming the capture, avoiding accidental menu captures:

```sh
swiftc -module-cache-path /tmp/motorstorm-swift-cache tools/ocr_frame.swift -o /tmp/motorstorm-ocr
python3 tools/capture_gameplay.py --out logs/capture-N --ocr /tmp/motorstorm-ocr --seconds 600
```

Use a fresh output directory. The observer requests a diagnostic frame every
30 presentations (`PS3RECOMP_METAL_FRAME_DUMP_EVERY`; normal default 120),
applies throttle after HUD detection, and saves follow-up images. This is
still a short Metal workload, not a ten-second resumable CPU/physics state.
The first observer run timed out during process cleanup after successfully
saving its capture; its evidence manifest was recovered from files/logs and
is explicitly marked as recovered. Cleanup now saves evidence first and
escalates termination of its own native process group if necessary.
The observer passed Python compilation; the OCR helper compiled successfully.

## References

- https://github.com/sp00nznet/ps3recomp
- https://github.com/Scirvir/MSMV-unpacker
- https://github.com/RPCS3/rpcs3
- https://github.com/sp00nznet/ps3recomp/pull/382 (surface preservation code)
- https://github.com/robbe1912/motorstorm-mv-recomp (research/reference design)

The port runner and CMake scaffolding originate in ps3recomp; retain its MIT
license and attribution when distributing source. Game data and generated
game code remain local.

### First corrupt composite isolated and register alias fixed (run 41)

Xcode replay of `logs/capture-40/race.gputrace` isolated the first observed
full-screen stripe corruption to API draw **190102**, encoder **158**, fragment
`main0 (24)`. The main scene after draw 186966 and the resolved scene after
187456 were intact. Replacing only the velocity sample with zero in Xcode
removed the stripes (`draw190102-zero-velocity.png`); that diagnostic edit was
then restored and the original shader reloaded. It is not the runtime fix.

Tracing the velocity producer back to draw **72953**, encoder **95**, fragment
`main0 (34)` identified a concrete translation error: it writes velocity into
H0.xy and then uses UP4UB on R0.x. NV40 overlays these registers, but our
fragment decompiler maintained independent arrays. R0.x still contained a
shadow sample, which was exported as packed motion data. RPCS3's
[RegisterDependencyPass](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/Program/Assembler/Passes/FP/RegisterDependencyPass.cpp)
confirms the mapping: H(2*n).xy/zw overlap Rn.x/y; H(2*n+1).xy/zw overlap Rn.z/w.
The implementation here is independently written and synchronizes written
lanes at runtime, including masked/predicated writes and control flow, preserving
untouched half-word bits. It is shared HLSL translation code, not a macOS-only
workaround. Motion blur remains enabled.

The rebuilt game reached actual racing again. `logs/capture-41/gameplay-000.png`
(clock 00:06.53) and `gameplay-001.png` (00:14.22) show the player buggy, opponents
and terrain without the screen-filling stripe corruption. A new two-frame
Metal capture is saved at `logs/capture-41/race.gputrace`. Throttle input was
accepted; these images do not yet establish sustained controllable driving.
There are remaining visual defects (notably large background/banner shapes),
and this is not a claim that all rendering is correct.

Validation: fragment decompiler 74/74 checks; HLSL-to-MSL translation 117/117;
four real Metal numeric tests (`test_metal_fp_alias`) covering velocity packing,
full-to-half reads, masked half writes, and false predicates; release game build
and actual gameplay capture. Run the GPU regression with:

```sh
cmake --build build/runtime --target test_metal_fp_alias
build/runtime/test_metal_fp_alias
```

The ten diagnostic FPS samples immediately before capture averaged **5.60
reported display FPS**, **5.70 guest FPS** (2–8 displayed FPS); mean of their
reported frame-interval averages was 190.44 ms. This includes frame dumping,
OCR and diagnostic logging, and is not a clean performance benchmark. There
is no demonstrated speed improvement or stable 30 FPS from this correctness
fix. Full CPU/GPU profiling remains separate work. Raw evidence and the exact
sample window are in `logs/capture-41/evidence.json`.

### Resolution experiment paused; original 720p renderer restored

The temporary 900p/1080p scaler produced a new black right edge in menus and
racing. Its CPU resampling also added work, and the input script did not produce
identical camera positions between boots. These trials are **rejected** as a
controlled resolution comparison. The extra fallback-depth test passed but did
not eliminate the gameplay defect; it was not a confirmed root-cause fix.

At the user's request, further resolution tests stopped and the original Metal
backend was restored. The experimental backend patch and test are archived in
`research/resolution-experiment/`, outside the active build. The normal internal
rendering resolution remains **1280x720**. `tools/benchmark_gameplay.py` remains
available for bounded 720p regression runs after substantive changes.

The clean 720p measurement in `logs/profile-43/report.json` recorded 6.10 FPS,
163.87 ms/frame, 212.91 ms aggregate CPU time across threads, and 10.77 ms GPU
execution per frame. RSX processing used 46.88 ms CPU, native SPU policy/jobs
50.00 ms, and the measured interpreter fallback only 0.068 ms. A different later
bridge view (`logs/resolution-720-steady`) reached 10.49 FPS; that is scene
variation, not an optimization gain. GPU and CPU overlap; aggregate thread CPU
time must not be added to GPU time or confused with elapsed frame time.

### RPCS3 references for the measured CPU paths

RPCS3 remains a useful implementation reference for this ARM64 + Metal port.
Inspection on 2026-10-20 found these concrete correspondences (no new RPCS3
implementation was copied during the resolution experiment):

- `Common/texture_cache.h`, `rtt_lookup` / `fast_texture_search`, and
  `Common/surface_store.h`, `get_merged_texture_memory_region`, track overlapping
  render targets by address, pitch, format and dimensions. Deferred subresources
  can form a copied/cropped view on the GPU and cache it with invalidation.
  Our `sink_bind_textures` falls back through `eng_surface_to_guest` and
  `eng_texture_slot` for differing colour extents. A proven same-format,
  same-pitch crop could stay in Metal; mismatched pitch, reinterpretation,
  overlapping writers and render feedback require separate handling. Preserve
  the existing colour-alias extent regression, channel remaps and write-order
  semantics when adding that path. Simply using a full-size texture view was
  already shown to stretch the image incorrectly.
- `Common/BufferUtils.cpp` dispatches ARM64 byte swaps and index conversion to
  NEON, including restart-index handling. Our `dc_fetch` and
  `rsx_vertex_fetch_one` still perform substantial scalar work. Batch conversion
  is relevant on both macOS and iOS, but the previously rejected exact-input
  vertex cache should not be reintroduced without new evidence.
- A game's native executable still pays for its compatibility layer and data
  conversions. Native compilation alone does not establish better performance
  than an extensively optimized dynamic recompiler. The user's working RPCS3
  run is a useful correctness/performance comparison, but a controlled RPCS3
  FPS measurement has not yet been recorded in this investigation.

Sources:
https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/Common/texture_cache.h
https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/Common/surface_store.h
https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/Common/BufferUtils.cpp
https://github.com/RPCS3/rpcs3/blob/master/LICENSE
https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpustarttime


### Work stopped; alternative based on a working RPCS3 frame

At the user's request, all game/profiling processes were stopped. No sustained
30 FPS or new gameplay speed improvement has been demonstrated. The resolution
experiment is archived; the active renderer is back to its original 720p code.

An independently written direct Metal colour-region snapshot prototype remains
**off by default**, enabled only by `RSX_GPU_COLOR_CROP=1`. It copies exact
same-format RGBA8/FP16 regions in GPU command order, retains immutable snapshots,
and reuses them until source writes or dimensions change. Layout, format, pitch,
mipmap and overlapping-writer checks retain the old fallback for unsupported
cases. Unit checks cover cache invalidation and fallback; real Metal checks
verify RGBA/FP16 bits, nonzero origins, ordering and resource retirement.
`logs/profile-gpu-crop-44` was stopped before the timing window: it is not an
accepted gameplay benchmark and provides no FPS gain claim. Do not enable this
prototype in normal launch scripts without gameplay validation.

The alternative investigation found a concrete upstream reference mechanism:
RPCS3 `Emu/RSX/Capture/rsx_replay.h` stores initial RSX registers, FIFO commands,
per-command memory updates, tile state and display-buffer state. Its companion
`rsx_replay.cpp` applies those states and replays the FIFO without running the
gameplay simulation. A known-good MotorStorm gameplay capture from RPCS3 could
therefore become the common input to a native Metal renderer. This requires an
import/replay adapter; our Xcode `.gputrace` files are already-translated Metal
captures and cannot substitute for the original RSX data. No usable RPCS3 RSX
capture was found in this project, and no new RPCS3 run was started after the
user asked to stop.

Proposed acceptance order: replay the captured frame correctly in RPCS3;
import exactly those commands/memory states into the native path; match the
scene and identify expensive conversions on identical input; only then reconnect
the native gameplay code. Fast frame replay would establish rendering capacity,
not 30 FPS gameplay or working physics. Adopting the whole RPCS3 runtime remains
an emulation-based alternative; it is not automatically a standalone native
ARM64+Metal game or an iOS solution.

References inspected:
- https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/Capture/rsx_replay.h
- https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/Capture/rsx_replay.cpp
- https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/PPUTranslator.cpp

### RPCS3 reference captured, 2026-10-20

RPCS3 0.0.43-20344, ARM64, commit
`58317c9350983d71f18df2b0a346433161d6b13c`, is installed locally at
`tools/rpcs3/RPCS3.app`. Official PS3 firmware 4.93 was installed. The initial
strict-rendering run stopped with a dead FIFO; its log is preserved at
`logs/rpcs3-reference/boot-strict-fifo-failure.log`.

The subsequent windowed run reached actual gameplay with these settings:
PPU/SPU LLVM, Vulkan, 1280x720, resolution scale 100%, Write Color Buffers and
Read Color Buffers enabled, Async Recompiler (multi-threaded), Sleep Timers
Accuracy Usleep Only, Strict Rendering Mode off. This follows the settings in
the working log attached to
[RPCS3 issue 8376](https://github.com/RPCS3/rpcs3/issues/8376#issuecomment-3315565790).
The game's window showed approximately 30 FPS during a short driving check
after shader warmup. This is an observation, not a sustained benchmark or a
performance result for our native runner. Settings are saved in
`research/rpcs3-reference/windowed-config.yml`; the prior global configuration
was backed up in `logs/rpcs3-reference/config-before-windowed.yml`.

Alt+C saved `logs/rpcs3-reference/race-start.rrc.gz`: 315,068 command records,
3,045 draw ends, 11,810 memory regions and 280,703,186 bytes of shared data.
The RPCS3 screenshot is `logs/rpcs3-reference/race-start-reference.png`.
Capture automatically pauses RPCS3; holding Start resumes it. The game was
resumed and throttle input moved the motorcycle along the track.

An isolated replay now feeds those recorded registers, vertex programs,
ordered memory updates and commands through our existing Metal draw engine:

```sh
python3 tools/test_rsx_capture.py
python3 tools/rsx_capture.py logs/rpcs3-reference/race-start.rrc.gz \
  --export logs/rpcs3-reference/race-start.msrx \
  --report logs/rpcs3-reference/race-start.json
cmake --build build/game --target motorstorm_rsx_replay
build/game/motorstorm_rsx_replay logs/rpcs3-reference/race-start.msrx \
  logs/rpcs3-reference/race-start-metal.ppm
```

All 3,045 draws ran guest shader programs. The output reproduces the reference
camera and vehicles but still has incorrect lighting and terrain geometry.
No native graphics or performance fix is claimed from this run. The original
capture was subsequently replayed in RPCS3 and displayed black, repeatedly
logging `Display queue was discarded while not empty!`. It is therefore not
yet a validated renderer comparison fixture. The live-game screenshot remains
valid evidence of the expected scene, but is not proof that the recorded
commands reproduce it correctly.

The pinned source distinguishes `GCM_DRIVER_QUEUE` (0xe940/0xe944) from
`GCM_FLIP_HEAD` (0xe920/0xe924). This recording contains an early flip of the
previous display buffer, then ends by queuing buffer 0. The diagnostic
`--complete-flip` option writes a separate RRC copy appending a flip of that
queued buffer, without changing any recorded draw or overwriting the source.
This removed the repeated queue warning but **did not recover the image**.
It is an experiment, not a validated repair. Opening RPCS3's RSX Debugger
during replay also stopped responding and logged Vulkan event timeouts.
Evidence is saved in `logs/rpcs3-reference/replay-control-failure.log`.
RPCS3 was reopened to its main game list after that failed inspection.

The initial 64 native pass dumps covered small particle textures rather than
the main scene. The replay-only `RSX_REPLAY_PASS_MIN_WIDTH` diagnostic now
allows the existing pass dumper to begin at a larger target:

```sh
RSX_ENGINE_PASS_DUMP=logs/rpcs3-reference/scene-passes \
RSX_ENGINE_PASS_DUMP_DRAWS=1 RSX_ENGINE_PASS_DUMP_MAX=128 \
RSX_REPLAY_PASS_MIN_WIDTH=640 \
build/game/motorstorm_rsx_replay logs/rpcs3-reference/race-start.msrx \
  logs/rpcs3-reference/race-start-metal-scene.ppm
```

This captured 61 native rendering passes. The completed main-scene color
pass (`scene-passes/pass002_t0.ppm`) already contains malformed terrain, before
the subsequent resolve and postprocessing. This narrows the investigation but
does not identify the first incorrect draw, nor establish whether missing
capture state contributes. The next acceptance gate is a recording that
replays correctly in RPCS3, followed by comparison of its scene draws against
Metal. No gameplay benchmark or production-renderer change was made in this
control check. All four synthetic capture-format tests pass.

The replay tool currently supports the NV3062/NV308A transfer objects observed
in this capture and rejects other active 2D transfer engines. RRC v6 omits
initial transform constants; correctness depends on the captured stream
writing the constants it uses. Tile metadata is retained but tile/compression
behavior is not implemented by the replay adapter. A frame replay contains no
physics or gameplay simulation and cannot establish native gameplay FPS.
Downloaded RPCS3 sources under `research/rpcs3-reference` are reference-only
GPL-2.0 material from the pinned commit, not linked into the native executable.

### Strict capture control and exposure-history isolation, 2026-10-20

A second capture now renders an image in RPCS3 after synchronous shader
compilation. This supersedes the earlier black-output observation as the latest
control result, but **does not establish a correct gameplay capture**.
Evidence is in `logs/rpcs3-reference/buggy-strict/`. The original source is
`~/Library/Application Support/rpcs3/captures/BCES00006_20261020054449_capture.rrc.gz`,
SHA-256 `686d383e39df63359ba13d597ba40065cb1639dd87a42016f8481e796a035e2a`.
It contains 373,950 command records and 3,696 draw ends. The game was captured
with Strict Rendering enabled and the Legacy single-threaded shader compiler.
Both settings were restored to the previous playable configuration afterward
(Strict off, Async multi-threaded). RPCS3 is left at its game list.

The reference scene is The Grizzly with Beelzebuggy Carabid 1. The live screenshot
`buggy-reference.png` shows time 00:05.44; the recorded frame shows 00:05.48.
They are adjacent observations, not a pixel-exact matching pair.
`rpcs3-full-replay.png` is an F12 screenshot of the full capture replay, while
`buggy-metal.png` is the same recorded command stream through Metal. Both replay
images contain large stretched terrain and black gaps absent from live play.
Therefore those shared defects cannot currently be attributed to Metal alone.
Metal's full output is also substantially darker than the RPCS3 replay.

Pass inspection identified an exposure-history target at local offset
`0x01fb7700`, 64x64 RGBA16F. Draw 3660 (command 368533, native pass 036)
alpha-blends into it. The capture first supplies this memory at draw 3662,
command 368888, memory ID 28969: 32,768 bytes whose first RGBA half-floats are
`(1.3818359375, 0.1312255859375, 0.1312255859375, 0.99755859375)`.
Further commands repeatedly restore the same block. The native adapter copies
these bytes into guest RAM, but texture binding still selects the cached GPU
surface. Its initial contents also lack the preceding frame's exposure history.
RPCS3's capture loader likewise copies recorded memory, but its render-target
cache and Read Color Buffers path handle those writes differently.

`tools/rsx_history_probe.py` performs a deliberately altered replay: it redirects
only the exposure render target to scratch, leaving subsequent texture reads
to consume recorded memory. It checks the exact input SHA and does not modify
the original capture or production renderer:

```sh
python3 tools/rsx_history_probe.py /private/tmp/motorstorm-rsx/buggy-strict.rrc.gz \
  /private/tmp/motorstorm-rsx/history-isolation.msrx
build/game/motorstorm_rsx_replay /private/tmp/motorstorm-rsx/history-isolation.msrx \
  /private/tmp/motorstorm-rsx/history-isolation-metal.ppm
```

The diagnostic brightens the scene. Mean absolute RGB error against the full
RPCS3 replay drops from 39.25 to 30.05 on the 0..255 channel scale. The images
are the same recorded frame and dimensions; no alignment or brightness
normalization was applied. This supports exposure-history/coherency as **one**
source of the brightness difference, not a complete fix. Terrain remains
malformed and the altered replay is not an acceptable production workaround.
Next work should address replay memory/surface coherency and missing initial
history explicitly, then establish a valid geometry fixture before changing
the production renderer's vertex handling.

Additional checks found 264 attribute ranges over 66 late draws not covered by
those draws' attached memory blocks when vertex frequency dividers are applied.
This is a per-draw coverage audit; earlier memory writes may cover some ranges.
It does not prove the cause of the large terrain gaps. A static constant audit
found c0 reads before an upload in 21 draws; dynamic constant indexing was not
resolved. These audit scripts/results are preserved beside the capture.
Checkpoint experiments could display the intermediate resolve target but did
not produce reliable F12 reference images and retain display-queue warnings;
they remain unvalidated diagnostic scripts, not supported importer features.

Active captures were moved to `/private/tmp/motorstorm-rsx` after an iCloud
evicted workspace file caused an RPCS3 read timeout (errno 60). Use locally
materialized files for replay; the copies in `logs/` may be offloaded again.
No native gameplay FPS claim or permanent visual-quality change follows from
these tests. Offline replay excludes PPU/SPU execution and physics.

### Native CPU/GPU visibility investigation, 2026-10-20 morning

The 30 FPS target is **not reached**. New runs use the original 1280x720 internal
targets, native jobs enabled, demand CPU readback and the same scripted route
to The Grizzly / Beelzebuggy Carabid 1. Screenshots show the racing HUD, car and
track; hanging banner/geometry artifacts remain. These are live gameplay runs,
not RPCS3 or offline frame-replay measurements. They are stationary observations
with small differences in settled camera/vehicle state, not deterministic
replays of identical simulation frames.

Two concrete fixes are implemented:

- The render owner waits on the existing FIFO event instead of unconditionally
  sleeping. CPU read requests explicitly signal that event. This follows the
  upstream boot harness's event-driven waiting and retains the 1 ms idle ceiling.
- B8 guest surfaces now store one blue byte per pixel when the Metal backend
  returns RGBA8 host pixels. Guest pitch, host pitch, clipping and odd-width
  tails are handled separately. Full-surface synchronization uses the selected
  surface directly and retains dirty state when bounds cannot be resolved.
  The runtime confirms this path is exercised on a 2048-pixel terrain row at
  main-memory surface offset `0x01a81300`, pitch 4096.

The B8 lead came from the native Windows MotorStorm project's
[f444bcb1832e](https://github.com/robbe1912/motorstorm-mv-recomp/commit/f444bcb1832e49a85a8f9562a6bf8c0f9bfa5efa).
Our neutral renderer needed its own adaptation. Five regression checks fail
before this fix and pass afterward: blue-channel extraction, row bounds,
CPU byte visibility, whole odd-width surfaces and rejection of an incompatible
RGBA8 upload. Existing ARGB8 and FP16 tests also pass.

An additional **opt-in** experiment, `RSX_ENGINE_CPU_READBACK_CACHE=1`, releases
fully synchronized 64-byte lines instead of requesting the render owner for
every subsequent CPU byte load. Dirty neighbouring lines remain protected;
overlapping surfaces must rearm on redraw, and a newer overlapping allocation
prevents clearing. The fine-state bitmap reserves 8 MiB of virtual BSS, touching
only the GPU-backed pages. It uses portable C atomics, no macOS-specific page
fault machinery. This follows the synchronization/overlap principles visible in
[RPCS3's texture cache](https://github.com/RPCS3/rpcs3/blob/58317c9350983d71f18df2b0a346433161d6b13c/rpcs3/Emu/RSX/Common/texture_cache.h#L794);
no RPCS3 implementation is linked or copied into this code.

Measured evidence, each from a requested 30-second gameplay window:

- `logs/profile-45-event-wake`: 7.00 FPS over the entire window. Completed frame
  intervals average 124.68 ms, but the last 3.84 seconds contain no flip; the
  subsequent stack sample is stuck in `func_00CF7358`, the six-lane job barrier.
  An 8.02 FPS average of only completed intervals would conceal this stall.
- `logs/profile-46-b8`: 8.03 FPS, mean completed interval 124.78 ms, p95 137.40 ms.
  Mean RSX CPU 42.99 ms, native SPU policy CPU 48.14 ms, interpreted SPU CPU
  0.062 ms, job polling CPU 31.71 ms, GPU execution 9.87 ms, GPU waits 15.38 ms.
  The final flip is 46 ms before the window ends; the earlier terminal stall
  did not recur in this bounded run, which does not prove it fixed.
- `logs/profile-47-row-cache`: 8.87 FPS, mean completed interval 112.74 ms,
  p95 140.66 ms. RSX CPU 45.56 ms, native SPU policy CPU 49.74 ms, interpreted
  SPU CPU 0.070 ms, job polling CPU 29.39 ms, GPU execution 9.90 ms, GPU waits
  14.00 ms. This modest improvement does not justify calling the port playable
  or promoting the cache to the launcher's default yet.

CPU totals sum concurrent threads; GPU, CPU, waits and polling overlap and must
not be added as a frame budget. The remaining interpreter cost is negligible
in these windows. The substantial costs are native job execution, RSX/resource
processing and cross-thread dependencies. A later stack sample still shows
many CPU byte reads waiting on the render owner, motivating the overlap audit.

`tools/report_gameplay_profile.py` now includes the entire measurement window
in `average_fps`, retains `frame_interval_fps`, and reports the unfinished tail
after the last flip. This prevents a frozen game from appearing fast because
only its completed frames were counted. A synthetic final-stall fixture checks
the distinction. Grouping spans by type also avoids repeated scans of unrelated
events during report generation.

Validation: `sh tools/test_gpu_sync.sh` passes dirty-line boundary/overflow,
cross-page loads, render-owner wakeup, redraw invalidation, neighbouring rows,
the neutral RSX regressions and the FIFO flip-stream tests. Full native builds
pass. Known-good pre-change and intermediate binaries are retained in
`/private/tmp/motorstorm-rsx/`. The runtime patch includes the changes.

`tools/probe_driving.py` is a separate visual check: it recognizes LAP/POS/BOOST
before applying R2 and retaining subsequent screenshots. `--drive-polls` now
defaults to 10000 for sustained throttle within its bounded 35-second capture;
the original 30-poll run 48 showed settling/rolling but did not establish
controllable driving. It makes no FPS claim while OCR/readbacks run. Run 48
reported no newer-overlap rejection, so alias exclusions have not been shown
to explain the remaining CPU-read stalls. `RSX_ENGINE_CPU_READ_AUDIT=1` now
distinguishes missing owners, already-clean surfaces, row padding and active
pixels; its logarithmically sampled diagnostics are not performance evidence.

The optional `MOTORSTORM_GPU_READ_YIELD=1` experiment lets the FIFO walker
return at complete packet boundaries when its owner has CPU-read requests.
Regression checks establish real GET progress, command ordering and no early
flip. Run 49 stalled before gameplay: main awaited a streaming thread while
another repeatedly read an audio file. It is not a benchmark or evidence of
an improvement, and the option remains off. The subsequent yield-disabled
run 50 exited at 39 seconds with the game's `Resource file was not created
with FileOStream` fatal. Neither run establishes causality for the yield.
`PS3_FS_READ_AUDIT=1` adds bounded empty-read diagnostics for this loading issue.

A separate input defect matches
[upstream issue 138](https://github.com/robbe1912/motorstorm-mv-recomp/issues/138):
PAD_FILE holds were decremented by calls returning no report. The production
API fixture reproduces a four-report R2 hold being lost across empty drains.
The corrected counter decrements only for delivered port-zero reports, after
pressure synthesis and freshness gating. `sh tools/test_pad_reports.sh` checks
the hold, pressure, release, port isolation, acknowledgment and drain termination;
the former implementation fails the hold and pressure checks. PAD_SCRIPT's
existing timing behavior is unchanged. This scoped input fix does not prove
vehicle physics or sustained gameplay speed.

Run 51 (`logs/native-51-input-audit`) verifies forward motion beyond the
starting gantry while holding R2 with the corrected report counter. It does
not establish steering, full physics correctness or a performance result:
repeated screenshots/OCR were enabled. CPU-read audit samples were all active
pixels of owned dirty surfaces, so this run does not support bypassing reads
as padding, missing owners or already-clean surfaces. Run 52, with optional
FIFO yield enabled, stalled before gameplay and supplies no usable FPS result.

An actual PPU translation defect occurs during gameplay in runs 47 and 51:
unresolved jumps to `0xD7ABB4` and `0xD7B23C` are valid cases inside the Havok
routine at `0xD7A994`. Its table pointer comes from two TOC loads, is spilled
to the stack and restored into the same register later used for base+offset.
The lifter missed that data flow. The generic fallback now follows a single
unchanged stack spill conservatively, rejecting calls, overlapping/unknown
stores, stack adjustments and branches bypassing the spill. Authored vectors
fail with the fallback disabled and pass with it enabled, alongside the
existing jump-table suite.

`python3 tools/repair_havok_switch.py` applies the recovered table to the four
affected generated functions and two required tail entries. It checks the
ELF hash, all 25 original signed offsets (19 unique targets), original emitter
output and repeatability before writing. Other generated functions remain
untouched. This repairs real missing control flow; its gameplay effect must
be checked independently of the unit tests and successful native build.

Run 53 reaches gameplay with no logged unresolved indirect calls. The full
30-second window measured **9.03 FPS**, mean interval 110.52 ms, p95 135.23 ms,
and a final unfinished interval of 81.68 ms. Mean RSX CPU was 42.94 ms/frame,
native SPU policy CPU 48.98 ms, interpreted SPU CPU 0.070 ms, job-poll CPU
28.60 ms, Havok-worker CPU 13.18 ms, and GPU execution 9.72 ms. These lanes
overlap and must not be added. `logs/profile-53-havok-switch/race.png` shows
the car and track but still has malformed banner/background geometry. The
camera/vehicle position differs from run 47, so 9.03 versus 8.87 FPS is not
an isolated speedup measurement. The missed-switch errors are gone, but
30 FPS and full graphical correctness remain unachieved.

A scoped ARM64 hardware-RTZ arithmetic candidate passed the existing eight
million vector cases, but switching/restoring FPCR per operation cost
39.3 ns per FMA+FM pair versus 7.5 ns for the current NEON implementation.
It was rejected before a gameplay rebuild. Evidence and the discarded header
are under `/private/tmp/motorstorm-rsx/`; no normal-build change was made.

RPCS3's pinned `58317c93` SPU LLVM implementation uses ordinary hardware
single precision for its approximate mode, unlike our exact SPU rounding:
[SPULLVMRecompiler.cpp](https://github.com/RPCS3/rpcs3/blob/58317c93/rpcs3/Emu/Cell/SPULLVMRecompiler.cpp).
The opt-in CMake experiment `MOTORSTORM_APPROX_HOT_SPU` limits that idea to
the measured hot module `jobmod_be896a6374ba_e12230`. Extended-range values
retain the existing helpers. The experiment changes rounding; it is not a
bit-identical optimization or a validated gameplay option. Its authored test
checks 2.4 million ordinary lanes against IEEE results and explicitly pins
the difference from SPU truncation. Run 54 is the corresponding gameplay
measurement; no recommendation follows until its evidence is reviewed.

Run 54 measured 9.33 FPS with approximate arithmetic, versus 9.03 in run 53.
SPU-policy CPU fell from 48.98 to 41.08 ms/frame, but that did not translate
into a substantial overall gain. The option is restored to OFF: arithmetic
accuracy is not sacrificed for this small, scene-dependent difference.
Run 55 (`logs/native-55-havok-driving`) verifies sustained forward motion on
R2 after the switch fix, including passing traffic from ninth to eighth. It
does not establish steering or complete physics correctness; the unsteered
car eventually rolls. No unresolved-indirect-call errors were logged.

`RSX_METAL_DIRECT_STAGE=1` writes geometry/uniform staging data directly into
shared Metal buffers. Four buffers are retained with their last command-buffer
fences, and reuse waits for GPU completion. Growth preserves existing bytes;
the asynchronous presentation path also retires its staging buffer. This
removes `newBufferWithBytes` and its whole-arena copy per submission. The old
path remains available with the option off. Apple documents this ownership
pattern in its [buffering guidance](https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/MTLBestPracticesGuide/TripleBuffering.html).

The real-GPU `test_metal_shared_stage` checks alignment, growth, immutable
in-flight data, exhaustion of all four slots, completed-slot reuse and size
limits. `test_metal_color_crop` also passes with the option enabled. Replaying
the same 3045-draw capture with copied and shared staging produces byte-identical
PPMs (SHA256 `f394110b34968fe5f078dcecd831d16bacb5097a873b0f80fbb3171c11dfc506`).
This is an equivalence check, not evidence that the source frame is correct.

Run 56 (`logs/profile-56-shared-stage`) used exact arithmetic and shared staging:
**9.47 FPS**, mean 105.47 ms, p95 128.30 ms, unfinished tail 103.75 ms. RSX CPU
was 38.97 ms/frame, SPU-policy CPU 46.42 ms, job-poll CPU 28.43 ms, Havok-worker
CPU 14.50 ms and GPU execution 9.82 ms. The same scripted navigation reaches
the same bridge area, but frame-to-frame simulation timing is not deterministic;
the 4.8% FPS difference from run 53 is not a controlled per-draw speedup.
The 30 FPS target and the remaining banner geometry defects are unresolved.

Run 57 tested `SPU_MFC_OWNER_CACHE=1`, a thread-local engine lookup cache with
retirement-epoch invalidation. Registry tests cover simultaneous ownership,
stalled DMA lists, cross-thread retirement and same-address reuse. Lookup locks
disappear from the subsequent stack sample, but gameplay measured **8.57 FPS**;
this is not an accepted speed improvement, and the option remains OFF. The
registry lifetime contract still forbids freeing an actively executing context.

Disabled PPU write watches now take a short inline gate; configured watches
and the dynamically armed barrier watch still reach their original handlers.
SPURS poll diagnostics now obey `SPURS_PM_TRACE`, like policy-entry/exit logs:
their "first four polls" counter resets every quantum and had produced
thousands of log lines per second even with tracing off. Run 58, with the MFC
cache OFF, measured **9.57 FPS**, mean 104.64 ms, p95 126.76 ms, RSX CPU 38.13 ms,
SPU-policy CPU 44.24 ms and job-poll CPU 28.66 ms. The minor FPS difference
from run 56 is not conclusive; the removed diagnostic work is directly visible
in the logs and CPU sample.

`MOTORSTORM_JOB_EVENT_WAIT=1` is an opt-in wait experiment for the measured
PPU barrier at `0xCF7358`. After 256 unsuccessful spins it arms a host watcher,
rechecks the original seven counters and suspends for at most 250 microseconds.
Successful SPU atomic commits wake matching watchers after releasing the
reservation lock. Ordinary writes remain covered by the timeout fallback.
The original lifted loop always makes the final decision and supplies guest
register state. `python3 tools/test_job_wait.py` checks that actual lifted code
against its original form, including concurrent lane completion, unrelated
notifications, missing notifications and queue-pointer replacement. Trace
kind 24 records notified waits versus timeout fallbacks, nested within kind 20;
these intervals must not be added to the existing job-wait duration. Run 59
tests this option in gameplay; it is not enabled by default.

Run 59 measured **9.13 FPS** with the event wait. Job-poll CPU fell from
28.66 to **2.14 ms/frame**, while elapsed job-wait time remained 29.41 ms.
The trace contains 52,105 notified waits and 14,979 timeout fallbacks, on
job headers `0x40244380` and `0x4024C480`. Total process CPU fell to
158.57 ms/frame across threads, but this is not an FPS win: the dependency
still takes time to complete. The option remains off by default. A production
channel regression additionally verifies successful atomic notifications occur
after releasing the reservation lock, and failed PUTLLC does not notify.

The next graphics investigation is raw vertex upload/GPU conversion. The
pinned RPCS3 implementation separates persistent and transient vertex streams
in [VKVertexBuffers.cpp](https://github.com/RPCS3/rpcs3/blob/58317c93/rpcs3/Emu/RSX/VK/VKVertexBuffers.cpp)
and decodes attribute formats, byte order and divisors in
[RSXVertexFetch.glsl](https://github.com/RPCS3/rpcs3/blob/58317c93/rpcs3/Emu/RSX/Program/GLSLSnippets/RSXProg/RSXVertexFetch.glsl).
This comparison motivated the experimental GPU conversion below. It also exposes different SNORM16/CMP32 normalization and half-denormal
rules; those need separate correctness checks, not silent changes hidden in a
performance experiment.

The experimental path is now implemented as `RSX_METAL_GPU_VERTEX_FETCH=1`.
It snapshots raw input spans before subsequent texture work, deduplicates
overlapping interleaved spans, and keeps existing CPU conversion for unsupported
or excessively sparse inputs. Metal converts all eligible draws in a command
buffer with one compute dispatch, split into groups of 64 vertices. Outputs
share the fenced staging arena and are consumed by the existing vertex shaders.
This is newly written Metal/C code; the RPCS3 sources inform the approach.

The raw-snapshot sanitizer test checks 256 plans / 6,144 vertex results,
overlap deduplication, source lifetime and rejection of invalid/sparse plans.
The real-GPU test compares 200,886 vertices byte-for-byte against the CPU path,
including every half and SNORM16 bit pattern, all supported formats and sizes,
mixed layouts, division/modulo indexing, defaults and 20-bit element wrapping.
Normalized-format lookup tables deliberately preserve the existing CPU results;
this experiment does not silently change the format semantics noted above.

The same 3,045-draw race capture also remains pixel-identical with GPU fetch
enabled (SHA256 `f394110b34968fe5f078dcecd831d16bacb5097a873b0f80fbb3171c11dfc506`).
The neutral draw-engine regressions, shared-stage lifetime test and ordered
color-region copy tests pass. The normal native build succeeds. Run 60
(`logs/profile-60-gpu-vertex-fetch`) measured **9.77 FPS**, mean 102.31 ms,
p95 121.39 ms, RSX CPU 33.90 ms, SPU-policy CPU 44.63 ms and GPU 9.63 ms.
The car and track remain visible, with the same outstanding tall geometry
artifacts. The event-wait and MFC-cache experiments were OFF. Relative to
run 58, RSX CPU fell from 38.13 ms; the small FPS change is not a controlled
isolated speedup because live gameplay timing varies between runs.

The bounded stage now preflights a complete draw before binding constants,
so an arena rollover cannot leave VS/PS constants in the previous buffer.
A forced 8 MiB arena regression verifies the new draw retains constants,
vertices and indices after submission. The neutral engine tests pass.

An exact ARM64 arithmetic experiment (`MOTORSTORM_CULL_SCOPE=ON` at CMake
configuration) specializes only the measured WWS block `0x8480`. Explicit
NEON instructions run in a bounded round-toward-zero FPCR scope, with the
host FPCR restored before control flow or callbacks. Extended-range results
use the existing exact helpers in host mode. `tools/test_spu_culling_scope.py`
compares all context bytes and branches in 200,000 cases; it measured
477.10 ns versus 194.09 ns per block invocation. Run 61 measures the actual
frame impact: **9.87 FPS**, mean 101.41 ms, p95 120.14 ms, SPU-policy CPU
43.43 ms versus 44.63 ms in run 60. The car and track remain visible. The
small whole-frame difference is not sufficient evidence for a general speedup.
The specialization remains opt-in; the original lifted source is untouched.

A subsequent ABI audit found `_cellSpursWorkloadAttributeInitialize` discarded
its ninth argument and hardcoded `maxContention=1`. The actual MotorStorm
caller at `0xCF6BFC` stores the requested limit in the BE doubleword at
SP+0x70 before invoking NID `0xEFEB2679`. RPCS3 accepts this ninth parameter
and writes it to the workload attribute. A context-aware import adapter now
reads that stack argument; concurrency is determined by the game rather than
a substituted value. The adapter fixture checks limits 1–6, BE stack-word position, unaffected
arguments and signed error results. A full scheduler test takes the attribute
through AddWorkloadWithAttribute and admits two real concurrent policy workers.
The generated NID table registers the context handler. Actual boot logging
then confirms MotorStorm requests **5** for each of its four main WWS lists
and **1** for MP3; run 62 measures the repaired behavior.

Run 62 (`logs/profile-62-spurs-real-contention`) reached gameplay at **12.03
FPS**, mean 83.18 ms, p95 100.88 ms. Job-poll wall time fell to **10.49 ms**
(from 28.03 in run 61); RSX CPU remained 34.44 ms. Policy CPU was 70.16 ms
across five concurrent lanes, each accounting for approximately 5.0–5.1 s of
CPU over the 30-second window. The larger aggregate CPU figure is not elapsed
frame time. The post-window sample now contains substantially more MFC lookup
lock contention, motivating run 63 with the already-tested owner cache. The
car and track are visible; the moving camera differs from earlier runs, so
12.03/9.87 is not a deterministic isolated rendering-speed comparison.

Run 63 (`logs/profile-63-parallel-mfc-cache`) repeats the repaired five-lane
scheduler with `SPU_MFC_OWNER_CACHE=1`: **14.30 FPS**, mean 69.89 ms, p95
88.23 ms. RSX CPU was 26.99 ms/frame, SPU-policy CPU 42.54 ms, job-poll wall
6.08 ms, and GPU 7.52 ms. Process CPU fell from 182.95 to 119.68 ms/frame
across threads. The sample removes the old MFC registry lock convoy, though
scheduler wake contention remains. The visual check shows the car, bridge,
track and HUD; remaining banner defects are unresolved. Scene timing varies,
so these live-run differences are not deterministic per-draw comparisons.

The optional packed-depth path now also passes 8,388,608 real-GPU comparisons
against double-precision CPU Z24 quantization (including near-one depths and
low-depth/endpoint samples). The 3,045-draw replay differs from the current
CPU-depth path in 13 pixels, with a maximum one-unit RGB difference on the
0–255 scale; it is **not** byte-identical. Repeated unchanged baseline replays
also vary by one pixel/one unit. The full-frame difference remains documented;
no reduction in resolution or graphics features is involved.

Run 64 (`profile-64-parallel-packed-depth`) measured **15.13 FPS**, mean 66.19
ms, p95 80.02 ms, RSX CPU 24.95 ms and GPU 9.17 ms. Run 65, with targeted
per-worker wakeups, measured **13.87 FPS** despite fewer scheduler lock samples;
that policy remains optional (`SPURS_TARGETED_WAKE=1`) and off by default.

`RSX_TEXTURE_XXH3=1` optionally replaces only the private texture-cache full-span
content hash with xxHash 0.8.3 XXH3. It reads the same bytes and retains the
existing sparse checks/invalidation cadence. The vendored upstream header and
BSD-2-Clause license are under `third_party/xxhash`. Mutation/bounds regressions
pass; a local microbenchmark measured 48.35 versus 8.28 GB/s (not gameplay FPS).
The neutral engine suite and saved-frame replay pass; replay RGB differences
are at most one unit, consistent with the separately documented replay
variation. Run 66 stalled in WWS DMA-wait code before the measurement window;
it has **no valid FPS result**. Run 67 restores the standard wake policy and
completes at **15.40 FPS**. The failure is retained for investigation, not
attributed to hashing without evidence. Live-run scene timing varies, so these
figures do not establish isolated per-draw speedups.


## Further measured native work — runs 68–74

Run 68 added exact same-format GPU region copies: 15.50 FPS, 64.53 ms mean.
Run 69 applied opt-in Apple QoS classes at thread entry: 15.83 FPS, 63.07 ms.
Run 70 added a dense generation-stamped vertex index map: 15.90 FPS, 62.88 ms.
The latter changes are small relative to run variation; none independently
establishes a large speedup. `tools/test_vertex_remap.sh` checks more than ten
million references against the original remap, including mixed base indices,
wide indices, duplicates, generation wrap and growth. The original hash route
remains the fallback when the dense map's eligibility conditions fail.

`MOTORSTORM_NEON_VMX=ON` uses an exact ARM64 NEON `vmaddfp` in five measured PPU
translation units. `tools/test_ppu_vmx_neon.sh` checks 9.6 million lanes against
the original helper, with all four rounding modes, VSCR.NJ, exceptional values
and aliased operands. The actual -O3 microbenchmark was 7.46 versus 4.56 ns per
vector. Run 71 stalled before gameplay and has no valid FPS result. Run 72
reached the race at 15.37 FPS, 65.12 ms mean; no gameplay gain from VMX is proven.

A production-channel regression exposed three event producers that did not
share the reservation/event-ack lock: DMA list-stall, tag-update and reservation
poll. Their unprotected read-modify-write could lose another event bit raised
by a peer. All now share the lock; the three concurrency cases fail before the
change and pass after it, alongside the SPU context/ownership/reservation suite.
This is a correctness repair; it does not yet establish that every intermittent
WWS loading stall is resolved. Optional `MOTORSTORM_WWS_WAIT_TRACE=1` observes
long load waits without completing jobs or altering guest state.

`RSX_GPU_COLOR_REINTERPRET=1`, together with `RSX_GPU_COLOR_CROP=1`, adds an exact
GPU path for the repeatedly sampled surface at 0x01FC0000: A8R8G8B8 backing
bytes interpreted as two big-endian half words. It retains integer bit patterns
in an RG16Uint view, then samples the RG16Float view; no half arithmetic is
used during conversion. The Metal fixture checks every half bit pattern,
including NaNs/subnormals, odd crops, and two snapshots separated by a clear.
Neutral engine tests cover format/extent invalidation, reuse, pitch rejection
and allocation fallback. The same 3045-draw replay differs from the CPU route
in 25 bytes by at most one unit of 255, within the minor replay variation seen
previously; it is not a complete reference-renderer correctness oracle.

Run 73 measured **19.60 FPS**, 51.03 ms mean and 65.38 ms p95, versus run 72's
15.37 FPS / 65.12 ms. RSX CPU fell from 21.34 to 15.13 ms/frame and FIFO drain
wall time from 29.93 to 15.14 ms. The verified screenshot retains the car,
bridge and HUD, with existing banner/geometry errors. These scripted runs use
the same navigation but are not deterministic simulation replays. Background
Xcode/iOS Simulator processes were present; small differences are not isolated
performance claims.

The earlier optional CPU-read yield could stop inside BEGIN/END despite its
comment promising otherwise. A new FIFO regression reproduces that defect;
the yield now waits for a completed draw as well as a packet boundary. Existing
ordering, GET publication and flip sequencing regressions pass. Port trace kind
25 now measures the complete CPU GPU-read request wait, including queue delay.
Run 74 evaluates this option; retain it as experimental until gameplay is checked.

Sources informing the GPU storage approach: [Apple Metal feature tables](https://developer.apple.com/metal/capabilities/),
[Apple texture pixel-format views](https://developer.apple.com/documentation/metal/mtltextureusage/pixelformatview),
and [RPCS3 Vulkan texture transfers](https://github.com/RPCS3/rpcs3/blob/58317c93/rpcs3/Emu/RSX/VK/VKTexture.cpp).
The Metal implementation above is independently written for this backend.

### Draw-boundary CPU-read service and early Metal submission

Run 74 (`logs/profile-74-draw-boundary-yield`) reaches the bridge with the car
and HUD visible: 20.50 FPS, mean 48.83 ms, p95 54.92 ms. RSX CPU is 14.99 ms,
GPU execution 9.20 ms and full CPU GPU-read dependency 11.29 ms/frame. The
last figure includes queue delay and synchronization, not only copying bytes.
581 reads of C1FBB800 account for 5.965 seconds of the 30-second window; these
are actual pixels in a dirty GPU target, so skipping them would be incorrect.
The guarded yield change resumes only after an RSX END, with a regression that
fails for the former mid-draw yield. This single scene-dependent run is not
proof of universal speedup or elimination of all load stalls.

`RSX_METAL_EARLY_RECORDS=512` is a new opt-in experiment: submit completed
draw batches asynchronously while the CPU processes following RSX commands.
It uses the same ordered Metal queue, full attachment stores, retained resource
objects, four fenced staging arenas and at most four pending early command
buffers. Guest reads still fence the queue. Texture-cache replacement uses
fresh immutable objects; generic updates of previously bound textures fence
their consumers first. GPU errors in earlier submissions remain errors.
Apple recommends balancing early GPU utilization against excessive submissions
([command buffer guidance](https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/MTLBestPracticesGuide/CommandBuffers.html)).

The real-device crop fixture verifies blocked-GPU asynchronous return, handle
recycling without destroying in-flight resources, and texture overwrite ordering.
The staging fixture still passes. The same 3045-draw replay differs from the
preceding RG16 run in only 10 bytes, maximum 1/255; it is an equivalence check,
not an independent oracle. Run 75 measures gameplay next; the launcher has not
yet enabled early submissions.

Run 75 (`profile-75-early-metal-512`) reaches the same bridge, 20.53 FPS,
mean 48.72 ms, p95 53.86 ms. Early submission cuts CPU GPU-read dependency
from 11.29 to 5.34 ms but increases RSX CPU to 16.69 ms and GPU execution to
11.52 ms (additional pass boundaries); overall throughput is unchanged.
It is a latency improvement, not a demonstrated FPS improvement.

Run 76 (`profile-76-cold-watch-event-wait`) is 20.60 FPS, mean 48.52 ms,
p95 51.53 ms. `barrier_watch_hit_slow` is now explicitly cold/noinline: ARM64
disassembly showed the compiler previously folded the large logger into the
disabled watch gate, spilling twelve registers on every store. The small gate
is now inlined into the stores. The real ELF-entry fixture passes (its runner
now links the required guest-read synchronization implementation). With the
previously verified job-event wait enabled, process CPU falls from 122.14 to
109.88 ms/frame and job-poll CPU from 7.87 to 1.13 ms. These two changes were
measured together; individual FPS effects are not established.

`RSX_FP_METADATA_CACHE=1` adds a bounded 64-entry instruction metadata cache,
independently implemented against the existing decoder. Every instruction
is compared exactly on reuse; inline constants are still reloaded and sanitized
each draw, and seeded pipeline hashes are unchanged. Branch programs retain
the existing decoder. This follows the general separation of shader metadata
and runtime constants visible in [RPCS3's renderer](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/GL/GLGSRender.cpp).
The address/undefined sanitizer fixture covers 5000 random programs, every
instruction mutation, changing constants, hashes, masks, maximum size and
invalid input. The draw-engine regressions pass with caching enabled. The
3045-draw replay is byte-identical to the preceding early-submission replay.
Run 77 adds a direct trace of flip waiting to identify the recurring 48 ms
frame intervals. No 30 FPS claim follows from these tests.

### Correct firmware flip ordering (runs 77–79)

Run 77 reaches 20.73 FPS (48.20 ms mean, 50.37 ms p95). Its new probe finds
18.20 ms/frame spent inside the HLE `cellGcmSetWaitFlip` CPU sleep loop. This
was an incorrect implementation: the installed PS3 firmware's export at
0x1180 writes four RSX command words, `00040064 00000010 00040068 00000000`,
and returns. Its checked entry reserves four words through the context's
callback; a nonzero callback result prevents any write. The Unsafe entry at
0x1228 omits only that reservation. The extracted firmware PRX SHA-256 is
`6b02dbb03ed7e3432c632565bbdf428c71a62c67aa98a31e5bf7c77525767873`.
Firmware is local analysis material and is not included in the runtime patch.

The corrected HLE enqueues that wait. `cellGcmSetFlip` now queues the driver
label release and a flip marker in the same context, instead of presenting
immediately before the frame tail has been flushed. Completion clears driver
label 1 before callback delivery, consistent with
[RPCS3's flip-event implementation](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/lv2/sys_rsx.cpp).
The CPU can prepare subsequent work while the RSX waits for the flip. It does
not skip the wait or complete work prematurely. The FIFO regression covers
unflushed flips, wait ordering, callback failure/success and the Unsafe entry;
the separate late-completion status fixture also passes.

Run 78 reaches **29.07 actual frames/s**, mean 34.39 ms, p95 40.86 ms, at the
same bridge with the buggy and HUD. CPU SetWaitFlip time is 0.00023 ms/frame.
It reports 872 requested and 872 FIFO-completed frames over 30 seconds. The
window's approximately 60 FPS was misleading: the runner presented each queued
flip twice (1743 backend calls). Run 79 fixes that duplicate pending-flag claim
and replaces the rounded 16 ms vblank clock (62.5 Hz) with 59.94 Hz monotonic
timing. These are correctness changes, not reduced rendering quality. Its
measurement is 29.23 FPS, mean 34.19 ms, p95 37.87 ms, with 877 requests,
877 FIFO flips and 877 backend presents. The same bridge/car/HUD is verified.

Run 80 (`native-80-control-plan`) verifies movement from the grid, along the
ground route to the right of the bridge and under the rock arch. The authored
controller plan includes throttle, left/right analog input and braking; input
reports retain full trigger pressure. Its first/last HUD times are 0.00 and
20.41 over 20.44 wall seconds. The images show forward motion, changes of heading
and illuminated brake lights; this does not establish a full lap or complete
physics correctness. Repeated readback/OCR makes this unsuitable for FPS claims.
The PAD_FILE fixture now also verifies simultaneous axes/throttle, port isolation
and neutral release. The user's Xcode shader edit is preserved separately at
`logs/capture-40/MTLLibrary 25/6C4E7579BD17E1E0`, not substituted into the game.

### Moving-car timing and callback investigation (runs 81–83)

The 15-second moving-car window (111–126 seconds after launch, authored
controls starting at 108 seconds) measures 25.73 FPS in run 81, p95 58.41 ms,
maximum 357.36 ms. Its longest frame spends 328.99 ms waiting in the main
thread's Havok-worker completion path, with 166.36 aggregate worker CPU ms.
RSX CPU is only 14.18 ms in that frame. Other long frames occur inside the
RSX drain. Screenshots and sampling are outside the measurement window.
The scripted braking also selects reverse when held; the final image says
WRONG WAY. This is a control/load test, not successful navigation of a lap.

`MOTORSTORM_GCM_CALLBACK_THREAD=1` is an opt-in dispatcher with its own reserved
guest stack and the existing non-reentrant callback guard. It takes inspiration
from [RPCS3's independent interrupt delivery](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/RSXThread.cpp).
The concurrency fixture verifies that a pump racing an occupied handler leaves
pending notifications intact. It does not bypass guest waits. Run 82 records
27.87 FPS, p95 46.23 ms, maximum 223.59 ms; run 83, the same binary with the
dispatcher off, records 27.33 FPS, p95 43.19 ms, maximum 546.33 ms. Vblank
delivery improves from 0.469 ms mean/2.503 ms p95 to 0.027/0.070 ms, but that
does not explain the hundreds-of-milliseconds stalls. Dynamic collisions and
timed inputs produce different trajectories, so the small throughput difference
is not a controlled causal FPS result. The dispatcher remains off in the launcher.

The sampled Havok paths also call scalar VMX add/subtract and negative
multiply-subtract helpers. Their exact NEON equivalents now join the existing
multiply-add specialization in the same five translation units. A 38.4-million-
lane differential fixture passes all four rounding modes, NJ denormal handling,
exceptional values/NaN payloads and aliased destinations. Local microbenchmarks
show roughly 2.2–2.4x improvement for the newly specialized operations; whole-game
impact is not established yet. New trace kinds distinguish cold shader translation
and Metal pipeline construction from normal RSX work in the next run.

Run 84 adds the new SIMD operations and cold-build probes: 25.00 FPS,
p95 53.99 ms, maximum 129.76 ms. There are 19 shader builds totalling
227.37 ms wall/224.76 ms CPU; 19 Metal PSO builds total only 4.20 ms wall.
Shader build cost averages 0.61 ms/frame, while total RSX CPU is 22.05 ms.
This does not establish a whole-game SIMD gain; the extra operations now sit
behind `MOTORSTORM_NEON_ARITHMETIC=OFF` by default, preserving the previously
validated multiply-add optimization. Different collision paths and concurrent
iOS/tvOS simulator activity limit comparisons of these moving-car runs.
`RSX_ENGINE_PHASE_TRACE=1` will separate geometry, targets, pipeline lookup,
textures and submission in the next diagnostic run. Its per-draw CPU clocks
add profiling overhead, so that run is for attribution, not a headline speedup.

### Geometry attribution and sparse raw snapshots (runs 85–86)

Run 85's expensive per-draw diagnostic clocks attribute 8.70 ms CPU/frame
to geometry preparation, 7.24 to pipeline lookup, 6.58 to staging/submission,
3.42 to textures and 0.73 to targets. These are nested inside RSX time and
include the diagnostic overhead; its 20.40 FPS is not a clean comparison to
the earlier runs without those clocks.

The 3045-draw RSX replay confirms 302 draws/72,692 vertices still use the CPU
vertex decoder. `RSX_GPU_VERTEX_GATHER=1` now allows an oversized sparse source
span to become an immutable snapshot containing only its referenced raw
elements. Each attribute resolves its original divisor/modulo/base-index wrap
before dense local references are assigned. The existing Metal decoder and
its ABI are unchanged. Invalid mappings retain the CPU fallback, and the
16 MiB snapshot limit remains. This option is off unless explicitly enabled.

With gather enabled, that replay uses GPU conversion for all 3045 draws and
577,922 vertices; its final PPM is byte-identical to the preceding baseline replay.
This checks regression equivalence, not independent accuracy against PS3.
The sanitizer fixture passes 512 snapshots/12,288 exact vertices, including
sparse ranges, nonzero starts, all formats and source lifetime. The real Metal
fixture passes 209,110 byte-identical vertices including every half/snorm bit
pattern, mixed divisors/modulo/defaults, sparse gather and output guards.
Run 86 measures the actual moving game with detailed draw clocks disabled.

Run 86 (gather on) records 20.87 FPS, p95 89.23 ms, RSX CPU 27.82 ms,
GPU 13.02 ms and main-condition waiting 12.45 ms. Run 87 (same binary,
gather off) records 28.20 FPS, p95 43.00 ms, RSX CPU 17.83 ms, GPU 11.29 ms
and main-condition waiting 4.30 ms. The screenshots have different positions
and race times (05.73 versus 24.49), so this is not an isolated causal rendering
comparison. Gather remains OFF in the launcher; no whole-game gain is claimed.

A replay-only paired benchmark (`RSX_REPLAY_VERTEX_BENCH=16`) now repeats both
CPU preparation paths against the same immutable per-draw data, alternating
order after warming both. Across 48,720 calls each, baseline costs 39.46 ms and
gather 31.45 ms: approximately 2.466 versus 1.966 ms per recorded frame for
packing plus fallback decoding only. It excludes reference collection/remap,
other RSX work, GPU execution and gameplay. This is a useful 0.50 ms local saving,
not the missing whole-frame solution. All 302 fallback draws are eliminated in
this fixture; actual run 86 also records zero fallback vertices in scene frames.

The user reports temporal flicker in shadows and vegetation. This remains an
open correctness issue; the geometry work above does not establish a fix.
Next visual diagnostics distinguish edge shimmer from whole-surface changes
and inspect consecutive frames, shadow depth and foliage transparency.

### Guest texture filtering and detail shimmer (runs 88–89)

The user clarified that the remaining symptom is minor detail flicker, rather
than whole surfaces disappearing. Capture 88 preserves a HUD-gated native
race capture and visual sequence in `logs/capture-88-detail-flicker`. Its
readback/capture overhead makes it unsuitable for FPS claims. The manual
Metal capture scope spans four present intervals; this does not establish four
independently selectable frames in Xcode. Temporal correctness remains open.

An audit of the 3045-draw recorded RSX replay finds 9315 actual sampled texture
bindings requesting 1x anisotropy, 288 requesting 2x, and 4449 with nonzero LOD
bias. These count sampled bindings, not merely enabled texture units. The old
Metal path omitted guest anisotropy and sampler bias. The neutral engine now
decodes both using the register layouts in
[RPCS3's texture implementation](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/RSXTexture.cpp).
`RSX_GUEST_ANISOTROPY=1` applies the requested anisotropy to linearly minified
mip chains, preserving point/unmipped filtering and the existing explicit
`PS3_ANISO` override. `RSX_GUEST_LOD_BIAS=1` applies signed 4.8 bias directly
through [Metal's sampler lodBias](https://developer.apple.com/documentation/metal/mtlsamplerdescriptor/lodbias)
on macOS/iOS 26 or later. Older SDK/OS builds retain zero bias with a diagnostic
when requested. The new options are OFF in the normal experimental launcher.

Metal documents S4.6 bias precision, so this is not bit-exact PS3 sampling.
The cache separately identifies signed bias, unsigned min/max LOD and effective
anisotropy. The GPU fixture tests negative/positive extremes and the game's
-0.796875 and -2 values, reset/reuse, clamp identity and unmipped filtering.
4096 LOD samples match independently constructed Metal samplers and analytical
mip-coloured results within precision tolerance; 256 anisotropy samples match
explicit Metal 2x filtering and all differ from 1x. Sampler bias applies to both
gradient-derived and explicit LOD in this test, consistent with the
[Vulkan LOD operation](https://docs.vulkan.org/spec/latest/chapters/textures.html#textures-level-of-detail-operation)
used by RPCS3's Vulkan backend. The neutral engine's sampler regressions pass.

The isolated replay completes all 3045 draws with the options enabled. Against
the baseline, anisotropy alone changes 7405 colour bytes (maximum 8/255); adding
guest LOD bias changes 467273 (maximum 170/255, mean absolute difference 0.2084).
These differences confirm that the states reach rendering, not that flicker is
fixed. Negative bias can increase detail and shimmer. The replay still has its
documented independent-reference limitations. No anti-aliasing, resolution or
texture-quality reduction is enabled to hide the symptom. Run 89 checks the
combined options in actual moving gameplay; temporal validation is still needed.

Run 89 reaches actual gameplay with car, terrain, vegetation and the HUD, but
the authored control plan again ends facing the wrong way; this is not a lap.
The 15-second window measures 24.13 FPS, 41.37 ms mean, 52.66 ms p95 and
164.17 ms maximum. RSX CPU is 25.30 ms/frame and GPU execution 11.30 ms/frame.
Run 87's corresponding values were 28.20 FPS, 17.83 ms RSX CPU and 11.29 ms
GPU. Dynamic trajectories and shared machine load preclude a causal comparison;
the unchanged GPU average does not establish a free quality improvement or a
CPU regression caused by filtering. Both options remain off by default, and
there is no claim that the user's temporal shadow/vegetation flicker is fixed.
With both options disabled, two replays differ from the archived baseline by
only 11 and 16 colour bytes respectively, each by one 8-bit step. This is not
byte-exact equivalence; the tiny repeatability difference has not been isolated.
The default path leaves the new Metal bias property untouched. The enabled GPU
fixture passes again after that guard, and the complete runtime patch's reverse
application check passes.

### Native diagonal MSAA experiment (runs 90–91)

The replay's coverage audit (`logs/coverage-audit-race-start.json`) records
1940 draws with surface mode 3 and AA control `0xffff0001`, and 1105
single-sample draws. No alpha-to-coverage or alpha-to-one is requested in
this fixture. Previously, mode 3 used a single-sample texture twice as wide
as the logical viewport. That preserves storage extent but does not reproduce
diagonal sample coverage or pixel shader invocation frequency.

`RSX_METAL_NATIVE_MSAA=1` now opts into actual Metal 2x render targets at the
logical pixel width. Each pass exports its individual colour, depth and stencil
samples back into the guest's 2x1 memory layout; subsequent passes import them
when loading previous content. This is a reversible per-sample transfer, not
an averaged resolve. The approach follows the storage model in RPCS3's
`Program/MSAA` shaders and Vulkan resolve helper. Read-only reference copies
and verified upstream blob identities are in `research/rpcs3-msaa/sources.json`.
Metal's default two sample positions match Vulkan's standard diagonal pattern;
physical PS3 sample ordering has not been independently verified.

The real-device tests in `logs/msaa-validation` pass with Metal API validation:
1024 samples each of RGBA8, FP16, FP32, depth and stencil survive four exact
round trips; all 256 stencil values and analytic diagonal edge coverage are
checked. A production-backend test checks 2x/1x/2x pass transitions, pipeline
creation, scissor scaling, immutable snapshots, depth-only clear preserving
stencil, and texture-handle reuse. Neutral engine tests verify forwarding and
reset of the sample layout/control. Other multisample modes, alpha-to-coverage
and partial sample masks are outside this experiment's supported states.

The 3045-draw replay completes with API validation disabled. Validation enabled
stops both the old baseline and the MSAA variant at the same existing error:
pipeline 201, texture unit 2 expects a cube texture but receives a 32x32 2D
surface (handle 321). The same-sized render-target alias route in
`sink_bind_textures` does not check cube dimensionality before returning a 2D
view. This remains a separate correctness issue; the full replay is not
validation-clean, and substituting a black texture would hide missing data.

Run 90 reaches native gameplay and preserves 20 visual samples over about
20.7 seconds. Car, terrain, vegetation, HUD and movement are visible. As in
earlier runs, the test control sequence finishes facing the wrong way; this
does not demonstrate a lap. The user's minor temporal flicker is not yet
proven fixed by these images. The MSAA option remains OFF by default.

Run 91 measures the actual game at 27.40 FPS over 15 seconds, with 411
guest flips, 411 completed FIFO frames and 411 backend present calls.
Mean frame interval is 36.52 ms, p95 45.53 ms and maximum 185.83 ms.
RSX CPU is 19.36 ms/frame, including 2.54 ms Metal encoding; GPU execution
is 13.42 ms/frame. CPU and GPU overlap. SPU interpretation is 0.083 ms/frame;
aggregate SPU policy/native-job CPU is 67.93 ms spread over worker threads.
This does not establish stable 30 FPS or an improvement over run 87
(28.20 FPS, 17.83 ms RSX CPU, 11.29 ms GPU): trajectories differ and the
machine is shared. Capture/OCR work is absent from the measurement window;
the post-window image shows the car, track and HUD at race time 24.86.
The complete measurements are in `logs/profile-91-native-msaa/report.json`.
Temporal quality and the pre-existing cube alias error remain open.

### Texture dimensionality: dynamic cubes and compressed volumes (runs 92–95)

The cube error identified above was a real binding error. Eleven dynamic
32x32 reflection cubes in the recorded fixture have six separately rendered
RGBA8 faces. The engine now gathers those faces into a Metal cube in command
order, using GPU blits and an immutable cache identified by all six source
handles and write serials. This follows RPCS3's cubemap-gather approach;
pinned reference sources are in `research/rpcs3-cube/sources.json`.
Ordinary memory-backed cubes keep their existing compressed mip chains.
Partially rendered cubes synchronize the available faces and preserve the
remaining guest data. Unsupported overlapping surface layouts are reported
explicitly; a partial region-merging implementation is not included.

The real-device cube test verifies face direction, RGBA8 and FP16 data,
component remapping, snapshot immutability and deferred source retirement.
Neutral engine tests also cover cache reuse, writes to later faces, guest
transfers, feedback, mip identity and fallback. An integration test caught
and fixed a command-record ordering bug: the clear collector previously
consumed the newly introduced cube-copy record. These tests and the existing
MSAA integration test pass. The 3045-draw fixture passes Metal API validation
with the cube gather, both with and without experimental MSAA; this remains
a binding/regression fixture, not an independent visual reference.

Native run 92 then exposed another real error after the previous cube
failure was removed: two 32x32x16 BC1/BC2 textures, each with six mip levels,
were being uploaded as 2D textures and bound to 3D shader arguments.
Compressed volumes were excluded from the old volume-upload branch.
Run 93 records their dimensions and formats with `RSX_VOLUME_AUDIT=1`.

Compressed BC1/BC2/BC3 volumes now decode every slice of every mip to an RGBA8
3D texture on cache fill. The decoded object is reused on subsequent draws;
ordinary 2D/cube compression paths are unchanged. The decoder is the original
[bcdec](https://github.com/iOrange/bcdec) v0.985 header, pinned to commit
`80859ed3b7afb1c527a2a99d70c61457bea72d0c` with its MIT license and file hashes
under `vendor/ps3recomp/third_party/bcdec`. The C decoder and engine path can be
shared with iOS; no iOS build or device validation is claimed.

Neutral tests check every texel across all 32 slices in six mip levels for
each compression format, including 2x2 and 1x1 mips and cache reuse. They
pass under AddressSanitizer and UndefinedBehaviorSanitizer. The GPU fixture
checks 224700 channel samples with Metal API validation: every uploaded RGBA8
byte reads back exactly at its intended slice and mip. Random BC blocks,
unaligned source addresses and guarded outputs are included. Independently
sampled Metal BC textures differ from the software decoder in 55165 channels,
by at most 2/255 (mean 0.254704/255). Both satisfy the independently calculated
palette and precision bounds in the
[Direct3D BC specification, sections 19.5.2 and 19.5.6–8](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm).
This establishes a format/layout check, not bit-exact hardware or PS3 decoder
equivalence. The original one-byte cross-decoder equality assertion was
invalid; it was replaced with the explicit reference calculation and a
separate exact upload assertion.

Run 93, which includes the cube fix but predates compressed-volume decoding,
measures 27.60 FPS over 15 seconds (414 guest flips/FIFO completions/presents),
36.16 ms mean frame interval, 47.71 ms p95 and 111.94 ms maximum. RSX CPU is
19.02 ms/frame, including 2.23 ms Metal encoding; GPU execution is 11.20 ms.
The post-window image confirms gameplay, car, terrain, foliage and HUD; the
control sequence again finishes facing the wrong way. This is not a lap or
stable 30 FPS. Evidence and test logs are in `logs/texture-dimension-validation`.
Native run 94 reaches the racing HUD with Metal API validation enabled and
completes the bounded 20-second control check without either texture-type
error or another reported Metal validation failure. Seven visual samples
over 19.25 seconds show the car advancing along the start straight, with
terrain, opponents, foliage, dust and HUD visible. This is not a completed
lap. The probe requested termination at the end and eventually killed the
process after graceful shutdown timed out; no clean shutdown is claimed.
Capture/OCR and validation overhead make this run unsuitable for FPS claims.
Minor temporal shadow/vegetation flicker remains open, and experimental
MSAA, guest anisotropy and guest LOD bias remain off by default.

Run 95 checks the complete cube/volume change without Metal API validation
or capture/OCR in the timing window. It measures **29.00 FPS over 15 seconds**
at the normal 1280x720 presentation, with 435 guest flips, FIFO completions
and backend presents. Mean frame interval is 34.51 ms, p95 41.01 ms and
maximum 116.49 ms. RSX CPU is 20.32 ms/frame, including 2.33 ms Metal
encoding; GPU execution is 11.46 ms and SPU interpretation 0.073 ms.
These overlapping lanes must not be added. The post-window frame at race
time 24.52 confirms a car, track and vegetation, but also the control plan's
collision and wrong-way outcome. This does not demonstrate a lap or stable
30 FPS, and differing trajectories/shared machine load prevent attributing
the FPS difference from run 93 to this fix. The complete report and build
identity are in `logs/profile-95-cube-volume-fixed`.

### Persistent shader translation cache (runs 96–97)

All seven slowest intervals in run 95 contain first-use shader builds
(17–45 ms within those intervals). They account for approximately 200 ms in
the 15-second recording; they are a source of hitches, not the whole frame
budget. The Metal translation path previously repeated HLSL -> SPIR-V -> MSL
on each new process. The sibling Vulkan translator already caches its own
translations. RPCS3 additionally persists raw shader/pipeline data and
preloads it before gameplay; pinned sources and verified Git blob identities
are in `research/rpcs3-shader-cache/sources.json`, particularly
[`rsx_cache.h`](https://github.com/RPCS3/rpcs3/blob/58317c9350983d71f18df2b0a346433161d6b13c/rpcs3/Emu/RSX/rsx_cache.h).

`PS3RECOMP_MSL_CACHE=<existing directory>` now opts into an on-disk cache of
exact translated MSL. It compares the full HLSL, shader stage, translator
build identity, glslang/SPIRV-Cross versions and runtime math option.
The payload has an integrity check and bounded size; truncated, corrupted,
incompatible or unavailable files fall back to translation. Writes use
separate process/serial temporary names and atomic replacement. Successful
translations alone are stored. This caches portable text, not GPU binaries;
Metal library/PSO creation still happens through the existing code. The
cache remains optional and is not enabled in the regular launchers yet.

New trace kinds 35/36 separate HLSL translation from Metal function creation;
kind 37 records cache outcomes. These times are nested inside the existing
shader-build and RSX intervals. The real-translator regression verifies exact
cold/warm/uncached output, a disk hit in a fresh process without translation,
distinct source/stage/math identities, corruption/truncation/trailing bytes,
small output buffers, unsuccessful shaders, missing directories and concurrent
readers. It passes, as do all 117 existing shader translation checks.

A same-input, 3045-draw replay provides the controlled component comparison:
all 375 shader translations hit disk on the second run. Translation CPU falls
from 7775.23 ms total to 28.49 ms total (wall 7803.44 to 28.74 ms). All 375
generated MSL files and the final 1280x720 image are byte-identical between
runs. Both complete with Metal API validation. This is a cache/correctness
fixture, **not gameplay FPS** or proof that the fixture itself is a visually
correct independent reference. Evidence is in `logs/msl-cache-validation/replay`.

Actual native run 96 starts with an empty cache: 28.00 recorded FPS, 35.73 ms
mean, 46.07 ms p95, 110.12 ms maximum. Its 22 shader requests all miss; the
recorded intervals contain 299.19 ms of translation and 2.61 ms of Metal
function creation. Run 97 reuses the cache: all 16 requests hit, with only
9.58 ms translation/cache lookup and 5.47 ms Metal function creation in the
recorded intervals. Nevertheless its overall throughput is **20.73 FPS**,
48.06 ms mean, 70.63 ms p95 and 342.07 ms maximum. RSX CPU also rises from
19.00 to 30.51 ms/frame. The largest stalls in run 97 contain no shader
translation, and several include long main-thread condition waits.
This is not a whole-game speedup result.

Post-window images show different collision/scene conditions and race clocks
(22.51 in run 96, 12.06 in run 97). A post-window screenshot does not establish
that the entire fixed 111–126-second window covered the same race phase.
The next benchmark must gate its control sequence and timing on an observed
racing HUD and stop/acknowledge screenshot readback before timing starts.
Unmeasured concurrent host load and different trajectories remain limitations;
neither a cache-induced FPS regression nor sustained 30 FPS is established.
Shadow/vegetation shimmer and the remaining synchronization stalls stay open.

### HUD-triggered measurement preparation

`tools/benchmark_hud_gameplay.py` requires two observed racing HUD images and
confirmation that the menu script's final event has fired. It then arms the
renderer with `PS3_FTRACE_TRIGGER=<path>`. The renderer publishes exact
`CLOCK_MONOTONIC` start/end timestamps to `<path>.ack`; the control plan uses
the same clock. `PS3RECOMP_METAL_FRAME_DUMP_PAUSE_DURING_TRACE=1` suppresses
diagnostic screenshot readback throughout lead-in and tracing. Ordinary
game readbacks remain unchanged.

The harness rejects late acknowledgements, screenshot changes during the
window, incomplete traces and missing post-window HUD evidence. It requires
a newly written screenshot after the window, rather than accepting the last
paused image. OCR runs outside the timing window. It records control event
times, pre/post images, binary hash, host load and timing acknowledgement.
The helper is not a deterministic replay: collision trajectories, race phase
and other programs' activity still require review before comparing FPS.

The standalone `tools/test_ftrace_trigger.py` in the runtime verifies disabled,
pending and legacy modes, eight concurrent trigger pollers, one-shot arming,
exact clock acknowledgement, pending thread names, event boundaries and
capture-pause lifetime. It passes normally and with Address/UndefinedBehavior
Sanitizer. The native executable and replay target build with the new hooks.
An actual HUD-triggered native run remains to be validated; this preparation
does not establish a further FPS improvement.

### Apple host-lock experiment

Run 97's later CPU sample contains 231 top-of-stack samples in
`spu_lockline_lock`. The Apple/POSIX implementation repeatedly exchanges one
shared atomic flag while another thread owns it. The Windows implementation
already has a different backoff strategy. Apple's
[game scheduling guidance](https://developer.apple.com/videos/play/tech-talks/110147/)
instead recommends avoiding busy waits and using owner-aware locks for mutual
exclusion on Apple silicon.

`SPU_LOCKLINE_HOST=1` now optionally selects `os_unfair_lock` on Apple
platforms. It uses fixed process-local storage and the existing same-thread
acquire/release pairs. The setting is latched before the first acquisition.
Guest reservations, invalidation notifications, PPU writes and SPU conditional
stores retain their original protected operations. The default remains the
original lock; the gameplay comparison below does not justify changing
that default. macOS and iOS share the implementation.

The actual-runtime lock fixture verifies a shared 128-byte payload under
six concurrent writers, publication, exact completion counts and latched
configuration. The 195 PPU/SPU coherence checks and 70 SPU-peer checks pass in
both modes, including Address/UndefinedBehavior Sanitizer runs. In the
synthetic 180,000-operation contention case, normal runs use approximately
425–542 aggregate CPU ms with the old lock and 19–20 ms with the native lock.
These numbers describe deliberately contended host locking under current
system load, **not a gameplay speedup**. Logs and source rationale are under
`logs/host-lock-validation`.

The largest run-97 frame also exposes a separate gap: its backend present
call lasts 261.81 ms, while recorded encoding ends 0.64 ms and GPU work ends
2.54 ms after that call starts. The existing trace does not identify the
remaining host-side work. Kind 38 now measures drawable acquisition, commit,
resource retirement, autorelease cleanup, frame dumping and capture setup
separately. Those stages overlap other trace lanes and must not be added to
them. The report also includes the overall backend-present interval. Neither
this gap nor the lock sample proves that host locking caused that specific
342.07 ms frame.

`PS3RECOMP_METAL_BACKGROUND=1` opens the normal windowed renderer without
activating it, allowing diagnostics while another Xcode project stays in
front. Occluded windows may report zero displayed FPS; guest throughput
alone is not evidence of visible interactive frame rate.

Native runs 98 (legacy lock) and 99 (Apple host lock) validate the HUD-triggered
harness. Both record 20 seconds, acknowledge the same monotonic controller
schedule, keep the screenshot unchanged throughout the window, and produce
fresh racing-HUD images afterward. Their executable hashes match. The
substantive configuration difference is `SPU_LOCKLINE_HOST=0/1`.

Run 98 records 26.80 guest FPS, 37.26 ms mean, 48.70 ms p95 and 153.87 ms
maximum; run 99 records 26.65 FPS, 37.57 ms mean, 48.94 ms p95 and 111.25 ms
maximum. Aggregate process CPU is 134.11 versus 134.99 ms/frame, and SPU-policy
CPU is 67.75 versus 67.57 ms/frame. There is **no demonstrated gameplay
throughput or CPU-efficiency improvement** from this lock change. The
synthetic contention advantage does not establish actual contention as the
current frame bottleneck. Leave the option disabled in the launchers.

The pre-window race clocks are 1.19 and 1.66 seconds; after-window clocks are
24.13 and 24.53 seconds. Both authored control sequences end in reverse near
the bridge supports. Images establish rendered car/terrain/HUD and movement,
not a lap, identical collision history, or temporal image quality. The game
window remains occluded, so these are guest-frame throughput measurements.
Both probes required forced shutdown after the normal termination grace.

No long present-stage delay recurs in either new window. Run 98's slowest
frame instead includes 93.71 ms in the main Havok completion wait and
90.63 aggregate Havok-worker CPU ms. Run 99's slowest frame includes a new
73.91 ms shader build; both runs encounter two disk-cache misses. Those are
distinct causes and neither supports treating the host lock as the main
remaining problem. The measured native async SPU routine (image 507917221) at `0xAAE8`
contains a 1000-iteration local-store delay between semaphore probes. The
checked event-wait experiment for that routine is described below.

### Native SPU semaphore backoff (runs 100/101)

The `0xAAE8` routine belongs to async image **507917221**, not WWS policy
image 900. Its preceding `GETLLAR` observes a zero semaphore and retains a
reservation; it then spends 1000 iterations repeatedly clearing one private
stack word before retrying the original atomic consumer.

`MOTORSTORM_SPU_SEMAPHORE_WAIT=1` replaces that delay with a reservation-change
notification and a 100 µs fallback timeout. The original semaphore read and
conditional decrement still decide when the job proceeds. The host waiter
does not consume guest events, fabricate successful atomics, or change a
reservation. Registration, predicate checking and notification share a fixed
lock order that closes the check-to-sleep race. Each waiter unlinks before its
context can be retired. The implementation uses pthreads shared by macOS and
iOS; only macOS has been exercised.

A generated copy of the original source inserts the opt-in change. The
generator checks the exact loop signature, and runtime guards require its
known register inputs, private aligned stack range, DMA buffer separation and
inactive diagnostics/lockstep. The original source remains intact. One final
original iteration produces the same registers and local-store bytes.
`python3 tools/test_spu_semaphore_wait.py` checks 2000 differential cases,
1000 wakeup races, already-lost reservations, timeout, late notification,
six sleepers, self-excluded DMA notification and preservation of event bits.
Both host-lock modes pass, including Address/UndefinedBehavior Sanitizers.
The existing 195 coherence and 70 peer checks pass in both modes.

Runs 100/101 use the identical executable and HUD-triggered 20-second control
sequence, with this option off/on. Both traces are complete without dropped
records or screenshot reads inside the window. The measured delay-loop CPU
falls from **3.509 to 0.132 aggregate ms/frame** (about 96%); calls fall from
90,467 to 18,956, including 6383 notification/spurious wakes, 12,448 timeouts
and 125 already-invalid reservations. No optimized-run call takes a guard
fallback. The original semaphore is rechecked after every wake.

Guest throughput is **27.20 versus 27.55 FPS**, mean frame time 36.784 versus
36.278 ms, and p95 48.087 versus 48.049 ms. GPU time remains 11.78 versus
11.71 ms/frame. Aggregate process CPU is 132.39 versus 121.74 ms/frame, but
different vehicle positions/collisions and shader cache misses also affect
these totals. The loop-specific saving is measured directly; the small FPS
difference is not a controlled claim of a general throughput gain. Neither
run proves sustained 30 FPS or fixes temporal shadow/foliage shimmer.

The windows remained behind other apps, so guest throughput is not visible
interactive FPS. Both runs render a moving car/track/HUD after the measurement,
and the authored braking sequence eventually reverses into the bridge area;
this is not a completed lap or identical collision replay. The probes end
with forced termination after their normal grace period. Keep this change
opt-in pending longer interactive validation. Evidence is in
`logs/semaphore-wait-validation/status.json` and `logs/profile-100-*` /
`logs/profile-101-*`.

### Counted vblank delivery and main-thread CPU (runs 102/103)

The native guest vblank handler at `0x0087EF88` (OPD `0x00F052F0`) increments
its frame-clock counter once per invocation. Our pending-bit implementation
could collapse several ticks into one invocation. The render thread produces
catch-up ticks after long FIFO drains, so this occurs during actual gameplay.
RPCS3's [`post_vblank_event`](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/RSXThread.cpp)
queues an interrupt callback for each tick, with argument 1.

`GCM_VBLANK_COUNTED=1` adds an atomic pending count while retaining the existing
serialized callback pump. Counts are published before the pending bit; a late
bit with an already-consumed count cannot fabricate a callback. A tick arriving
during delivery remains pending for a later pump. The legacy behavior is still
the default. Flip-completion checks, RSX semaphores and graphics settings are
unchanged. This change does **not** yet move the clock off the rendering thread.

The real GCM fixture passes in both modes, including late flip completion,
unflushed SetWaitFlip, delivery during an occupied pump, 37 delayed ticks and
10,000 ticks racing four consumers. Address/UndefinedBehavior Sanitizers pass,
as do the existing 40 flip-stream checks. The native game and replay tool build.
New trace probes measure caller-thread CPU at flip requests and count returned
vblank callbacks. A synthetic report fixture checks separate thread counters,
wall intervals and multiple callbacks sharing a pending bit.

Runs 102/103 use the same executable and HUD-gated 20-second control schedule;
apart from output paths, the only configuration change is the counted-vblank option. Both also enable
the preceding optional SPU semaphore wait. Run 102 delivers 1131 callbacks from
1200 queued notifications (69 merged), or 56.55 callbacks/second. Run 103
delivers 1200 callbacks with 1199 notifications inside the recording window;
the one-event difference is consistent with work crossing a recording boundary.
Its four shared pending-bit notifications no longer imply discarded callbacks.

Measured guest throughput is 27.65 versus 28.45 FPS, mean frame time 36.18 versus
35.16 ms, and p95 47.06 versus 42.13 ms. Maximum frames are 93.32 and 142.90 ms.
However, the final pre-window race clocks are **0.79 versus 7.54 seconds**, with
different traffic and later collisions. This is evidence that the counted
delivery works in the game, **not a controlled FPS improvement claim**. Neither
run establishes stable 30 FPS. Both shader caches have zero misses.

The new caller probe identifies thread 1 as the flip requester. Its CPU work
averages 14.63 versus 14.36 ms/frame; the remaining elapsed time is 21.55 versus
20.80 ms, comprising waits and scheduling delays. RSX CPU is 20.05 versus
18.07 ms/frame, and GPU execution is 11.68 versus 11.03 ms. These lanes overlap
and must not be added. The slowest counted-mode frame includes a 65.84 ms main
condition wait despite only 18.33 ms of main-thread CPU, so preserving vblank
events alone does not explain or remove the long stalls.

Both traces have no dropped records or in-window image readbacks and produce
fresh car/track/HUD images afterward. The window stayed behind other apps;
these are guest throughput measurements, not visible interactive FPS. The
script eventually reverses near the bridge; no lap or temporal shadow/foliage
quality is established. Both bounded probes require forced shutdown after
their grace period. Keep counted delivery opt-in while improving race-phase
alignment and separating clock-production delay from main-thread waiting.
Evidence: `logs/vblank-queue-validation/status.json` and `logs/profile-102-*` /
`logs/profile-103-*`.

### Independent display clock (runs 104–106)

`MOTORSTORM_VBLANK_THREAD=1` starts a lightweight monotonic 59.94 Hz producer
after backend initialization and requires `GCM_VBLANK_COUNTED=1`. The producer
only publishes ticks. Guest callbacks stay under the existing pump guard;
flip completion, FIFO draining, render resources and Metal calls stay on the
render owner. This follows the separation used by RPCS3's `VBlank Thread`.
The implementation uses standard C++ threads/timing and does not depend on a
macOS display API; iOS execution is still untested.

The shared 64-bit vblank counter is now atomic and its getter is a read,
matching [RPCS3's API](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellGcmSys.cpp).
Previously every query manufactured another tick and returned only 32 bits.
The callback fixture checks concurrent reads and the 32-bit boundary, in
addition to the earlier queued-tick and flip-ordering cases. Both normal and
Address/UndefinedBehavior Sanitizer runs pass, as do the 40 flip-stream checks
and native/replay builds. The new clock rejects startup without counted mode.

The HUD harness now supports `--race-time-min 10`, with a 1.5-second maximum
OCR overshoot and the existing two-second capture-free lead-in. Accepted
pre-window clocks are 10.36, 10.05 and 10.37 seconds. All three runs use the
same executable, settings and authored control schedule, apart from clock
mode in 104/105 and callback-dispatch mode in 105/106. This improves alignment
but does not make collision history or host scheduling deterministic.

Run 104 uses counted ticks on the render thread: 29.45 FPS, 33.97 ms mean,
38.58 ms p95 and 89.42 ms maximum. Its vblank intervals have 24.73 ms p95,
32.87 ms maximum and 55 gaps over 25 ms.

Run 105 enables the independent producer: 29.85 FPS, 33.51 ms mean, 35.22 ms
p95 and 44.43 ms maximum. Vblank intervals have 17.57 ms p95 and 17.68 ms
maximum, with no gaps over 25 ms or catch-up gaps under 8 ms.

Run 106 additionally enables the existing `MOTORSTORM_GCM_CALLBACK_THREAD=1`:
29.85 FPS, 33.51 ms mean, 34.86 ms p95 and 42.00 ms maximum. Mean callback
delivery delay falls from 0.605 to 0.015 ms, and p95 from 2.882 to 0.069 ms.
That directly verifies prompt delivery; it produces no additional average-FPS
gain in this comparison. Main PPU CPU averages 12.87 ms, RSX CPU 13.35 ms and
GPU execution 10.38 ms per frame. These are overlapping lanes, not a sum.

All three traces are complete with no dropped records or in-window screenshot
reads, and all produce fresh car/terrain/HUD images afterward. Runs 105/106
each have no frame over 50 ms in their 20-second windows. This is promising
pacing evidence, not proof of long-term 30 FPS, full-grid performance, image
stability or visible interactive playback. The test windows remain occluded,
and the braking plan ends in reverse. No resolution or image-quality setting
was reduced. The combined paths are now included in `play-experimental.command`
for extended validation; shared SDK options remain opt-in.

Evidence is in `logs/vblank-clock-validation/status.json` and runs 104–106.
Next, cover the opening full-grid phase and a longer driving interval before
promoting this configuration beyond the experimental launcher.

### Opening-grid validation and scripted input release (run 107)

`PAD_SCRIPT_STOP_FILE` lets the HUD-aware harness cancel pending and held
scripted menu presses before recording gameplay. A native acknowledgement
confirms that cancellation has latched; independent file, keyboard and
controller input remain available. The option is absent during normal play.
Without `--cancel-menu-on-hud`, the benchmark retains its previous guard that
waits for the last scheduled menu press to drain.

The input fixture verifies release of a held confirmation, cancellation of
future Start/Circle presses, preservation of independent throttle and pressure,
and continued keyboard input. Normal and Address/UndefinedBehavior Sanitizer
runs pass. The fixture now supplies its own SDL host stubs: the original
sanitizer attempt stopped before `main` inside an SDL startup error dialog,
so that attempt was not counted as a test result. Native/replay builds and the
runtime-patch reverse-apply check also pass.

Run 107 begins from an observed race clock of 0.59 seconds, followed by a
one-second capture-free lead-in. Over 30 seconds it completes 853 frames:
28.43 guest FPS, 35.17 ms mean, 46.55 ms p95 and 121.19 ms maximum. Main PPU
CPU averages 14.43 ms/frame, RSX CPU 18.05 ms and GPU execution 10.73 ms.
The CPU/GPU lanes overlap. The independent clock remains regular: 17.52 ms
p95 and 18.07 ms maximum between vblank publications.

The shader cache serves 29 of 35 requests. The worst frame spends 92.81 ms
translating a shader and only 0.66 ms creating its Metal library; its GPU work
takes 4.29 ms. This identifies a concrete first-use hitch, not the explanation
for every slow frame. Reuse helps previously encountered shaders; coverage of
new race scenes and precompilation still need investigation.

The trace has no dropped records or in-window screenshots. Its first image
shows the starting grid, but the final image shows a cliff closeup and the HUD
clock reads only 22.47 seconds. Crash/slow-motion effects or other timer behavior
have not been separated. Do not treat this as verified normal-speed driving
for 30 seconds. A separate screenshot-based driving probe is used to inspect
the route and controls; that probe is explicitly unsuitable for FPS claims.

### Unused texture dimensions and shader duplication (runs 109–111)

All six translation misses in run 107 differ from already cached HLSL only
in one texture declaration: cube/volume versus 2D on a unit the shader never
references. Four pairs produce byte-identical MSL; the other two have compiler
identifier changes. The largest hitch therefore rebuilt a program for state
that cannot affect its sampling.

`RSX_FP_SAMPLED_DIMENSIONS=1` intersects the cube and volume masks with the
program's existing static sampled-unit mask before pipeline lookup and HLSL
generation. Used units retain their type, slot and sampling operation. The
existing metadata cache or instruction walk supplies that mask; no image
contents or runtime branch result is used. Built-in fallback behavior is
unchanged. This is portable C and remains optional in the shared SDK.

The production engine fixture exercises both modes and metadata paths,
TEX/TXP/TXL/TXB, used slots 0/15, changes to every other slot, and used
2D/cube/volume transitions. Thirty unused dimension changes create thirty
redundant pipelines in legacy mode and none in the filtered mode. Normal
and Address/UndefinedBehavior Sanitizer runs pass. Existing full draw-engine
and shadow-engine fixtures also pass with sanitizers and the option enabled.
Native and replay builds succeed.

Replaying the same 315,068 commands and 3,045 guest draws with Metal API
validation reduces shader functions from 375 to 369. The 1280×720 outputs
differ in one green-channel byte, 140 versus 141 at (156,104); every other
channel is identical. This is a regression comparison against the recorded
frame's existing output, not a claim that that old reference frame has correct
lighting or establishes native gameplay performance. Cold replay time is not
a speedup result: the new HLSL forms also encounter cold Metal driver entries.

Run 110 validates native menus and the race with Metal API checks enabled.
Car, track, traffic and HUD remain visible without reported Metal validation
errors. The validation overhead slows simulation substantially, so its
wall-timed steering plan cannot validate the normal-speed bridge approach.
Run 109 also misses the bridge and crashes; route control is still unfinished.
The visual harness now starts after the first HUD's cancellation acknowledgement,
without waiting for another successful OCR result.

Run 111 disables Metal validation and all in-window image capture for eight
seconds of straight throttle from the grid. It completes 210 frames with no
dropped trace records. Its 13 translation requests are all disk-cache hits.
Main PPU CPU averages 17.23 ms/frame, RSX CPU 29.26 ms and GPU execution
13.52 ms; SPU interpretation is 0.078 ms. The longest frame has 77.30 ms of
RSX CPU but only 1.11 ms of shader-build time. Further draw-phase diagnosis
is needed; shader reuse alone does not achieve 30 FPS in this scene.

Evidence: `logs/sampled-dimensions-validation/`, `logs/drive-109-*`,
`logs/drive-110-*` and `logs/profile-111-*`.

[Apple's binary archives](https://developer.apple.com/documentation/metal/metal-binary-archives)
target precompiled Metal GPU code. They remain a possible later improvement,
but the measured run-107 hitch was primarily in our HLSL-to-MSL translation,
before that stage. This change addresses that demonstrated duplication first.

### Opening-grid CPU attribution (runs 112–113)

Run 112 enables per-draw phase timers. Across 191 completed frames, it
records about 3,526 draws/frame. Nested RSX CPU averages are 8.66 ms for
geometry preparation, 7.28 ms for pipeline lookup, 6.83 ms for staging and
submission, 4.98 ms for textures and 0.87 ms for targets. These overlap the
overall RSX total and must not be added to it. Clock probes themselves cost
CPU time, so the diagnostic's 23.875 FPS is not a normal-play measurement.
Its worst frame also encounters a new shader: 23.48 ms HLSL translation and
256.51 ms Metal-library creation. First-use Metal compilation remains a
separate hitch to address.

Run 113 samples the native process's stacks during the opening grid.
Reference deduplication is the most sampled standalone geometry helper
(59 leaf samples), followed by vertex packing (28); vertex-program input
analysis has only seven. This changes the next action from caching program
analysis to investigating repeated reference remaps. The all-thread sampler
strongly disturbs gameplay (13 FPS in this diagnostic); it is used for
attribution only, not as a performance comparison.

`RSX_VERTEX_REMAP_CACHE=1` is an opt-in experiment that memoizes the pure
reference remap. Every input byte, including the base index, must match before
reuse; a hash only selects candidate entries. Vertex attributes, restart cuts
and topology are still processed for every draw. The cache holds at most
32 MiB of payload plus entry metadata, with the original algorithm retained
for small/oversized lists, allocation failure and a full cache.

The differential fixture compares over 17 million references against the
original general remapper, including first-use order, mutations, forced hash
collision, eviction, memory budget and destruction/reallocation. Sanitizers
and the full draw-engine/shadow fixtures pass. Metal API validation replay
with the option off/on produces byte-identical 1280×720 images. These checks
establish regression coverage; native gameplay impact is measured separately.
Evidence is in `logs/vertex-remap-cache-validation/`.

Runs 114/115 compare the same binary with remap reuse on/off: 25.5 versus
23.875 guest FPS, RSX CPU 29.19 versus 32.04 ms/frame. This is not an isolated
6.8% speedup: the off run encounters one cold shader/Metal hitch, with a
499.82 ms maximum frame, and the OCR race gates differ by 0.43 seconds.
After excluding that one compilation interval for diagnosis, mean intervals
are 39.25 ms on versus 39.51 ms off. Those selected intervals are not a
replacement gameplay FPS result.

Run 116 enables phase timers with remap reuse. Geometry takes 8.31 ms/frame
at about 3,670 calls/frame, versus 8.66 ms at about 3,526 calls/frame in run
112: 2.26 versus 2.46 microseconds per call. This supports a modest geometry
improvement, but varying draws/AI and diagnostic overhead limit attribution.
Pipeline lookup and staging remain substantial. The experimental launcher
does not enable the remap cache. All three native captures complete without
dropped capture windows or in-window screenshots; the post-frame car and
track are visible, but extended driving and stable 30 FPS remain unverified.

The CPU sample also identifies a further lead in the already specialized
native culling block `0x8480`: frequently sampled instruction offsets are
loads immediately following FPCR rounding-mode writes. This is a lead for
investigation, not proof of hardware stall duration. Any optimization must
preserve exact arithmetic and restore host FPCR before callbacks. Assembly,
test results and the decision are recorded in
`logs/vertex-remap-cache-validation/status.json`.

### Exact bounded culling arithmetic (runs 117–118)

The sampled `0x8480` block already uses native ARM64 arithmetic with a single
round-toward-zero scope. Widening that scope across guest branches would
bypass existing interrupt and runtime dispatch boundaries, so these boundaries
are preserved. This experiment instead removes repeated range checks inside
the block when one conservative input check proves them redundant.

`port/spu_cull_bounds.h` accepts only positive zero or normal inputs whose
magnitude is at least 2^-30 and less than 2^30. For this particular sequence
of three products and one added input, all nonzero intermediate values remain
normal and within the IEEE single range. Explicit ARM64 arithmetic retains
round-toward-zero and every intermediate rounding; zero is normalized to the
SPU representation. Inputs outside the proof use the existing exact scoped
helpers. The source generator checks the original block's SHA-256 before
applying the specialization, so changing the lifted code requires re-auditing
the proof. Local-store callbacks occur before the rounding-mode change, and
the original mode is restored before either original exit.

The option also uses an integer NEON comparison that preserves SPU signed
magnitude ordering, including exponent-255 values and exponent-zero inputs.
It does not substitute an IEEE floating-point comparison. No PS3
synchronization, physics step, geometry, shader, texture or quality setting
is removed.

Validation passes 300,000 full-context and branch comparisons, including
200,000 eligible inputs; four million comparison lanes; every input-register
boundary check; and 40,000 checks of alternate caller rounding modes and
callback state. Address/undefined-behavior sanitizers pass too. The isolated
block microbenchmark measures 98.88 ns for the previous scope versus 74.65 ns
for the guarded implementation. These are CPU microbenchmark results, not
gameplay FPS.

Both native runs use the same executable, 1280×720 internal rendering, the
same control plan and an eight-second opening-grid window, without in-window
screenshots or dropped trace records:

- Run 117, enabled: 27.25 guest FPS, 36.79 ms mean, 46.71 ms p95, 102.60 ms
  maximum. RSX CPU 28.01 ms/frame, main PPU CPU 16.52 ms/frame, GPU 14.64
  ms/frame. All 13 shader requests hit the disk cache.
- Run 118, disabled: 26.125 guest FPS, 38.26 ms mean, 47.51 ms p95, 88.95 ms
  maximum. RSX CPU 29.39 ms/frame, main PPU CPU 16.98 ms/frame, GPU 14.07
  ms/frame. All 12 shader requests hit the disk cache.

The OCR gates differ by 0.29 seconds, and the post-window cars occupy
different positions in traffic. The largest measured CPU reduction is on
the RSX thread, rather than an isolated culling counter; this prevents
attributing the entire FPS difference to this specialization. Aggregate SPU
policy CPU is 79.58 versus 80.03 ms/frame across concurrent workers and must
not be added to the other frame-time lanes. Actual guard eligibility in
gameplay has not yet been counted.

Keep `MOTORSTORM_CULL_FAST_BOUNDS=1` opt-in. The next discriminating check is
actual gameplay guard coverage and culling cost, rather than assuming the
microbenchmark gain explains the whole-frame difference. The images show
the car, lower route and HUD, but do not establish a completed lap, corrected
temporal shimmer or sustained 30 FPS. Evidence, source hashes and decisions:
`logs/cull-bounds-validation/`, `logs/profile-117-cull-bounds-grid/` and
`logs/profile-118-cull-scope-grid/`.

### Culling attribution and a rejected follow-up (run 119)

`MOTORSTORM_CULL_PROFILE=1` samples one in 64 complete calls to the original
`0x8380` routine, preserving its original call/drain/restore sequence. It
counts `0x8480` eligibility after the original local-store load and before
changing FPCR. Thread-local diagnostic state never retains a host-stack
pointer across guest dispatch; nested or abandoned calls invalidate a sample.
The new diagnostic variants pass 6,000 full-context comparisons, sampling,
callback and unwind-state checks, including sanitizers.

Run 119 records 172,815 sampled complete calls and 367,998 sampled `0x8480`
blocks. **98.944%** are eligible for bounded arithmetic. The other 3,886
blocks each contain a small normal input below the proven lower bound; no
negative-zero, subnormal or large-input rejections occur in this sample.
The range is not widened on that observation alone.

Inclusive sampled call CPU averages 530 ns (459 ns median, 958 ns p95).
Multiplying by the sampling stride estimates 27.92 ms of aggregate CPU per
guest frame, including setup, arithmetic, dispatch and diagnostic overhead.
This is not 27.92 ms of frame latency: workers overlap, the timer and rejected
input classification add cost, and systematic sampling is an estimate.
The diagnostic's 26.25 guest FPS is not a speed comparison. The trace has no
dropped records or in-window screenshots, and the post-window car/track/HUD
remain visible. `tools/report_cull_profile.py` preserves coincident timestamps
from different workers rather than dropping or arbitrarily pairing samples.
Evidence: `logs/cull-profile-validation/attribution.json`.

An additional arithmetic scope around the `0x8380` setup passes 100,000
full-context/branch comparisons and 4,000 alternate-FPCR checks, but is slower
in the isolated fixture: 64.24 ns scoped versus 52.89 ns original (sanitizers:
102.78 versus 89.74 ns). It is retained only as a reproducible test experiment,
not generated into the game. This rejects a plausible optimization before
another native benchmark. Evidence: `logs/cull-setup-validation/`.

### Validated fragment-program length reuse (runs 120–122)

The earlier stack sample identifies repeated `rsx_fp_program_size` calls.
The engine decodes the fragment program's length separately for vertex layout
and pipeline selection, even when its instruction metadata is already cached.
`RSX_FP_CACHED_SIZE=1` reuses the length only after resolving the entire cached
memory span and comparing every instruction byte. An address alone is not a
hit. Mutation of END, source kinds or any other instruction falls back to the
original decoder; unsupported/uncached programs retain the original path.
Inline constants are still read for every draw.

This follows the same instruction/constant separation visible in
[RPCS3's fragment-program comparator](https://github.com/RPCS3/rpcs3/blob/58317c9350983d71f18df2b0a346433161d6b13c/rpcs3/Emu/RSX/Program/ProgramStateCache.cpp#L739).
The locally saved primary sources match their recorded Git blob hashes.
No RPCS3 emulator timing is used as evidence for this native change.

The engine fixture checks both modes, actual cache hits, changing instruction
streams at the same address, moving END, adding/removing inline constants,
live constant updates, unreadable tails, eviction and shutdown/reinitialization.
It and the existing metadata, draw-engine, shadow and sampled-dimension tests
pass with address/undefined-behavior sanitizers. Replaying 3,045 recorded guest
draws with Metal API validation yields byte-identical 1280×720 output in both
modes, SHA-256 `3fb8df0f896e3b3658e57aa2f8b04c57d3d1c9f26839ba11a1a04b3a6b2c3832`.

Native run 120 enables reuse: 27.25 guest FPS, 36.70 ms mean, 46.08 ms p95,
84.69 ms maximum, RSX CPU 27.03 ms/frame and GPU execution 13.84 ms/frame.
All 13 shader requests hit the disk cache. Run 121 disables reuse but encounters
one new shader: 38.45 ms HLSL translation plus 284.86 ms Metal-library creation
inside a 436.49 ms frame. Its 25.125 FPS is therefore not an isolated comparison.

A targeted warm-cache repeat, run 122, has zero shader misses and measures
26.25 FPS, 38.17 ms mean, 47.63 ms p95, 80.01 ms maximum, RSX CPU 29.06 ms/frame
and GPU execution 13.75 ms/frame. All three use the same executable and
`MOTORSTORM_CULL_FAST_BOUNDS=1`, without culling/phase diagnostic timers,
dropped trace records or in-window screenshots. The enabled/disabled warm
OCR gates differ by 0.36 seconds. The post-window car is rolling after a
collision in runs 120 and 122, so camera/traffic are not identical and these
are not evidence of successful extended driving.

These results support retaining the cache in the **experimental** launcher,
with the bounded culling path, while shared runtime defaults remain off and
both options can be overridden with `0`. The arithmetic differential tests
and recorded-frame comparison support preserving semantics; they do not
prove that existing temporal shimmer is fixed. Sustained 30 FPS, a complete
race, 60 FPS and iOS execution remain unverified.
Evidence: `logs/fp-cached-size-validation/`, `logs/profile-120-*`,
`logs/profile-121-*` and `logs/profile-122-*`.

### Fragment-hash attribution, before changing cache policy (run 123)

An opt-in `RSX_FP_HASH_PROBE=1` diagnostic classifies each fragment hash
request as a cached seed, freshly rebuilt metadata, changed vertex-derived
seed, or unavailable metadata. It preserves the original hash implementation
and times only recomputations. Per-present trace kinds 1004/1005 contain
counts, instruction bytes and CPU nanoseconds; the gameplay report extracts
them. The actual-engine ASan/UBSan test checks all four classifications,
unchanged hash results (including zero and maximum seeds), and disabled
diagnostics. Both game and replay targets build successfully.

Run 123 uses the promoted culling/length-cache configuration and additionally
enables draw-phase timers. Of 732,077 hash requests, **624,838 (85.351%) reuse
the cached seed** and **107,239 (14.649%) follow metadata rebuilding**.
There are **zero changed-seed requests and zero no-metadata requests**.
Recomputation consumes 209.25 ms across 214 aggregate frame records, about
0.978 ms per record including CPU-clock overhead. Expanding the number of
remembered seeds therefore has no demonstrated benefit in this workload and
is not implemented. The next useful distinction is whether metadata rebuilds
come from new instruction contents, reused guest addresses, or capacity
evictions; these counters alone do not identify the cause.

Nested phase diagnostics report geometry 7.71, pipeline lookup 6.39, textures
4.53, staging/submission 6.47 and targets 0.80 ms per record. These are part
of RSX CPU time, not additional frame latency. The run's 26.75 guest FPS,
37.36 ms mean frame interval and 32.38 ms RSX CPU include both sets of
diagnostic timers and must not be treated as a new optimized benchmark or
compared directly with uninstrumented runs 120/122. GPU execution averages
14.03 ms; all 12 shader-translation requests hit the disk cache.

The trace completes with no dropped records, no screenshots inside the timed
window and no harness error. The post-window image at race time 00:10.32
shows the car, track, opponents and HUD. This single image does not validate
temporal shimmer or extended driving. Evidence:
`logs/fp-hash-probe-validation/` and `logs/profile-123-fp-hash-probe/`.

### Reusing metadata when an unchanged shader moves (runs 124–126)

Run 124 adds a read-only exact-content search on metadata misses. Of 110,401
misses, **90,373 (81.859%) already have identical instruction contents in
the 64-entry cache**, all at a different resident address. The remaining
20,028 have no current match. No same-address instruction mutations occur
in this window. The searches cost 24.82 ms total, versus 215.49 ms of
subsequent fragment hashing alone. Both include probe overhead; metadata
construction itself adds further work. This changes the optimization choice
from extra hash seeds to reuse across guest addresses.

`RSX_FP_RELOCATE_CACHE=1` retains the existing address cache and eviction policy.
On a miss it searches for exact instruction equality, including length,
instruction order and inline-constant source kinds. A hit copies only the
used metadata arrays and cached hash into the destination's separately owned
entry. All live constant values are still read from the new guest address.
An unmatched or unsupported program takes the original decoder. Identical
addresses, hashes or instruction prefixes alone never authorize reuse.
This uses the content/constant distinction in the pinned RPCS3 comparator
and its content-keyed fragment-program lookup; no emulator performance result
is substituted for native testing.

ASan/UBSan tests cover 5,000 randomized programs and independent metadata
copies, instruction mutations, zero/max hash seeds, changed constants at the
new address, changed vertex-program seeds, same-address changes, shortening
and extending code, eviction, shutdown/reinitialization, maximum-size programs
and unsupported fallback. Draw-engine, sampled-dimension and shadow tests
also pass with the option enabled. The 3,045-draw Metal API validation replay
is byte-identical with the option off/on, SHA-256
`3fb8df0f896e3b3658e57aa2f8b04c57d3d1c9f26839ba11a1a04b3a6b2c3832`,
and has no API validation errors.

The same native executable, unchanged 1280×720 settings and eight-second
opening-grid procedure produce:

- Run 125, enabled: 28.0 guest FPS; 35.79 ms mean, 44.58 ms p95, 88.63 ms max;
  RSX CPU 26.66 ms/frame, GPU 13.87 ms/frame.
- Run 126, disabled: 27.0 guest FPS; 37.09 ms mean, 47.13 ms p95, 88.70 ms max;
  RSX CPU 28.36 ms/frame, GPU 13.55 ms/frame.

Both have 13 shader-cache requests, all hits, no phase/culling/hash diagnostic
timers, no dropped trace records and no in-window screenshots. The observed
race gates differ by 0.26 seconds and traffic/collisions are not deterministic:
the post-window positions are 15th and 11th. These are promising lower-RSX-CPU
and short-window throughput results, not a precise causal whole-game speedup.
The new option is enabled in the experimental launcher, with an explicit `0`
override; the shared runtime default remains off.

Temporal shadow/foliage shimmer, extended controlled driving, sustained 30 FPS,
60 FPS and iOS remain unverified. Evidence:
`logs/fp-relocation-validation/`, `logs/profile-124-fp-relocation-probe/`,
`logs/profile-125-fp-relocation-1/` and `logs/profile-126-fp-relocation-0/`.

### Detailed RSX attribution and vertex metadata experiment (runs 127–129)

`RSX_ENGINE_DETAIL_TRACE=1` adds nested geometry, staging and vertex-hash
phases to the existing optional timers. Run 127 attributes about 2.00 ms CPU
per aggregate frame to vertex-program hashing. The diagnostics themselves
add substantial clock-reading overhead: its 21.625 FPS is not an optimized
performance result. The largest individual target-setup phase takes 35.34 ms
in guest frame 5077. That frame also encounters a new 4096×4096 B8 surface
and rejected self-feedback; causal attribution requires separate seed and
allocation measurements.

`RSX_VP_METADATA_CACHE=1` stores the original hash and input-mask analysis for
64 vertex programs. XXH3 selects a two-way bucket, but length and every
instruction byte must also match. Live transform constants remain outside the
cache. ASan/UBSan tests cover 2,500 randomized programs, instruction-byte
mutations, forced selector collisions, bucket eviction, relocated instructions,
changed END/input masks, maximum lengths and engine reinitialization.
Draw-engine/shadow/dimension tests pass. Off/on Metal API validation replay
is byte-identical to the preceding 3,045-draw reference.

With detailed timers disabled, the same binary gives 27.25 FPS enabled
(RSX CPU 25.00 ms, GPU 13.68 ms) and 28.25 FPS disabled (RSX CPU 25.45 ms,
GPU 13.84 ms). Both shader caches are warm; both traces complete without
drops or in-window images. Gates differ by 0.44 seconds, and reviewed
post-window images show different car trajectories. This does not establish
either a whole-game gain or a causal regression. The option stays disabled.
Evidence: `logs/vp-metadata-validation/`, `logs/rsx-detail-validation/` and
`logs/profile-127-*` through `logs/profile-129-*`.

### Correct guest initialization of B8 surfaces (runs 130–131)

The generic engine allocated B8 targets as host RGBA8, then incorrectly used
that host format to interpret the guest backing bytes as tightly packed
A8R8G8B8. A 4096×4096 B8 surface therefore requested 64 MiB instead of
16 MiB; smaller targets with a 4096-byte guest pitch also read the wrong rows.
The existing CPU readback already treated B8 as one byte stored in host blue,
so the two directions disagreed.

The optional typed initializer uses the guest color format and pitch. Linear
B8 expands one source byte into host RGB, with alpha one and the significant
byte in blue. Linear A8R8G8B8 retains its channel conversion while respecting
guest row spacing. The requested source span ends at the last actual pixel,
without requiring final-row padding. Invalid lengths/pitches, address overflow,
unbacked local memory and unsupported layouts do not become guessed ARGB
uploads. Existing targets are retained without repeated initialization.
The shared runtime default remains off; the experimental launcher enables it.

ASan/UBSan tests exercise exact-size B8 source buffers, 230 odd/padded
ARGB/B8 layouts, channel values, bounds and invalid descriptions, and the
actual dispatch-to-backend initialization path. The existing engine, shadow
and sampled-dimension suites pass with the new option enabled. Metal API
validation replay of the earlier 3,045-draw race-start frame remains
byte-identical; that recording predates these later B8 allocations and does
not by itself validate them.

`RSX_SURFACE_SEED_TRACE=1` measures only target initialization and allocation,
without enabling per-draw CPU-clock probes. Same-binary eight-second warm
gameplay comparisons at 1280×720 show:

- Run 130, legacy initializer: 28.25 guest FPS; 35.49 ms mean, 43.67 ms p95,
  83.52 ms maximum. Nine seeds in one frame cost 30.22 ms CPU and Metal
  allocation/upload costs 18.31 ms. The largest seed alone costs 23.39 ms.
- Run 131, typed initializer: 28.625 guest FPS; 34.92 ms mean, 44.04 ms p95,
  52.45 ms maximum. The same nine seeds cost 5.20 ms CPU and allocation/upload
  14.53 ms. The largest seed costs 3.89 ms.

Both have 14 shader-cache requests, all hits, no lost records, no screenshots
inside the timing window and no harness errors. Race gates differ by 0.06 s.
Reviewed post-window images show the car on the course, opponents and HUD,
but different trajectories. The isolated conversion saving is well supported;
the small average-FPS difference is not a precise whole-game speedup. Neither
stable 30 FPS nor temporal shimmer is established. Normal launch disables
the diagnostic timers.

The B8 self-feedback rejection remains. Recorded state is texture/target
offset `00A81300`, 4096×4096, pitch 4096, texture format A1, 13 declared
levels, filter `01013F34`, control0 `80078000`. The minification filter is
nonmipmapped, which is a useful lead, but channel mapping, guest mip surfaces
and ordered source snapshots still need validation before enabling those
draws. Pinned RPCS3 format and surface-layout sources, with verified Git
blob hashes, are in `research/rpcs3-surface-format/`. The refreshed Windows
upstream surface code was also inspected; its generic full-resource feedback
copy does not resolve these format/layout requirements.

Evidence: `logs/surface-seed-validation/`, `logs/profile-130-surface-seed-0/`
and `logs/profile-131-surface-seed-1/`.

### B8 feedback candidate (runs 132–136, validation in progress)

Run 132 captures the previously rejected fragment program
`54447b2f654ad468`. Both texture reads are ordinary samples of the blue
component. Pinned RPCS3 `VKDraw.cpp` keeps the sampler at level zero when
the minification filter does not sample mipmaps; 13 declared levels alone
do not require reading all of them. This provides a concrete, narrow case
for restoring the draw instead of guessing the texture layout.

`RSX_B8_SURFACE_FEEDBACK=1` adds an ordered GPU conversion from the blue
byte of the RGBA8 backing target into an immutable R8 texture. It handles
supported self-feedback and matching aliases; an alias snapshot is reused
only until the producer changes. Format, pitch, extent, sample count,
filter, LOD and newer-overlapping-writer guards remain mandatory. The
option is **not enabled in the experimental launcher**.

The actual engine/Metal test passes with API validation, covering all 256
byte values, queued writes, blue-only write masks, consumer ordering,
alias reuse/invalidation and unsupported-layout rejection. The existing
color-feedback checks also pass (8,731 checks, zero failures). CPU engine,
shadow and sampled-dimension suites pass under ASan/UBSan. Two old Metal
test-fixture issues were corrected: default samplers must be created when
bypassing app startup, and private depth snapshots require a GPU readback
instead of direct CPU access.

Run 133 is rejected by the race-clock gate and is not a timing result.
Run 134 completes an eight-second gameplay window with Metal API validation
and no reported Metal errors, but does not reach the B8 allocation/copy
path. Its reviewed image shows the car, opponents, track and HUD near the
opening grid. API validation adds substantial CPU overhead; this run is
not comparable to normal performance measurements. Run 135 extends the
diagnostic drive to 30 seconds and exercises the path: the periodic counter
reaches 138 B8 copies, with no reported Metal validation errors. However,
eight logged rejections now name mip-filtered consumers (`02043F34`), so
the single-level candidate is incomplete.

The pinned RPCS3 `TextureUtils.cpp` implementation and the game's observed
render-target addresses establish another error: linear mip levels retain
the same guest row pitch. The shared layout helper previously applied it
only to level zero. It now preserves the pitch at every linear uncompressed
level; swizzled and compressed layout handling remains as before. A poisoned
padding fixture and the nine observed MotorStorm offsets test that behavior
independently of the new gather implementation.

The candidate now assembles an immutable R8 mip chain in command order.
Rendered levels come from the blue bytes of live GPU surfaces; untouched
levels come from the corresponding guest bytes. It does not synthesize
mipmaps. Producer handles/serials and full hashes of untouched texels guard
reuse. The Metal component-remap view also retains all levels instead of
silently exposing only level zero.

Seven levels pass individual real-shader sampling tests with API validation,
including mixed CPU/GPU producers and same-frame invalidation in either
domain. The test failed with the old single-level view before the view fix.
ASan/UBSan layout and engine suites pass. The earlier race-start replay
completes, with 144 pixels differing by at most four component units; the
reviewed scene remains visually the same. That earlier recording does not
exercise the later B8 feedback. Run 136 validates the full chain in live
gameplay before enabling it in the launcher or claiming a visual benefit.

Evidence: `logs/b8-feedback-validation/`,
`research/rpcs3-surface-format/`, and `logs/profile-132-*` through
`logs/profile-136-*`. No new 30 FPS, 60 FPS or temporal-quality result is
claimed.
