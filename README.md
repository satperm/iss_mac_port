# ISS — macOS port (Apple Silicon)

Play your own copy of the PS4 game ISS (CUSA00223 / CUSA00004, update 1.07) on an Apple Silicon
Mac. This repository contains **no game code or data**: it loads the player's own decrypted copy
and runs it with a PS4 runtime (`port/src`) and a Vulkan renderer (`port/gpu`, derived from
shadPS4) on Metal through MoltenVK. The game's x86-64 code runs under Rosetta 2.

It is a macOS fork of [iss_pc](https://github.com/HayatoG/iss_pc), the native PC port.

> **Status: first test version, work in progress.** The story can be played from the start with
> GPU particles (smoke, rain, sparks). Tested on one Mac (M3 Max, 36 GB) as far as the first missions
> in Seattle. Translucent smoke still flickers. On that Mac expect about 50-60 FPS in calm outdoor
> scenes and 25-45 FPS in heavy ones. See
> [Expected performance](#expected-performance) and [Known issues](#known-issues).

## Contents

- [Requirements](#requirements)
- [Download](#download)
- [Getting the game files](#getting-the-game-files)
- [Build](#build)
- [Play](#play)
- [Settings and shortcuts](#settings-and-shortcuts)
- [Expected performance](#expected-performance)
- [Status](#status)
- [Known issues](#known-issues)
- [Roadmap](#roadmap)
- [Troubleshooting and logs](#troubleshooting-and-logs)
- [For developers](#for-developers)
- [Credits](#credits) and [Legal](#legal)

## Requirements

- A Mac with Apple Silicon (M1 or later). Developed and measured on an M3 Max MacBook Pro.
- 16 GB of memory or more (the game uses about 6-9 GB).
- macOS 15 or later.
- Rosetta 2: `softwareupdate --install-rosetta --agree-to-license`.
- Xcode (from the App Store). The command line tools alone are not enough: the build compiles a
  patched MoltenVK.
- [Homebrew](https://brew.sh) (the default arm64 one).
- About 10 GB of free disk space for dependencies and builds, plus the game itself.

To build from source, which takes about 30-60 minutes the first time, you also need Xcode and
Homebrew. A downloaded app only needs Rosetta 2 and your game files: see [Download](#download).

## Download

If a release is available on the [Releases](https://github.com/satperm/iss_mac_port/releases) page:

1. Download `ISS-macOS-<version>.zip`, unzip it and move `ISS.app` to Applications.
2. The app is not notarised, so macOS blocks a downloaded copy. Clear the download flag once:
   `xattr -dr com.apple.quarantine /Applications/ISS.app`
3. Open it and continue at [Play](#play).

Settings, saves and caches are in `~/Library/Application Support/ISS`.

## Getting the game files

You need your own, legally obtained copy of the game, **decrypted**, as a folder: base game 1.00
merged with update 1.07. The folder looks like this:

```
CUSA00223/
  eboot.bin
  sce_sys/       (param.sfo, icon0.png, ...)
  sce_module/
  ...game data...
```

No game files, keys or dumps are provided here, and none are accepted in issues.

## Build

```sh
git clone https://github.com/satperm/iss_mac_port.git
cd iss_mac_port

# 1. Dependencies (once): x86-64 libraries and the patched MoltenVK, into port/deps
bash port/scripts/macos/bootstrap_deps.sh

# 2. Python environment for the run script (once)
python3 -m venv port/.venv && port/.venv/bin/pip install iced-x86

# 3. Loader, runtime and renderer (again after every update of the repository)
cd port && bash build_macos.sh && cd ..

# 4. The app: ISS.app in the repository folder (again after moving the folder)
bash port/scripts/macos/make_app.sh
```

`build_macos.sh --test` also runs the unit tests. Use the plain build for playing: the quick
`ISS_LTO=OFF` developer build is about 25% slower.

## Play

Open **ISS.app** (in the repository folder; you can drag it to the Dock). The first time:

1. Under **Game folder**, click **Choose…** and select your game folder.
2. Pick the **Text** and **Voices** languages, then **Play**.

The first start prepares the shaders once: about a minute with no window while it lists them from
your game files, then up to about two minutes on a "Compiling shaders" screen (measured on an M3 Max).
It needs about 900 MB of disk space and runs again only when the game files or the port change. Later
starts skip it. To skip it entirely, set `TLG_PRECOMPILE=0`; shaders then compile during play and stutter.

Without the app, from a Terminal:

```sh
cd port && TLG_GAME_DIR=/path/to/CUSA00223 bash run_macos.sh
```

### Saves

Saves live in `port/user/savedata` (in a downloaded app: `~/Library/Application Support/ISS/user/savedata`). The launcher's **Saves…** window backs them up (**Back Up
Now**), restores a backup and starts a new game (**Start New Game…**). A backup is also made
automatically before every start; the newest 10 are kept. Backups are in `out/save-backups` next to `user`.

## Settings and shortcuts

In game:

| Keys | What it does |
|---|---|
| ⌘ , | Port menu (graphics, LOD distance, controls) |
| ⌘ 0 | Show or hide the FPS counter |
| ⌘ 8, F8 or the PS button | Save a screenshot to `Desktop/ISS Screenshots` |
| ⌘ 9 | Show or hide Apple's Metal performance HUD |

A PlayStation or Xbox controller is recommended; keyboard and mouse also work.

In the launcher:

| Setting | Options |
|---|---|
| Smoke and sparks | **Auto** (on, the default), Off (no GPU particles, fastest), All particles (exactly as on the PS4, much slower) |
| Detail | Original (as on the PS4), Lower, Lowest (the game's own lower settings: particles, high-resolution textures, pedestrians; for slower Macs) |
| Frame pacing | Smooth (even frame times, default), 60 Hz steps |
| Window | Full screen (needed for macOS Game Mode) or windowed; fill 16:10 screens without black bars |
| Native GPU process | On by default: runs the renderer as a native Apple Silicon process. Turn it off to run everything under Rosetta |
| Show FPS, Metal Performance HUD | On-screen counters |

Hold ⌥ (Option) while opening the app to show the launcher window when it is set to start the game
directly.

The port menu's **LOD distance** below 1.0x trades distant detail for speed (0.5x: about +8% on open
outdoor views).

## Expected performance

Measured on a **MacBook Pro with M3 Max**, macOS 26, internal display, 1080p rendering, default
settings (GPU particles on, Detail: Original), optimized build, 2026-11-02. Each row is the range
over a 2-4 minute scripted run, after the first seconds.

| Scene | FPS |
|---|---|
| Opening area: bridge with rain, camera turning | 50-55, dips to about 40 |
| Open view of the city from the bridge | 41-46 |
| Smoke-shot tutorial (forest, drone, smoke powers) | 60-64 |
| Burning building (heavy fire and sparks) | 29-49, mostly 35-45 |

With the native GPU process off (everything under Rosetta) the bridge runs at 40-45 and the open
view at 25-27.

What changes it:

- **GPU particles Off**: noticeably faster in effect-heavy scenes, but powers show no smoke and
  there is no rain.
- **LOD distance 0.5x**: about +8% outdoors.
- **Detail: Lower / Lowest**: lighter particles, textures and crowds.
- Resolution and upscaling do not help much: the limit is not pixel count (see
  [Known issues](#known-issues)).
- Memory in game: about 6-8 GB.

These are scripted runs. In one play session (2026-11-03) the bridge ran at 25-30 FPS while a
standing test there gave about 67 FPS (2026-11-04), so the frame rate depends heavily on what is
happening; the difference is not explained yet.

Other Macs have not been measured yet. The limit is mostly CPU-to-GPU round trips, so chips with
fewer performance cores and smaller GPUs (M1, M2, base M3) will be slower than these numbers; Pro
and Max chips of any generation should be closer to them. Reports with your Mac model and FPS are
welcome in [Issues](https://github.com/satperm/iss_mac_port/issues).

## Status

- The game runs from the first cutscene; saving and loading work. Played from a new game to the first
  missions in Seattle (about 70 minutes) on an M3 Max without a crash. Later parts are untested.
- GPU particles: smoke powers, rain, neon, sparks and fire.
- Text and voice languages; controller (USB-C DualSense tested), keyboard and mouse.
- Launcher with save manager, crash reports and session logs.
- Native GPU process (arm64) on by default.
- First start compiles the shaders behind a loading screen (about 2 minutes on an M3 Max).
- Screenshots at any time: ⌘ 8, F8 or the PS button.

Fixed recently:

- Black rounded squares in the sky and in smoke (NaN pixels spread by the game's blur passes).
- Smoke flickers less: lowered wave64 `ReadFirstLane`, and the particle counter copied out of GDS only
  after the compute shaders finished. Some flicker remains (see below).
- Frames where the scenery vanished into fog during camera turns (a depth fast-clear bug).
- The start-up crash right after the splash screen (a MoltenVK drawable race,
  [MoltenVK #2791](https://github.com/KhronosGroup/MoltenVK/issues/2791); the build uses a patched
  MoltenVK).
- A freeze at the intro video.
- Particle draws are no longer emulated through tessellation (+20-50% in particle scenes).

## Known issues

- **Smoke flickers.** Translucent white and dark smoke (fires, chimneys, drones) still flickers and
  moves less naturally than on PS4: parts of it vanish for a frame. Better than before, cause not
  fully found. The smoke of your own powers looks right.
- **Frame rate.** Not a steady 60 FPS; heavy scenes drop to 25-40. In scripted scenes the limit is the
  round trip between the game's CPU code and the GPU (about 20 waits per frame), not the GPU's raw speed
  or the resolution. The drops seen while playing on the bridge are not explained yet.
- **Not checked against PS4 yet:** bright points on the black D.U.P. walls in Seattle and hard-edged
  black boxes behind some HUD icons. They may be the game's own look. The glitchy columns before
  the tunnel to Seattle are the game's own effect, the PS4 version has them too.
- **Only one Mac tested.** The downloadable app has not yet been run on another Mac. The PS button
  screenshot is unconfirmed.
- The controller keeps reaching the game while another app is in front.
- Cutscenes and movies cannot be skipped so far.
- **Shaders the first-start list misses** still compile during play, with short stutters.
- **Memory**: 6-8 GB in game; on 16 GB Macs close other large apps.
- Some HUD elements (for example the current objective text) may be missing at times.
- MoltenVK, the Vulkan-on-Metal layer the port uses, is deprecated by Khronos (KosmicKrisp is its
  successor); the port keeps a patched MoltenVK 1.4.2 for now.

Please report problems in [Issues](https://github.com/satperm/iss_mac_port/issues) with your Mac
model, macOS version, the scene and the session log (see below). No game files, please.

## Roadmap

Near term:

1. **Smoke**: find the remaining flicker.
2. **Frame rate** on the bridge and in other scenes while playing.
3. **Particle performance**: fewer render-pass breaks around particle draws.
4. **HUD**: find the missing objective text.

Toward 60 FPS:

5. **KosmicKrisp**, Khronos' successor to MoltenVK, now that the GPU side is native.
6. A native Metal renderer, only if profiling shows the Vulkan layer is still the bottleneck.

## Troubleshooting and logs

- **Session logs**: `~/Library/Logs/ISS/` (`game.log` is the latest). After a crash the launcher's
  **Crash Report…** window can collect the log and macOS's crash report into a zip for an issue.
- **The game crashed at start**: open it again. The launcher also retries a known start-up crash
  automatically.
- **Black screen, broken picture or very slow after an update**: rebuild (`cd port && bash
  build_macos.sh`). If shaders still misbehave, delete the `cache` and `shader` folders in
  `port/user`.
- **"The game needs Rosetta 2"**: install it with the command in [Requirements](#requirements).

## For developers

- Layout: `port/src` (loader and PS4 runtime, C), `port/gpu` (renderer, C++, shadPS4-derived),
  `port/launcher` (Swift launcher), `port/scripts/macos` (build, packaging and diagnostic scripts).
- `CLAUDE.md` is the running engineering log: what was tried, measured and fixed, and why.
- Unattended test runs with scripted controller input, frame dumps and an FPS/memory report:
  `port/scripts/macos/diag_run.sh` (see its header). Renderer diagnostics are switched on by `TLG_*`
  environment variables documented next to their code.
- Pull requests are welcome; please measure performance changes in a real scene.

## Credits

- [iss_pc](https://github.com/HayatoG/iss_pc) by HayatoG and contributors: the native port this
  fork is based on.
- [shadPS4](https://github.com/shadps4-emu/shadPS4): the GPU core the renderer derives from.
- [MoltenVK](https://github.com/KhronosGroup/MoltenVK): Vulkan on Metal.
- [bloodborne_mac_port](https://github.com/Wooloomooloo2/bloodborne_mac_port): techniques for running
  PS4 code on macOS.

## Legal

This is an independent, non-commercial project. It is not affiliated with, endorsed or sponsored by
the game's publisher or developer. All trademarks belong to their owners. You need your own legally
obtained copy of the game.

License: GPL-2.0-or-later (as iss_pc and shadPS4).
