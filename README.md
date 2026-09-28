# PTGDroid

PTGDroid is an unofficial, open-source Android port of **Pathway to Glory** (Nokia N-Gage, 2004).
It runs the original game code on Android without an emulator of the phone: the game's ARM code is
either recompiled ahead of time to C or run by a small ARM interpreter, and the parts of Symbian OS
the game uses are reimplemented natively on top of SDL2.

**No game files are included.** You need your own backup of the game (the files from the N-Gage
game card). This project contains only original code and tools.

## How it works

The N-Gage ran Symbian OS 6.1 (EKA1) on an ARM920T. The game is a Symbian application
(`6r72.app`) plus its data (`data.pak`) and a copy of zlib (`zlib.dll`).

- **E32 loader** (`runtime/runtime.c`) – loads the game's executables from your copy, applies
  relocations and resolves their imports by ordinal against the reimplemented system libraries.
- **Static recompiler** (`recomp/armrecomp.py`) – translates the ARMv4T code of an E32 image into C,
  one C function per guest function (branches become `goto`, calls become C calls, indirect calls go
  through a dispatch table). Used for local builds made from your own copy.
- **Interpreter** (`runtime/interp.c`) – runs the same ARM code directly. The published APK uses it,
  so it contains no code from the game; recompiled builds use it as a fallback.
- **Symbian runtime (HLE)** (`runtime/hle_*.c`, `kernel.c`, `heap.c`) – reimplementations of the
  APIs the game imports: heap, descriptors, active objects and scheduler, timers, threads, traps and
  leaves, files (`RFs`/`RFile`), the application framework (`CEikApplication`, `CAknAppUi`,
  `CCoeControl`), the window server and Direct Screen Access, bitmaps, the MMF audio output stream,
  and "not available" implementations of Bluetooth, sockets and the N-Gage Arena services.
- **Front end** (`runtime/main_sdl.c`, `touch.c`) – SDL2 window, 176×208 EColor4K framebuffer,
  audio, keyboard/gamepad input and on-screen touch controls.

Details that matter for this game: Symbian timers only expire on the 1/64 s system tick (the game
caps itself at 30 fps with a `HomeTime` limiter and 1 ms timer waits), and a newly resumed thread
must run until it first blocks (the sound thread relies on it).

The recompiler and the interpreter are checked against [Unicorn](https://www.unicorn-engine.org/)
with a differential test (`tools/difftest.py`): the same function is run from the same random state
in both and registers, flags and memory are compared.

## Using the prebuilt APK

1. Download `PTGDroid-<version>.apk` from the [Releases](../../releases) page and install it
   (64-bit Android 7.0 or later).
2. Make a ZIP of your game card backup. It must contain `game.id` and the `system` folder with
   `system/apps/6r72/6r72.app`, `data.pak` and `zlib.dll` (a folder around them is fine).
3. Open PTGDroid and select the ZIP. The files are copied into the app's private storage; this is
   only needed once. Saved games and settings are also kept in the app's storage.

### Menu and video filters

A menu is shown before the game starts and whenever you press **Back** on Android (Escape on the
desktop, Guide on a gamepad); opening it pauses the game. It lets you choose:

- **Filter** – how the 176×208 screen is scaled up:
  - *Pixel perfect*: nearest-neighbour scaling.
  - *Sharp*: integer upscale followed by bilinear filtering (crisp pixels without uneven sizes).
  - *Smooth*: Scale2x applied twice, which rounds diagonal edges of the pixel art.
  - *LCD*: a screen-grid effect.
- **Touch controls** – on/off (turn them off when playing with a gamepad or keyboard).

Settings are saved and restored on the next launch.

### Controls

| N-Gage | Touch | Keyboard (desktop) | Gamepad |
|---|---|---|---|
| Joystick | D-pad (8-way) | Arrow keys | D-pad |
| Select (joystick press) | D-pad centre | Enter / Space | A |
| Left / right soft key | top-left / top-right bar | F1 or Q / F2 or W | Start / Back |
| 0-9, `*`, `#` | keypad | 0-9, E (`*`), R (`#`) | X = 5, Y = 7, LB = `*`, RB = `#` |
| C (clear) | `C` | Backspace | B |

Both portrait and landscape layouts are supported.

## Building

Clone with the SDL2 submodule:

```sh
git clone --recursive https://github.com/sherkovpt/PTGDroid.git
```

### Android (interpreter build, like the published APK)

Requirements: Android SDK platform 36, NDK 30.0.16248370, CMake 4.1.2 (all from the Android Studio
SDK Manager) and JDK 17 or newer. Create `android/local.properties` with `sdk.dir=<path to SDK>`,
then:

```sh
cd android
./gradlew assembleRelease
```

The APK is written to `android/app/build/outputs/apk/release/`. Without release signing properties
it is signed with your local debug key.

### Android (recompiled build, from your own copy)

Put your game files in `game/` (so that `game/system/apps/6r72/6r72.app` exists) and generate the C
translation (Python 3.10+):

```sh
python recomp/armrecomp.py game/system/apps/6r72/6r72.app generated/app --module app
python recomp/armrecomp.py game/system/apps/6r72/zlib.dll generated/zlib --module zlib --base 0x18000000 --import-base 372
```

(`372` is the number of imports of `6r72.app` v1.03; `python tools/e32.py <6r72.app>` prints it.)
When `generated/` exists the build links the recompiled code automatically. The generated code is
derived from the game: keep it to yourself and do not publish builds that contain it.

### Desktop (Windows, for development)

```sh
cmake -S . -B build
cmake --build build --config Release
build/Release/ptg.exe game/system/apps/6r72/6r72.app
```

Useful environment variables: `PTG_TOUCH=1` (show touch controls), `PTG_FPS=1`, `PTG_TRACE=1`
(log every system call), `PTG_SCRIPT="ms:key,..."` and `PTG_DUMP=N` (scripted input and frame dumps
for testing).

### Differential test

```sh
pip install unicorn capstone
cmake --build build --config Release --target ptg_difftest
python tools/difftest.py game/system/apps/6r72/6r72.app build/Release/ptg_difftest.exe --seeds 3
```

## Limitations

- Multiplayer (Bluetooth, N-Gage Arena) and voice chat are not available; the game is told the
  services are missing.
- Only Pathway to Glory v1.03 has been tested.
- The interpreter build loads levels more slowly than a recompiled build; gameplay runs at the
  game's own 30 fps cap on current phones.

## Legal

PTGDroid is not affiliated with or endorsed by Nokia or RedLynx. "Pathway to Glory", "N-Gage" and
"Nokia" are trademarks of their respective owners. This repository contains no game code or data;
do not redistribute the game files or code generated from them.

## License

GNU General Public License v3.0 (see `LICENSE`).

Third-party components:
- [SDL2](https://github.com/libsdl-org/SDL) 2.32.10 (zlib license), as a git submodule.
- Symbian 6.1 export lists from [EKA2L1](https://github.com/EKA2L1/EKA2L1) (GPL-3.0),
  in `third_party/eka2l1/`, used to map import ordinals to API names.
