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

**iOS.** Playable, up to about 20 FPS. The iOS code needs more speed work, and
the phone should not be pushed so hard that it overheats.

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
  choose your own Apple Development Team. The bundle IDs in the project are
  placeholders, `com.motorstorm.port.macos` and `com.motorstorm.port.ios`.
  Change them if they collide with another app on your account.

The game runtime is compiled as Release even when the Xcode scheme says Debug.
Running from Xcode attaches the debugger; quit the app and open it again from
the device if you want to judge speed.

iOS dependencies, once per machine:

```sh
python3 tools/build_ios_dependencies.py --sdk iphoneos
```

On iOS, copy the `game` folder into the app's Documents directory, or use
**Choose game folder**, then press **Start game**.

## Restore the source

Requirements: Xcode, CMake 3.24 or newer, Ninja, and Python 3. The macOS build
also needs Homebrew SDL2, glslang, SPIRV-Cross, and FFmpeg.

```sh
python3 tools/bootstrap_runtime.py
sh tools/analyze_game.sh
```

Then build from Xcode. `BACKUP.md` lists what this repository keeps and what
stays on your machine.
