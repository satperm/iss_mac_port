# ISS — macOS port (Apple Silicon)

Play your own copy of the PS4 game ISS (CUSA00223 / CUSA00004, update 1.07) on an Apple Silicon
Mac. This repository contains **no game code or data**: it loads the player's own decrypted copy
and runs it with a PS4 runtime (`port/src`) and a Vulkan renderer (`port/gpu`, derived from
shadPS4) on Metal through MoltenVK. The game's x86-64 code runs under Rosetta 2.

It is a macOS fork of [iss_pc](https://github.com/HayatoG/iss_pc), the native PC port.

> **Status: first test version, work in progress.** The story can be played from the start with
> GPU particles (smoke, rain, sparks). Tested on one Mac (M3 Max, 36 GB) as far as the first missions
> in Seattle. On that Mac expect about 65 FPS on the opening bridge, about 45 in the heaviest
> fire-and-smoke scene (the launcher's Benchmark) and 25-30 in busy streets. See
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
| ⌘ 9 | Show or hide Apple's Metal Performance HUD (hides the port's FPS counter) |

A PlayStation or Xbox controller is recommended; keyboard and mouse also work.

In the launcher:

| Setting | Options |
|---|---|
| Smoke and sparks | **Auto** (on, the default), Off (no GPU particles, fastest), All particles (exactly as on the PS4, much slower) |
| Detail | Original (as on the PS4), Lower, Lowest (the game's own lower settings: particles, high-resolution textures, pedestrians; for slower Macs) |
| Frame rate cap | Display (the display's refresh rate, at most 120; default), 60, 30 (steadier on slower Macs) |
| Render scale | 100% (default) down to 50%: the game draws a smaller picture and scales it up; more FPS where the GPU is the limit, softer image |
| Frame pacing | Smooth (even frame times, default), 60 Hz steps |
| Window | Full screen (needed for macOS Game Mode) or windowed; fill 16:10 screens without black bars |
| Native GPU process | On by default: runs the renderer as a native Apple Silicon process. Turn it off to run everything under Rosetta |
| FPS counter | Off, Game (the port's counter), Metal HUD (Apple's Metal Performance HUD); one at a time |
| Benchmark (button) | 90 s at the truck crash, standing still, with the settings above; shows the result, saves it in `~/Library/Logs/ISS/Benchmarks` and can open a filled-in GitHub issue to compare Macs |

Hold ⌥ (Option) while opening the app to show the launcher window when it is set to start the game
directly.

The port menu's **LOD distance** below 1.0x trades distant detail for speed (0.5x: about +8% on open
outdoor views).

## Expected performance

Measured on a **MacBook Pro with M3 Max**, macOS 27, 1080p rendering, default settings, release
0.1.6 (2026-11-09). Each value is the median of 40 s after loading.

| Scene | FPS |
|---|---|
| Truck crash in the forest (fire, smoke, sparks; the launcher's **Benchmark**), standing | 45 (0.1.5: 45, 0.1.3: 36) |
| Bridge after the car crash (rain, smoke), standing | 65 (0.1.5: 60) |
| Bridge, camera turning at a constant speed | 53 (0.1.5: 49) |
| Bridge with render scale 50% | about 72 |
| Frame rate cap 30 | exactly 30 |

Older scripted runs while moving (0.1.2, 2026-11-02): bridge with rain and the camera turning 50-55
(dips to about 40), open city view from the bridge 41-46, burning building 29-49.

What changes it:

- **Render scale** below 100%: about +11% at 67% on an M3 Max, more on Macs with smaller GPUs.
- **Smoke and sparks: Off**: faster in effect-heavy scenes, but powers show no smoke and there is no
  rain.
- **LOD distance 0.5x** (port menu): about +8% outdoors.
- **Detail: Lower / Lowest**: lighter particles, textures and crowds.
- Memory in game: about 6-8 GB.

Other Macs have not been measured yet. Run the launcher's **Benchmark** and press "Share on GitHub"
to add yours.

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

- **The game's particle work runs on its own GPU queue** (0.1.6): its results no longer wait behind
  the frame's graphics (about 10 ms -> 2.4 ms on the bridge). Bridge 60 -> 65 FPS standing, 49 -> 53
  turning the camera; the truck crash stays at 45. `TLG_ACB_QUEUE=0` turns it off (as 0.1.5).
- **Benchmark in the launcher**: 90 s at the truck crash (fire, smoke, sparks), standing still, with your
  settings; your saves are not touched. The result is shown, saved in `~/Library/Logs/ISS/Benchmarks`,
  and "Share on GitHub" opens a filled-in issue (label `benchmark`) to compare Macs.
- **Metal Performance HUD works** (launcher: FPS counter -> Metal HUD). The GPU process ran its window
  loop as one main-queue block that never returned, so the HUD never attached; the loop now yields the
  main queue every pass. One counter at a time: the game's or the HUD.
- **Launcher: frame rate cap (Display / 60 / 30) and render scale (100% to 50%)** for slower Macs. 30 FPS
  is steadier than an uneven 35-50; the render scale draws the game at a part of 1080p and scales it up
  (`TLG_FPS_LIMIT`, `TLG_RENDER_SCALE`).
- **Smoke and fire are complete and steady, and heavy scenes run faster** (truck crash, M3 Max: 36 -> 45
  FPS). The game runs its GPU particles as ~95 small compute jobs in a row on a compute queue, with no
  sync between them, as the PS4 allows; they now overlap on the GPU instead of running one after
  another (MoltenVK patch for concurrent compute encoders), and the particle system's fences report
  when the GPU is really done, so no smoke system goes missing. On the bridge 69 -> 59 FPS (the correct
  smoke there is more work). `TLG_UNIFIED_COMPUTE_FENCES=0` brings back the old smoke,
  `TLG_ACB_UNORDERED=0` / `MVK_TLG_CONCURRENT_DISPATCH=0` the serial compute queue.
- **Headlight and lamp glare no longer shows through walls** or from far away. The game asks the GPU how
  much of each light is visible; every light is now answered "hidden" through the ordered path, so the
  glare sprites stay off behind objects. Same FPS on the bridge (`TLG_OCCLUSION_MODE=4` restores the
  counted queries).
- **About 3% more FPS on the bridge** (M3 Max, 65.5 -> 67.5 on average, steadier lows). A stencil test
  that writes nothing keeps the depth buffer read-only, so it no longer changes layout, ending the render
  pass, each time the game samples it (`TLG_STENCIL_RO=0` restores the old behaviour).
- **Smoke no longer flickers** (fires, chimneys, the intro text). The game sorts its smoke particles on
  the GPU in steps that rely on the PS4's 64 lanes running in lockstep; Apple GPUs run 32 at a time, so
  the steps raced and particles were duplicated or dropped every frame. Compute shaders now get a
  barrier there (`TLG_WAVE_MEM_BARRIERS`, from the analysis in
  [iss_pc #557](https://github.com/HayatoG/iss_pc/issues/557) on NVIDIA, which also has 32 lanes).
- Black rounded squares in the sky and in smoke (NaN pixels spread by the game's blur passes).
- Frames where the scenery vanished into fog during camera turns (a depth fast-clear bug).
- The start-up crash right after the splash screen (a MoltenVK drawable race,
  [MoltenVK #2791](https://github.com/KhronosGroup/MoltenVK/issues/2791); the build uses a patched
  MoltenVK).
- A freeze at the intro video.
- Particle draws are no longer emulated through tessellation (+20-50% in particle scenes).

## Known issues

- **Frame rate is not a steady 60 everywhere.** The bridge is about 65 standing and 50-55 while moving;
  the truck crash 45. Busy streets with thousands of objects are limited by the CPU work per draw
  (about 25-30 FPS where the game draws ~7,500 objects a frame). Drops seen while playing on the
  bridge are not explained yet.
- **No lens glare at all.** To stop headlight and lamp glare showing through walls, every light is
  reported hidden, so the glare sprites never draw, also where the light is in plain view
  (`TLG_OCCLUSION_MODE=4`: counted queries, glare back, but through walls too).
- **No temporal upscaling.** FSR 3 / MetalFX flicker and ghost here: the game's jitter and camera motion
  are not wired to the upscaler yet (the game-side hooks exist but are off). The render scale is a
  plain resize.
- **The port menu's frame rate limit has no effect** with Smooth frame pacing (the default); use the
  launcher's Frame rate cap.
- **Not checked against PS4 yet:** bright points on the black D.U.P. walls in Seattle and hard-edged
  black boxes behind some HUD icons. They may be the game's own look. The glitchy columns before
  the tunnel to Seattle are the game's own effect, the PS4 version has them too.
- **Only one Mac tested** (M3 Max). The PS button screenshot is unconfirmed.
- The controller keeps reaching the game while another app is in front.
- Cutscenes and movies cannot be skipped so far.
- **Shaders the first-start list misses** still compile during play, with short stutters. The first
  Benchmark after an update can be lower for the same reason: run it twice.
- **Memory**: 6-8 GB in game; on 16 GB Macs close other large apps.
- Some HUD elements (for example the current objective text) may be missing at times.
- MoltenVK, the Vulkan-on-Metal layer the port uses, is deprecated by Khronos; the port keeps a patched
  MoltenVK 1.4.2. Its successor KosmicKrisp (Mesa 26.2.5, native) reaches gameplay here but needs about
  twice the GPU time (bridge 36 vs 69 FPS), so it is not used.

Please report problems in [Issues](https://github.com/satperm/iss_mac_port/issues) with your Mac
model, macOS version, the scene and the session log (see below). No game files, please.

## Roadmap

Near term:

1. **CPU work per draw** in busy streets (profile where the game draws ~7,500 objects a frame).
2. **Fewer GPU submissions a frame** (each leaves the GPU idle for a moment).
3. **Automatic updates** in the launcher (as bloodborne_mac_port), keyboard and mouse remapping.
4. **HUD**: find the missing objective text.

Later:

5. **Temporal upscaling** (FSR 3, MetalFX) through the game's own jitter and camera (hooks from iss_pc),
   useful for slower Macs and for sharper images above 1080p.
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
