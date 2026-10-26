# MotorStorm PS3 — macOS & iOS native port

Two separate native apps in one Xcode project. MotorStorm Complete Edition is
compiled ahead of time for Apple Silicon and drawn with Metal. This repository
does not contain the game, firmware, saves, or generated game code.

The macOS app and the iOS app do not share an engine build. macOS stays on a
pinned baseline. iOS is built from the current development tree. A change to
one does not move the other.

## Status

**macOS.** Playable. Races run fairly smoothly, up to about 30 FPS. There is
still room to make the game lighter: on a MacBook Pro M5 the fan starts after
a while.

**iOS.** The user reports that the WWS mode-7 build runs well on Gummi (Duo).
That build is preserved as `ios-duo-20261025`, with the same mode now packaged
as the iOS default. Sustained 30 FPS has not been measured on Duo. The last
confirmed driving test on Gummi 2 (iPhone 16 Pro Max) averaged 15.29 FPS, so
performance on that phone remains unresolved.

The local signed app, checksums, source snapshot and Git history are saved in
`output/releases/ios-duo-20261025/`. See `BACKUP.md` for restoration. This private
archive is excluded from Git; the source tag is the GitHub recovery point.

## Game you need

Your own copy of **MotorStorm Complete Edition**, European title ID
**BCES00006**, application version **03.10**. The archive this port was made
from is named:

`Motor_Storm_Complete_Edition_BCES00006 copy.rar`

Extract it and place the game files in `game/` so this path exists:

`game/PS3_GAME/USRDIR/EBOOT.elf`

Do not commit the archive or the extracted game. Expected input hashes are in
`research/input-manifest.json`. No license to the original game is granted.

## Open in Xcode

Open `MotorStorm/MotorStorm.xcodeproj`.

- **MotorStorm**, destination **My Mac**: the macOS app. It signs locally and
  does not need a paid developer account.
- **MotorStorm iOS**, a connected iPhone: the iOS app. In Signing & Capabilities,
  choose your own Apple Development Team and bundle ID. The saved iOS setup
  uses the development identity that installed the tested app; replace it
  for a different account. The repository's macOS bundle ID remains
  `com.motorstorm.port.macos`; existing local macOS signing overrides are
  independent.

The game runtime is compiled as Release even when the Xcode scheme says Debug.
Running from Xcode attaches the debugger; quit the app and open it again from
the device if you want to judge speed.

The current iOS performance experiment enables ThinLTO for generated game code
and the runtime. Its objects and linker cache are isolated under
`build/ios-game/<sdk>/thinlto/`; macOS retains its pinned build. The first build
will take longer. On the **MotorStorm iOS** target, set the user-defined setting
`MOTORSTORM_BUILD_IOS_THINLTO` to `0` to compare with the previous non-LTO build;
remove it or set it to `1` to enable ThinLTO again. Diagnostics record
`build_options.thin_lto`. Device gameplay validation is required before claiming
a performance improvement.

The iOS thread-RTZ arithmetic now handles SPU extended-range/overflow cases
with ARM binary64 vectors. It maps the SPU bit encoding directly (exponent
255 is finite), performs FADD/FMUL/FMLA with the existing round-toward-zero
thread mode, then truncates/saturates back to the SPU format. Ordinary cases
retain the single-precision path. This preserves the integer reference's
results, including zero, underflow, cancellation and extended finite values;
it does not approximate the game's floating-point operations.

The original integer fallback remains available: launch the process with
`MOTORSTORM_SPU_WIDE_RTZ=0` to compare, or `1` to enable it (the default).
This diagnostic switch is read by a C constructor, so set it in the process
launch environment, not the packaged runtime-options JSON. Run
`sh tools/test_spu_thread_rtz.sh` (optionally
`TEST_SANITIZE=address,undefined`) for differential arithmetic and FPCR checks.
`TEST_THREAD_RTZ=1 sh tools/test_audio_dsp.sh logs/audio-dsp-capture-13`
replays locally captured DSP calls with production RTZ arithmetic; the private
fixtures are not distributed. A faster fallback microbenchmark is not proof
of a sustained race FPS improvement. The macOS baseline is unchanged.

iOS dependencies, once per machine:

```sh
python3 tools/build_ios_dependencies.py --sdk iphoneos
```

