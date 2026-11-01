#!/usr/bin/env bash
# macOS counterpart of run.sh: prepares the game image and starts out/tlg-probe (x86-64, under
# Rosetta 2). Same environment as run.sh (TLG_GAME_DIR, TLG_DATA_DIR, TLG_PATCHES, TLG_MODS_*,
# TLG_USER_DIR, TLG_TIMEOUT, ...); extra arguments go to tlg-probe (for example --cpu-only).
# As run.py (Windows) it asks tlg-probe which instruction sets are missing and builds the ISA
# rewrite table for them: under Rosetta that is SSE4A (src/cpu_check.c).
#
#   TLG_GAME_DIR=/path/to/CUSA00004 bash port/run_macos.sh [--cpu-only]
set -euo pipefail
cd -- "$(dirname -- "$0")"
data=${TLG_DATA_DIR:-.}
out=$data/out
mkdir -p "$out"
export TLG_CONFIG=${TLG_CONFIG:-$data/tlg.ini}
# Python: the port's own environment (iced-x86 for the ISA rewrite) when there is one.
if [[ -z ${PYTHON:-} ]]; then
    if [[ -x .venv/bin/python ]]; then PYTHON=.venv/bin/python; else PYTHON=$(command -v python3 || true); fi
fi
if [[ -z ${PYTHON:-} ]]; then echo 'Install Python 3 or set PYTHON.' >&2; exit 1; fi
if ! "$PYTHON" -c 'import iced_x86' 2>/dev/null; then
    echo "The ISA rewrite needs iced-x86: python3 -m venv port/.venv && port/.venv/bin/pip install iced-x86" >&2; exit 1
fi
game=${TLG_GAME_DIR:-../game/CUSA00223}
if [[ ! -f $game/eboot.bin ]]; then echo "No eboot.bin in $game (set TLG_GAME_DIR)." >&2; exit 1; fi
if [[ -z ${TLG_PREBUILT:-} ]]; then
    bash build_macos.sh
fi
probe=${TLG_PROBE:-out/tlg-probe}
# Vulkan through MoltenVK (port/deps, scripts/macos/bootstrap_deps.sh), or TLG_VULKAN_DRIVER=kosmickrisp
# (scripts/macos/build_kosmickrisp.sh: correct picture, ~30% slower under Rosetta as of Mesa 26.2.5).
if [[ ${TLG_VULKAN_DRIVER:-} == kosmickrisp ]]; then
    export VK_DRIVER_FILES=${VK_DRIVER_FILES:-$PWD/deps/kosmickrisp/share/vulkan/icd.d/kosmickrisp_mesa_icd.x86_64.json}
fi
export VK_DRIVER_FILES=${VK_DRIVER_FILES:-$PWD/deps/x86_64/share/vulkan/icd.d/MoltenVK_icd.json}
# MoltenVK without Metal argument buffers (descriptors bound directly, as bloodborne_mac_port 0.3.3):
# with them, GPU time per frame grew over the session (title screen 10 -> 0.6 FPS in 70 s);
# without, a steady ~16 FPS there before the NoContraction change, ~30 after.
export MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=${MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS:-0}
# Room for the ~80 submissions a frame the tessellation limits make (MoltenVK's default of 64
# active command buffers made submissions wait). Not MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=2:
# encoding at record time crashed in the driver's occlusion query setup (cannery area).
export MVK_CONFIG_MAX_ACTIVE_METAL_COMMAND_BUFFERS_PER_QUEUE=${MVK_CONFIG_MAX_ACTIVE_METAL_COMMAND_BUFFERS_PER_QUEUE:-512}
# Synchronous submits (MoltenVK encodes in vkQueueSubmit). The asynchronous mode (0: encoding on
# MoltenVK's queue thread, cannery benchmark 24.0 -> 27.0 FPS) crashed at start-up right after the
# splash screen in Metal's scheduled handlers (objc_msgSend in didSchedule, Block_copy in
# MTLDispatchListAppendBlock): 3 of 4 runs with 2 shots/s, 1 in 6-8 plain starts; with 1: 0 of 4
# and 0 of 8 since (2026-10-31). But synchronous costs ~25% FPS (bridge 56-61 -> 42-47), and the
# crash only happens at start-up, so asynchronous stays the default; 1 for crash-free starts.
export MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=${MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS:-0}
# MoltenVK checks that the objects a command buffer uses are still alive when it encodes them:
# the renderer sometimes destroys an image view between recording and the (asynchronous) encode,
# which crashed ~1 start in 3-7 (AGX setTexture in MVKCmdDispatch::encode, objc_msgSend in
# didSchedule). 2026-10-31: 0 crashes in 7 starts with it, no FPS change on the bridge.
export MVK_CONFIG_LIVE_CHECK_ALL_RESOURCES=${MVK_CONFIG_LIVE_CHECK_ALL_RESOURCES:-1}

