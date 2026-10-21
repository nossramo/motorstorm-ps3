# MotorStorm: keep macOS and iOS development separate

These instructions apply throughout this repository. Read the platform/build
sections of `README.md` before changing code or Xcode settings.

## Platform boundary

- An iOS task does not authorize changes to macOS behavior or build settings.
  A macOS task must likewise preserve unrelated iOS work.
- macOS uses the commit in `port/macos-baseline.json`, restored by
  `tools/build_macos_runtime.py` into `build/macos-stable/source`, with its own
  runtime, generators, generated PPU/SPU sources, libraries and object files.
- Treat that generated macOS snapshot as immutable. Do not edit it in place,
  bypass its checksum guard, or point its build at the live development tree.
- iOS uses the working-tree `port/`, `vendor/ps3recomp`, generated sources and
  `patches/ps3recomp-local.patch`, built by `tools/build_xcode_runtime.py` into
  `build/ios-game/<sdk>`. These are not the macOS engine sources.
- Do not share generated sources, build caches or link-response files between
  platforms. Platform-specific launch defaults must remain independent.
- The shared Xcode project contains both targets. Edit only the intended
  target's settings, build phases and scheme; avoid project-wide changes that
  affect the other target. Apply the same care to shared Python/shell helpers.
- Do not automatically advance the macOS baseline to HEAD or import iOS
  optimizations into it. Promoting a new engine baseline requires a task that
  explicitly includes macOS, separate review and macOS gameplay validation.
- For authorized macOS app/build fixes, use the macOS app entry point or the
  dedicated builder/options layer. `tools/macos_runtime_options.cmake` carries
  the runtime-C-only `-fno-common` alignment fix; iOS must not load this include.
- Preserve the other platform's uncommitted work. Do not reset, overwrite or
  discard it when restoring or fixing one platform.

## Validation when changing the boundary or macOS build

- Run `python3 tools/build_macos_runtime.py --check` to verify the baseline.
- After building macOS, run `python3 tools/test_macos_isolation.py --compiled`
  to check the actual compile/link dependencies and the alignment-fix scope.
- Build the affected Xcode target. A successful build/startup is not proof of
  correct race performance; test gameplay when engine/gameplay behavior changes
  and report precisely what was verified.