The iOS PPU-local experiment keeps guest registers in native local variables
inside four sampled functions (`00A2ECAC`, `00A3AD78`, `00D18624`, `00D4CCC4`).
It uses the pinned MIT DOD3 fork's may-dirty analysis, writes all changed
registers at transfers, and reloads every local after calls. It does not use
ABI-based dead-register elimination or change memory/float helpers. Original
bodies remain in the app: set `MOTORSTORM_PPU_LOCALS` to `"0"` in
`port/ios/runtime-options.json`, rebuild and restart for comparison. Build with
`MOTORSTORM_BUILD_IOS_PPU_LOCALS=0` to omit the pass entirely. Session metadata
records `build_options.ppu_locals` and the runtime switch. Generated copies stay
in `build/ios-game/<sdk>/[thinlto/]ppu_locals/`; the original lift and macOS
snapshot are untouched. Run `python3 tools/test_ppu_locals.py --thinlto`
(or `--sanitize`) for production-body differential tests and host copy timings.
Bounded tests with mocked callees are not full gameplay validation; sustained
iPhone FPS and temperature require a matched device run. Upstream provenance
and license are in `tools/third_party/ps3recomp/`.

The iOS audio-filter experiment replaces the prefix of one verified DSP loop
with a native recurrence using the same exact SPU arithmetic. Its final guest
iteration reconstructs every register; unsupported inputs and diagnostic builds
use the original loop. Set `MOTORSTORM_AUDIO_FILTER` to `"0"` in
`port/ios/runtime-options.json` and rebuild to compare the retained original
loop, or build with `MOTORSTORM_BUILD_IOS_AUDIO_FILTER=0` to omit the experiment.
Session metadata records `build_options.audio_filter_native` and the runtime
environment. Run `python3 tools/test_audio_filter.py --capture logs/audio-dsp-capture-13`
to check synthetic edge cases and a local captured DSP call (omit `--capture`
when that private fixture is unavailable). Host loop timings do not establish
an improvement in sustained iPhone FPS or temperature.

The next iOS audio stage selects a complete `0x32770` filter entry with
`MOTORSTORM_AUDIO_BLOCK=1` (the packaged default). Its two recurrence regions
use a buffer/coefficient/history API in `port/spu_audio_block.h`, without a
guest register file inside the DSP kernel. All four history lanes remain
exact. Checked eight-sample windows use ARM single-precision operations;
the bound `|coefficients| <= 2^8`, `|input/history| <= 2^32` limits every
intermediate below `2^120`. Other values use native binary64 vectors, with
SPU precision, extended range, saturation and flushing after every operation.
Neither path reassociates the recurrence or changes SPU round-toward-zero.

The adapter preserves both DMA boundaries, including publication of history
between the two segments. The final eight samples of each segment still run
through the retained guest body once to reconstruct every scratch register.
Unsupported layouts, overlapping buffers and diagnostic builds retain guest
execution. This replaces one filter unit, not the entire mixer/decoder or
audio engine. Set `MOTORSTORM_AUDIO_BLOCK=0` at launch to restore the previous
filter experiment; additionally set `MOTORSTORM_AUDIO_FILTER=0` for the original
lift. Both switches are recorded in session metadata. Generated copies remain
inside the iOS build; the macOS baseline is unchanged.

Run `python3 tools/test_audio_block.py --thinlto --capture logs/audio-dsp-capture-13`
for independent integer-arithmetic checks, complete-entry/channel comparisons
and the private captured call. Omit `--capture` when the local fixture is
unavailable; use `--sanitize` for ASan/UBSan. Host kernel speedups do not
establish sustained race FPS or reduced device temperature.

The broader iOS SPU register experiment uses `MOTORSTORM_SPU_PIPELINE=2`
to keep named native registers across 90 fused regions in the two measured
WWS modules. Every potentially changed register is published at exits and
context callbacks; observers retain the original route. It changes neither
arithmetic nor the guest memory model. `0` selects the retained baseline.
The corresponding audio variant was slower on captured inputs, including
with Release/ThinLTO flags, and is deliberately absent from production
generation. It remains reproducible in `tools/test_audio_worker.sh`.