original_game=$game
game=$("$PYTHON" scripts/mods.py "$game" --out "$out" \
    --mods-dir "${TLG_MODS_DIR:-$data/mods}" --config "${TLG_MODS_CONFIG:-$data/mods.json}" \
    --enabled "${TLG_MODS_ENABLED:-1}")
if [[ $game != "$(realpath "$original_game")" ]]; then
    mod_game=$game
    trap '"$PYTHON" -c '\''import shutil,sys; shutil.rmtree(sys.argv[1])'\'' "$mod_game"' EXIT
fi
"$PYTHON" scripts/prepare.py "$game" --out "$out"
# As run.py: link_modules.py links libc.prx and libSceFios2.prx (run.sh's link_libc.py step predates
# it and rejects this game's libc).
"$PYTHON" scripts/link_modules.py "$game" --out "$out"
# Instruction sets this CPU lacks: exit 20 is a set nothing covers; the rest get the rewrite table.
isa_args=()
features_status=0
features=$("$probe" --cpu-features) || features_status=$?
echo "$features"
if (( features_status == 20 )); then exit 20; fi
if [[ ${TLG_ISA_REWRITE:-} != 0 ]] && { [[ $features == "CPU: missing "* ]] || [[ -n ${TLG_ISA_REWRITE:-} ]]; }; then
    # The table only depends on the linked image, the script and the CPU's feature list; building
    # it takes ~2 s of every start, so it is kept until one of them changes.
    isa_key=$( { shasum "$out/boot-linked.bin" scripts/isa_rewrite.py | cut -d' ' -f1; echo "$features"; } | shasum | cut -d' ' -f1)
    if [[ -f $out/isa-rewrite.bin && -f $out/isa-rewrite.key && $(<"$out/isa-rewrite.key") == "$isa_key" ]]; then
        isa_args=(--isa-rewrite "$out/isa-rewrite.bin")
    elif "$PYTHON" scripts/isa_rewrite.py "$out/boot-linked.bin" --out "$out"; then
        echo "$isa_key" > "$out/isa-rewrite.key"
        isa_args=(--isa-rewrite "$out/isa-rewrite.bin")
    else
        echo 'ISA rewrite failed: starting without the table' >&2
    fi
fi
"$PYTHON" scripts/content_profile.py "$game" --out "$out" --sku "${TLG_CONTENT_SKU:-full}"
"$PYTHON" scripts/patches.py --out "$out" --extra "${TLG_PATCHES:-}" --game-dir "$game" \
    --patches-dir "${TLG_PATCHES_DIR:-$data/patches}" --patches-config "${TLG_PATCHES_CONFIG:-$data/patches.json}"
