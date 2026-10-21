# Source-only backup

This repository preserves the authored MotorStorm port work. It does not
contain the game, firmware, generated game source, installed game data, saves,
executables, shader caches, recordings or GPU captures. Those remain local.
Git LFS is not required.

## What is preserved

- `port/`: authored runner, platform adapters, native optimization helpers,
  CMake build configuration and tests.
- `MotorStorm/`: the Xcode project, macOS app entry point and app icons. The
  macOS target links the game runtime; iOS still has its SwiftUI starter.
- `tools/` and `tests/`: source generation, launch, capture, analysis and
  verification tools.
- `patches/ps3recomp-local.patch`: tracked changes **and new source files** from
  our modified runtime/recompiler, against the exact commit in
  `research/ps3recomp-commit.txt`.
- `research/`: input hashes, pinned upstream versions, small authored control
  plans and the original resolution experiment.
- `README.md`: research history and measured limitations. References to
  `logs/` refer to local evidence that is deliberately not uploaded.

The main upstream is [ps3recomp](https://github.com/sp00nznet/ps3recomp).
Its MIT license is preserved in `port/UPSTREAM-LICENSE`; notices for bcdec and
xxHash are included with their code in the runtime patch.
[MSMV-unpacker](https://github.com/Scirvir/MSMV-unpacker) was an optional archive
reference; its pinned commit is recorded, but it is not required by the build.
No license for the original game or its assets is granted by this repository.

## Restore on another Mac

Install the prerequisites listed in `README.md`, then:

```sh
python3 tools/bootstrap_runtime.py
```

This fetches the pinned open-source runtime and applies the saved patch. It
refuses to overwrite a checkout with unrelated local changes. To verify an
existing checkout without downloading or changing files:

```sh
python3 tools/bootstrap_runtime.py --check
```

Supply your own matching local game files under `game/`. The input used here
is BCES00006 version 03.10; hashes are in `research/input-manifest.json`.
Then regenerate the game-derived sources locally and build:

```sh
sh tools/analyze_game.sh
sh tools/build_port.sh
sh tools/play-experimental.command
```

Generation also restores the manually authored job-wait wrapper with
`tools/patch_job_wait.py`. That tool stores the wrapper and an expected
fingerprint, not the original game function. It refuses to patch an unexpected
function. The SPU overlays and policy are also generated from the local game.
`tools/build_port.sh` additionally generates the three checked Multistream DSP
overlays with `tools/lift_motorstorm_audio.py`. The generator and authored NEON
helpers are preserved; generated DSP code, its byte signatures and audio replay
captures are excluded. Replay tests require the matching local captures under
`logs/`, while the conversion and runtime handoff tests use synthetic inputs.
The build also invokes `tools/specialize_audio_worker.py` to regenerate four
checked Multistream loop replacements in ignored `build/`. The authored
generator, `port/spu_audio_worker.h` and differential replay harness are backed
up; the specialized game source and its input snapshots are not.
Existing installations and saved state under `gamedata/` are not backed up.

The runtime patch also preserves the compact Metal B8 initialization path and
the optional fast SPU dispatch checks, with synthetic GPU/dispatcher regression
tests. `tools/test_surface_gpu_seed.sh`, `tools/test_dispatch_checks.sh` and
`tools/profile_manual_gameplay.py` are authored source. Their local gameplay
profiles, executable snapshots and Instruments traces under `logs/` are excluded.

All 91 changed/new runtime files were restored byte-for-byte in a temporary
directory from upstream source plus the patch. The wait wrapper was also
reconstructed byte-for-byte without modifying the live generated source.
A complete fresh regeneration/build/gameplay run from this backup has not
been performed. The historical `tools/repair_havok_switch.py` is retained as
an integration repair; the generic discovery fix is already in the runtime
patch. After restoring the local prerequisites, the Xcode **MotorStorm** scheme
builds the shared CMake/Ninja archive and runs the macOS game directly. The
generated link response and launch resource are local build output, not backup
content. `port/macos-launch.json` preserves the shared launch defaults. Use
Xcode's normal Derived Data location outside iCloud Drive for app products.

The macOS target now builds an independent snapshot of the revision recorded
in `port/macos-baseline.json`, via `tools/build_macos_runtime.py`. Keep that
commit in Git history: its runtime patch and generators reconstruct the macOS
version even while the working tree's sources and patch evolve for iOS.
The generated snapshot and compiled files in `build/macos-stable/` are local
only. The game assets are referenced, not copied or uploaded. The iOS target
continues building the working tree; changing iOS source cannot change the
pinned macOS source. Do not edit the generated macOS snapshot in place.

## Updating the backup

Before committing further runtime changes:

```sh
python3 tools/export_runtime_patch.py
python3 tools/bootstrap_runtime.py --check
git status --short
git diff --stat
```

Inspect the staged paths and sizes before pushing. Keep generated game code,
game data, binaries and captures excluded even if Git offers to force-add them.
Only the latest runtime patch is versioned; Git history preserves future
versions without duplicate patch files.