`MOTORSTORM_COUNTING_WAIT=1` enables a separate iOS scheduling experiment at
the measured physics counting semaphore. The original wait, CAS, backoff and
post operations remain; an Apple atomic compare-and-wait follows an exhausted
backoff, and a successful zero-to-one post wakes waiters. A 250 microsecond
timeout covers other writers and changes to the counter pointer. This frees
CPU while waiting; it is not a substitute for reducing actual SPU work.
The packaged iOS development build selects `MOTORSTORM_SPU_PIPELINE=2` and
`MOTORSTORM_COUNTING_WAIT=1`; both are recorded in session metadata. Set either
to `0` for the retained baseline (the helpers also default to off when no
setting is supplied). These remain experimental development defaults: the
first user-driven runs on Gummi 2 averaged 15.8 FPS with the new SPU path and
11.8 FPS with the old path, with the new wait enabled in both. Both runs
reported critical thermal state and did not replay identical driving, so
this is not a controlled 34% speedup claim or a successful 30 FPS validation.

Validate with `python3 tools/test_spu_pipeline.py`,
`python3 tools/test_job_merge.py --pipeline` (optionally `--sanitize`) and
`python3 tools/test_counting_wait.py`. The WWS replay checks every one of
1,293 entry points against the original dispatch, including full context,
local store, exit target, interrupts and four rounding modes. These are
correctness tests; only an actual matched race establishes an FPS benefit.
The local checkpoint `build/ios-spu-pipeline-20261025-01/` preserves the prior
sources. Its `restore-baseline.py` verifies later edits before restoring with
`--apply`; neither switch changes the macOS baseline. The older signed apps
are now compressed under `output/releases/ios-duo-20261025/historical-apps/`;
`cleanup-manifest.json` maps each original app path to its verified archive.

The iOS whole-batch WWS experiment is selected by the packaged default
`MOTORSTORM_WWS_CHAIN=7`. Launch with `MOTORSTORM_WWS_CHAIN=0` to retain the
previous route; process environment settings override the packaged default.
Bit 1 replaces image 205's complete object-classification phase
(`88D8` through the retained `8BE0` epilogue): world transforms, size/LOD tests,
frustum and occluder tests, ordered output lists and counters. This is the
first semantic stage of the broader WWS port. Bit 2 replaces the complete
occluder-index sort (`DCBC` through `F010`, 212 original basic blocks).
`1` selects classification only, `2` sorting only, and `3` both. Bit 4 extends
the native sort through occluder selection, ordered lists and the retained
`F530` epilogue (236 original blocks in total); `7` selects all three stages.
These stages do not replace the job manager, DMA protocol, the earlier
occluder-plane construction, image 209 physics or audio engine.
The adapter is generated only for iOS and leaves all previous bodies intact.

The native algorithm delays oriented-box transforms until sphere tests need
them, and evaluates eight corners in two ARM SIMD groups. SPU rounding,
extended finite values, estimate/interpolation rules and list order remain
unchanged. A checksum-bound caller analysis follows all five callers and
rejects use of changed volatile scratch values. The original epilogue restores
the nonvolatile registers; private callee stack scratch and diagnostic host
transfer counts are deliberately not reproduced. Pending interrupts, custom
instrumentation, unsupported layouts and batches that may reach a DMA flush
retain the previous route before any native write. No DMA, channel or job
completion operation is removed.