# Vblank uncapped (480 Hz ticks, the frame rate capped at the display's): the game's frames take
# their own time instead of snapping to 16.7 / 33.3 ms. Cannery, quiet M3 Max: 44.3 FPS with a
# 2.5 ms frame-time spread, against 45.4 FPS and 8.2 ms (judder) at 60 Hz. TLG_VBLANK_HZ=60: as before.
export TLG_VBLANK_HZ=${TLG_VBLANK_HZ:-0}
# As run.py: the temporal upscalers are not calibrated for this game yet; linear image readbacks
# fix its corrupted textures.
export TLG_UPSCALER=${TLG_UPSCALER:-none}
# Full screen, the image stretched over the whole display (16:10 laptops: no black bars).
# TLG_FULLSCREEN=0 / TLG_STRETCH=0 for a window or the original 16:9 picture.
export TLG_FULLSCREEN=${TLG_FULLSCREEN:-1}
export TLG_STRETCH=${TLG_STRETCH:-1}
# On-screen FPS (the port's counter) and Apple's Metal performance HUD (frame time, GPU time,
# memory); TLG_SHOW_FPS=0 / MTL_HUD_ENABLED=0 hide them.
export TLG_SHOW_FPS=${TLG_SHOW_FPS:-1}
export MTL_HUD_ENABLED=${MTL_HUD_ENABLED:-1}
# GPU particles (smoke, sparks, neon, rain): the game reads its particle survivor counts back from
# GPU memory every frame.
#   shared (default): unified memory (~9 GB) + the late particle read: smoke, rain, sparks
#     (2026-10-31: bridge 28-37 FPS, smoke tutorial 44-54, burning building 21-25).
#   off: copied memory, Relaxed readbacks, no GPU particles (bridge 56-61 FPS).
#   precise: all particles: Precise readbacks, write merge, no-wait matrix stores; much slower
#     (bridge ~16-17 FPS); ~15 GB.
# The bridge's broken frames were a depth fast-clear bug, fixed 2026-10-31 (vk_rasterizer.cpp).
# Quad lists (particles) as indexed triangles instead of MoltenVK's tessellation emulation, the
# indexed ones expanded on the GPU (2026-10-31): burning building 21-25 -> 30-34 FPS, 9.1 -> 6.5 GB,
# bridge 28-37 -> 35-43, same picture. TLG_MAC_QUAD_INDEX=0 restores tessellation.
# BP08 (the driver VkPipelineCache blob, ~80 MB) off: loading it hung ~half of the starts at
# "driver pipeline cache loaded" in both modes (2026-11-01: 0 of 4 hangs without it).
export TLG_BP08=${TLG_BP08:-0}
export TLG_MAC_QUAD_INDEX=${TLG_MAC_QUAD_INDEX:-1}
# The renderer as a native arm64 process next to the game (out/tlggpu-host; docs/macos/
# GPU_PROCESS_PLAN.md): burning building ~36 -> ~44 FPS, bridge the same (2026-11-01).
# The default since its stale textures were fixed (2026-11-01). TLG_GPU_PROCESS=0 (launcher toggle
# off): one x86-64 process under Rosetta.
export TLG_GPU_PROCESS=${TLG_GPU_PROCESS:-1}
# Natively MoltenVK picks MTLEvent semaphores, under Rosetta the implicit single-queue ordering;
# with ~50 submissions a frame the implicit one is faster: native burning building 37.8 -> 43.9 FPS.
[[ $TLG_GPU_PROCESS == 1 ]] && export MVK_CONFIG_VK_SEMAPHORE_SUPPORT_STYLE=${MVK_CONFIG_VK_SEMAPHORE_SUPPORT_STYLE:-0}
TLG_PARTICLES=${TLG_PARTICLES:-shared}
case $TLG_PARTICLES in
    shared)
        export TLG_UNIFIED_MEMORY=${TLG_UNIFIED_MEMORY:-1}
        # The late particle read is what lets the game see its GPU particles (smoke, rain,
        # sparks). It lost the device in the burning building before 2026-10-31; with that day's
        # fixes: no hang in 240 s there, smoke/rain/neon back, bridge clean.
        export TLG_PARTICLE_LATE_READ=${TLG_PARTICLE_LATE_READ:-1}
        export TLG_PARTICLE_LIGHT_ONE_SUBMIT=${TLG_PARTICLE_LIGHT_ONE_SUBMIT:-1}
        # Tessellated indirect draws in flight (2 kept MoltenVK's scratch pool small); 8: +4 FPS.
        export TLG_MAC_TESS_IN_FLIGHT=${TLG_MAC_TESS_IN_FLIGHT:-8};;
    precise)
        export TLG_UNIFIED_MEMORY=${TLG_UNIFIED_MEMORY:-0}
        export TLG_READBACKS=${TLG_READBACKS:-2}
        export TLG_WRITE_MERGE=${TLG_WRITE_MERGE:-1}
        export TLG_NOWAIT_WRITES=${TLG_NOWAIT_WRITES:-43e1fb,43e23a,43e26e,43e280,43e286};;
    *)
        export TLG_UNIFIED_MEMORY=${TLG_UNIFIED_MEMORY:-0};;
esac
export TLG_READBACK_LINEAR=${TLG_READBACK_LINEAR:-1}
# Image readbacks copied on the GPU and applied when the CPU touches the pages (as the Windows
# launcher's default): the cannery's 512x512 RGBA16F image went back ~330 times a second, each copy
# draining the GPU, and the CPU never reads it. Interior 34 -> 43 FPS, cutscene 41 -> 50.
export TLG_DEFERRED_READBACK=${TLG_DEFERRED_READBACK:-1}
probe_args=("$out/boot-linked.bin" --content-profile "$out/content.bin" --patches "$out/patches.bin" --app0 "$game"
    --user "${TLG_USER_DIR:-$data/user}" --timeout "${TLG_TIMEOUT:-0}" ${isa_args[@]+"${isa_args[@]}"} "$@")
# First-start shader compilation (vk_precompile.h): the job list from the player's game files, then
# the game compiles it behind a loading screen, in parallel. On a cold cache the bridge lost ~26 s to
# compiles on the render thread, frames up to 13 s; with this no frame over about a second
# (2026-11-01). TLG_PRECOMPILE=0: shaders compile in game.
if [[ ${TLG_PRECOMPILE:-1} != 0 && -z ${TLG_PRECOMPILE_JOBS:-} ]]; then
    jobs=${TLG_USER_DIR:-$data/user}/cache/shader-jobs.bin
    if [[ ! -f $jobs ]] && command -v osascript >/dev/null; then
        osascript -e 'display notification "Preparing shaders from your game files, once. The game opens in about a minute." with title "ISS"' 2>/dev/null || true
    fi
    "$PYTHON" scripts/macos/prepare_shader_jobs.py "$game" "$jobs" && export TLG_PRECOMPILE_JOBS=$jobs
fi
exec "$probe" "${probe_args[@]}"
