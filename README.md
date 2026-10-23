# BotW Recompiled: Android / Linux ARM64 (unofficial)

An unofficial **Android / Linux ARM64 port** of [BotWRecomp](https://github.com/IshaanKalra2103/BotWRecomp)
by Ishaan Kalra, which statically recompiles the PowerPC code of **The Legend of Zelda: Breath of the
Wild (Wii U)** to C++ and runs it on a Cafe OS runtime written for the project.

The original project targets iPhone and Apple Silicon Macs with Cemu's Latte GPU layer drawing through
Metal. This fork replaces that renderer with Cemu's **Vulkan** backend and builds on Linux ARM64, so
it runs on Android phones with a Vulkan driver (tested on Turnip / Adreno).

> [!IMPORTANT]
> **This repository contains no game files, no game code, no ROMs, no keys and no copyrighted or
> protected material of any kind.** It is only a tool. You need your own legally obtained copy of the
> Wii U game, dumped from your own console. The build recompiles *your* copy on *your* machine.
> There is no download-and-play package, and there never will be one. How you obtain and use the game
> files is your own responsibility. Do not ask for game files, keys or dumps in issues or discussions.

> [!WARNING]
> **This project is in an early stage and under active development.** Expect bugs, crashes, low
> frame rates and breaking changes. It is not ready for general use, and there is no APK or
> installer yet.

## Status

Early and experimental. Everything below was tested on a single device:

- **Device:** POCO F5 (Snapdragon 7+ Gen 2, Adreno 725)
- **Environment:** Debian ARM64 inside a [DroidSpaces](https://github.com/ShubhamPatilsd/droidspaces) container on Android
- **GPU driver:** Mesa Turnip (Vulkan 1.4)

What works:

- Full Linux ARM64 build (CMake + clang).
- Vulkan renderer (Cemu's Latte Vulkan backend, vendored from Cemu commit `a53cf0e`) running on Turnip,
  from the title screen to the open world.
- **H.264 video playback** (cutscenes, tower videos) through a port of Cemu's `ih264d` decoder.
  Without it the game crashes on video scenes.
- Workaround for a Turnip crash caused by the `fmulz`/`fmadz` shader pattern.
- Optional **PGO** (profile-guided optimization) build, which gave a clear speedup on the generated guest code.
- `botw-run.sh`: launcher that locks the GPU clock and pins threads to the big cores.
- An experimental second code generator, `tools/codegen_v2.py`, that emits tighter guest code
  (work in progress).

Performance (honest numbers, early work):

| Setup | Open world |
| --- | --- |
| Baseline build | about 12-16 fps |
| GPU clock locked at 591 MHz + PGO | about 20-30 fps, dropping below 20 in heavy scenes |

Profiling shows roughly 75% of CPU time in the recompiled guest code and about 14% in the
renderer/driver, so current work focuses on the generated code, not the GPU.

Known gaps: performance is not yet playable for most people, shaders compile on first use (hitches),
only one device and one driver have been tested, DLC is untested, and there is no APK yet.

## Roadmap

1. Finish performance work on the Linux/Debian build (codegen, PGO, compiler flags).
2. Package everything as a standalone Android **APK**, which will live in a **separate repository**.
3. Controller and touch controls on Android.

## What you need

- **An Android phone with a Vulkan 1.1+ GPU.** Only the POCO F5 (Adreno 725, Turnip) has been tested.
  A Linux ARM64 environment is required for now (for example Debian in DroidSpaces, or Termux proot
  with GPU access).
- **A PC or the phone itself** to run the recompiler (a few GB of RAM and some patience).
- **A legal dump of the game, as a folder, in version v208.** Breath of the Wild for Wii U,
  **USA, update 1.6.0 (v208)**, dumped from your own console, as a folder containing `code/`,
  `content/` and `meta/` (the update's files copied over the base game's). Other regions and other
  versions are **not** supported: the recompiler and the patches are made for that exact executable.
  Check yours:

  ```bash
  sha256sum /path/to/game/code/U-King.rpx
  # ba58da5b95ce929e005d058ceb08b9b2788d1ab2bbc8a6c189bbadca0bb34d30
  ```

  Dump the game and its update from your own Wii U (for example with
  [Dumpling](https://github.com/emiyl/dumpling)). Nothing else is provided or supported here.

## Build (Linux ARM64)

The commands are the same as upstream's desktop build; this fork only changes specific files.

```bash
# Dependencies (Debian/Ubuntu)
sudo apt install build-essential clang cmake ninja-build git python3 \
    libvulkan-dev libsdl3-dev libfmt-dev libssl-dev libzstd-dev libboost-dev libglm-dev
# Python tooling: uv (https://docs.astral.sh/uv/)

git clone https://github.com/DenisGames54/BotWRecomp-Android
cd BotWRecomp-Android
git checkout android-vulkan

# 1. Recompile your copy of the game to C++ (writes generated/)
uv run python -I tools/recompile.py /path/to/game/code/U-King.rpx generated

# 2. Build
cmake -B build -G Ninja && ninja -C build

# 3. Run
BOTW_GAME=/path/to/game tools/play.sh
```

The scripts look for the game in a folder named `game` next to the repository; if yours is elsewhere,
set `BOTW_GAME=/path/to/game`.

## Credits and licenses

This is a fork of [IshaanKalra2103/BotWRecomp](https://github.com/IshaanKalra2103/BotWRecomp).
All the recompiler and runtime work is by **Ishaan Kalra**. This fork adds the Linux ARM64 / Android
work (Vulkan renderer, H.264 decoder, Turnip fixes, PGO, launcher scripts) by
[DenisGames54](https://github.com/DenisGames54).

- [Cemu](https://github.com/cemu-project/Cemu): the Latte GPU layer (including the Vulkan renderer)
  and the H.264 decoder (`ih264d`) come from Cemu, plus parts of GX2, snd_core and the motion code.
  Each such file says so at the top.
- Cemu's graphic packs (CC0) and the FPS++ pack authors for the 60 fps approach, as credited upstream.
- [SDL3](https://libsdl.org), [fmt](https://fmt.dev), [zstd](https://facebook.github.io/zstd/),
  [Dear ImGui](https://github.com/ocornut/imgui), [Mesa / Turnip](https://www.mesa3d.org).

Licenses are unchanged from upstream: the project's own code is MIT ([LICENSE](LICENSE)). Code from
Cemu stays under the Mozilla Public License 2.0 ([runtime/cemu/LICENSE.txt](runtime/cemu/LICENSE.txt)).
Third-party components keep their own licenses.

*The Legend of Zelda* and *Breath of the Wild* are trademarks of Nintendo. This is an unofficial fan
project, not affiliated with or endorsed by Nintendo. This repository is provided as a technical tool
only, with no warranty, and the user is solely responsible for how it is used.