The sort uses native records with cached integer keys that preserve SPU
extended-float comparisons, including signed zero and flushed subnormals.
Its median selection, equal-key partition and insertion order match the
original algorithm. Tie order matters because subsequent occlusion work
consumes a bounded prefix. The algorithm is adapted from the official
[Microsoft STL implementation](https://github.com/microsoft/STL/blob/main/stl/inc/algorithm);
its license is retained in `port/wws_sort.LICENSE.txt`. Unsupported indices,
contexts or a depleted introsort depth budget fall back before any write.
A second checksum-bound continuation audit verifies the skipped scratch
register values are dead; r87 is explicitly restored. Run
`python3 tools/test_wws_sort.py --capture <directory> --output <build-directory>`
with the same sanitizer/production-baseline options as below. Sort-entry
captures and shuffled data derived from other captures are reported separately.

Occluder selection tests four independent spheres per NEON vector using the
exact SPU arithmetic helpers. The original pairwise plane loop at
`F044..F2E8` has no memory/channel writes and no live register result; the
generator checks that property and its continuation before omitting it.
This eliminates unobserved calculations without changing occlusion decisions.
Use `--whole-phase` with `tools/test_wws_sort.py` to compare all visible memory
and live registers through the retained return, including both lists and their
full counter quadwords. Actual phone fixtures, 11,600 sorting cases, 512
geometry edge cases and unsupported-context/layout tests pass with the original
source, the previous production pipeline, ThinLTO and ASan/UBSan. The combined
sort/selection host replay is about 8.3 times faster than the previous
production pipeline; this is not a race-FPS measurement.

For private, local replay fixtures, launch with
`MOTORSTORM_WWS_CAPTURE=logs/wws-capture-02`, then copy that Documents directory
from the app. Captures are bounded and disabled during performance runs.
Run `python3 tools/test_wws_chain.py --capture <directory> --output <build-directory>`;
add `--sanitize` for ASan/UBSan. To compare with the currently generated iOS
pipeline, add `--production-baseline
build/ios-game/iphoneos/thinlto/native_geometry/jobmod_be896a6374ba_e12230.c`.
Replay compares all externally visible local-store bytes and live registers
through the return, plus unsupported-context guards and synthetic edge cases.
Host kernel timing is not evidence of sustained race FPS. Classification alone
measured 15.42 FPS during 117 seconds of user-confirmed driving on Gummi 2
(iPhone 16 Pro Max); it did not solve the race-performance problem. That run
is not a matched A/B comparison. The later mode-7 build
(`A7FAB6EE-B551-3625-8F1E-16C3584EB106`) measured 15.29 FPS across 113 seconds
of confirmed driving, with one-second windows from 10.95 to 17.97 FPS, all
below 26 FPS. Thermal state was serious throughout both runs. The different
routes and preceding thermal histories prevent attributing the difference to
the code change. The 30 FPS target remains unmet; the experiment is not
enabled by default. A separate parked CPU trace places audio and image 209
among the remaining large costs. The attempted driving CPU trace failed to
attach, although devicectl confirmed the same live app PID and uninterrupted
session; do not present the parked profile as a driving profile.

The local checkpoint `build/ios-wws-native-chain-20261025-01/` contains the
pre-experiment sources and hashes. Signed apps are in the historical archive
described above; the exact Duo-tested app also has its own `approved-app.tar.gz`.
The checkpoint's `restore-baseline.py` first
verifies every affected path; `--apply` restores only this experiment and
refuses to overwrite subsequent edits. The launch switch restores the old
route without rebuilding or reinstalling. The macOS baseline is unchanged.

The first native iOS geometry kernels implement two complete box/plane
predicates in WWS image 205 (`8120`, `8380`). They test eight corners in two
NEON groups, then reconstruct the final guest scratch state once. The original
rounding/order, every GPR, local-store contents, return target and dispatch-step
count are preserved. A checked magnitude range excludes underflow/extended
arithmetic from the batch kernel; unsupported values, execution modes and
diagnostic observers use the retained original pipeline. This is the first
geometry component, not a replacement for the complete collision engine.
WWS executes this module as resident overlay 205 while retaining policy image
900; the native guard checks that resident code owner, not just the base image.
Tests cover this dispatch state, direct-image calls and unrelated overlays.

Set `MOTORSTORM_NATIVE_GEOMETRY` to `"0"` in `port/ios/runtime-options.json` and
rebuild/restart to select the old implementation. Build with
`MOTORSTORM_BUILD_IOS_NATIVE_GEOMETRY=0` to omit it entirely. Generated sources
stay in the iOS SDK's `native_geometry/` build directory. Session metadata
records both the build option and actual runtime switch. Run
`python3 tools/test_native_geometry.py --thinlto --production-baseline`
or `--sanitize` for full-call comparisons and bounded-kernel tests. Host
component speedups do not establish sustained phone FPS or energy savings.
Optional `MOTORSTORM_GEOMETRY_CAPTURE=logs/geometry-inputs` saves at most 32
compact, pointer-free inputs per kernel for that process (first four, then
every 1024th call); it is off in normal launches. Replay them with
`--capture <directory>`. Treat capture runs separately from normal performance
measurements. These private game fixtures are not committed or exported with
the ordinary diagnostics ZIP.

On iOS, opening the app starts MotorStorm automatically, with a plain black
background until the game's first frame. There is no added startup logo, title,
setup buttons, FPS overlay or artificial delay. Touch controls appear when the
first drawable is actually presented. File validation
runs off the UI thread; existing local folders and saved folder bookmarks still
work. If game files are missing, an error dialog offers folder selection or a
retry. There is no file picker or confirmation on a normal successful launch.

The iOS Duo fill experiment stretches the final game image to cover landscape
displays taller than 16:9. It changes only the game view's frame and explicitly
uses resize gravity; the Metal drawable and game render targets keep their
guest resolution. Touch controls remain a separate, unscaled view.
Conventional 16:9 or wider screens keep the previous aspect fit. Launch
with `MOTORSTORM_IOS_FILL_SCREEN=0` to restore aspect fit without rebuilding;
the default is enabled. This intentionally changes the game image's proportions
and does not change the camera's field of view or add rendered detail.

The iOS driving controls sit just below screen center: larger left/right
buttons on the left, with Boost above them, and brake/throttle side by side on
the right. Their size adapts to the safe area (up to 104 points instead of 64).
The handbrake touch button is removed; the top Back button keeps its existing
input mapping but no longer displays a pause symbol.
The controls use a subtle static translucent gradient, thin borders and SF
Symbol arrows, with a warm yellow pressed state. Their geometry is unchanged;
the style adds no backdrop blur, display timer or per-frame CPU work.
Horizontal positions follow the screen edges with a small margin, so the Duo
cover display's asymmetric safe-area inset does not push one group inward when
rotated. Button sizes and vertical placement still use the same layout rules;
the inner display's layout is unchanged.

### iOS performance diagnostics

Normal and connected-device launches both start automatically; the old
`--start-game` argument is no longer required. The helper
`tools/ios_device_control.py --device <UDID> --out logs/<run> launch` enables the
existing `PAD_FILE` controller in the app's Documents directory. Use the same
device/output options with `pad 0x4000` to confirm in menus,
`pad 0x0008 --reports 40` for Start/pause during slow gameplay,
`pad 0x0200 --reports 60 --axes 128 128 128 128` for a bounded throttle input,
`screenshot` to inspect the phone, or `collect` to copy its performance logs.
Short four-report presses may be missed in slow gameplay. Report counts are
consumed input reports, not seconds. Acceptance is checked
by reading back the emptied command file; it does not mean the hold has ended.
Commands are sequential, and a new command waits for the previous hold to end.
The helper records device identity and host timestamps. Screenshots and file
transfers can perturb a run: keep them outside accepted timing windows and
verify real driving rather than treating a racing HUD or a stationary car as
evidence of representative race performance. These controls do not change the
macOS app or turn a phone run into a deterministic replay.

The iOS topology reuse experiment caches the complete translation of adjacent
index packets: decoded vertex references, duplicate remapping and the final
triangle index list. Every input byte and all topology controls must match;
hash equality alone never permits reuse. Vertex attributes are still fetched
at the original point for every draw. Misses use an owned input snapshot and
the existing translator; unsupported, mixed or noncontiguous streams retain
the original path. Cached payloads are bounded to 32 MiB, plus entry metadata
and at most 256 KiB of scratch input. It does not cache animated vertex data,
reduce geometry, or change shaders.

Set `RSX_VERTEX_TOPOLOGY_CACHE` to `"0"` in `port/ios/runtime-options.json`
and rebuild/restart to restore the old path. Only the iOS profile enables it.
Session metadata records the switch; native logs report cumulative hits,
misses, bypasses and references skipped every 450 presented guest frames.
Run `sh tools/test_topology_cache.sh` for complete draw-output differential
tests, changed indices/attributes, restart cuts, hash collisions and bounded
fallbacks. Host test timings do not establish sustained iPhone FPS. This
targets the repeated CPU work identified in driving measurements, following
Apple's [CPU-side Metal performance guidance](https://developer.apple.com/documentation/xcode/analyzing-the-performance-of-your-metal-app).

The iOS direct vertex snapshot experiment writes validated raw geometry into
CPU/GPU shared Metal buffers at the original vertex-fetch point. It removes
the second snapshot-to-staging copy without changing vertex arithmetic or
reading live guest memory on the GPU. A separate pool preserves snapshots
across texture/prepare-draw submissions; pending draws and the latest GPU
completion fence prevent premature reuse. The pool has eight slots, initially
4 MiB each and at most 16 MiB each, allocated on demand. Exhaustion uses the
retained heap snapshot path without an extra wait.

Set `RSX_METAL_DIRECT_VERTEX_SNAPSHOT` to `"0"` in
`port/ios/runtime-options.json` and rebuild/restart for the original path.
Only the iOS profile enables it. Session metadata records the switch; native
diagnostic builds report cumulative direct/heap draws, avoided copy bytes, pool
capacity and exhaustion every 15 seconds. Run `sh tools/test_vertex_packed.sh` and
`sh tools/test_metal_vertex_snapshot.sh` for CPU/GPU equivalence, delayed GPU
consumers, submission boundaries and fallback tests. This follows Apple's
[shared-resource guidance](https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/MTLBestPracticesGuide/ResourceOptions.html)
and [resource lifetime/copy guidance](https://developer.apple.com/videos/play/tech-talks/10580/).
Removing the copy is verified by these tests; sustained iPhone FPS and energy
savings require matched gameplay measurements.

Normal iOS builds compile out the offline recorder, ZIP exporter, FPS tracking,
passive CPU/GPU/audio/wait counters and thread-clock registry. They create no
performance sessions or native log files. WWS capture/statistics and periodic
Metal snapshot/cache logging are also omitted from normal builds. Functional
game counters, synchronization, error handling and shader caches remain intact.
Dormant SDK debugging hooks still exist; their presence is not proof that they
run, and this cleanup is not a measured FPS improvement.

For an explicit recording build, set the iOS target's
`MOTORSTORM_BUILD_IOS_DIAGNOSTICS=1`, rebuild and launch without a debugger.
This restores offline recording and underlying FPS samples, without a share
button or an on-screen FPS label. Copy recorded sessions with
`tools/ios_device_control.py --device <UDID> --out logs/<run> collect`.
Existing diagnostics ZIPs can still be imported with `tools/collect_ios_diagnostics.py`.
The normal app cannot enable this recording through a launch argument.

Run `sh tools/test_ios_play_build.sh` to verify compile-time removal and
`sh tools/test_offline_diagnostics.sh` for the opt-in recorder/exporter. WWS
replays accept `--play-build` to verify the kernels without capture/statistics.
The iOS build records `build_options.offline_diagnostics` in its launch resource.

Coarse recording remains separate from expensive per-draw/SPU probes. Add
`MOTORSTORM_BUILD_IOS_DETAILED_DIAGNOSTICS=1` only for a specific investigation.
Recorded aggregate samples include menus and pauses; select actual driving
before comparing FPS. Logger CPU time excludes instrumentation on other threads.

For renderer-only diagnosis, add
`MOTORSTORM_BUILD_IOS_RENDER_DIAGNOSTICS=1` to the recording build. This enables the existing
renderer wall-time counters and draw/byte counts in every offline sample,
without enabling SPU local-store observers or job-chain sampling. Metadata
records `render_phase_diagnostics`. Leave the option unset for normal builds.
These phase intervals overlap (parent and child phases); do not add them
together or treat wall time as thread CPU time. Compare driving windows,
excluding loading, stationary cars and race results, and measure final FPS
again with the normal build after identifying a fix.

Normal iOS builds also compile out the legacy per-access SPU local-store
watch/low-read/SMC/WWS probes. Address masking, byte order and game calculations
are unchanged. Detailed diagnostics restores those probes, or use
`MOTORSTORM_BUILD_IOS_SPU_LS_DIAGNOSTICS=1` to restore only them for comparison.
The launch/session metadata records `spu_ls_diagnostics`. This removes known
debug work; its effect on race FPS still needs a device comparison.

## Restore the source

Requirements: Xcode, CMake 3.24 or newer, Ninja, and Python 3. The macOS build
also needs Homebrew SDL2, glslang, SPIRV-Cross, and FFmpeg.

```sh
python3 tools/bootstrap_runtime.py
sh tools/analyze_game.sh
```

Then build from Xcode. `BACKUP.md` lists what this repository keeps and what
stays on your machine.
