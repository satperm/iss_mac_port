# Plan for unattended agent sessions (8-9 hours, nobody at the Mac)

Read `AGENTS.md` first ("Working unattended"), then the last entries of `CLAUDE.md`. Work in order; every
item has a time box. When a box runs out without a measured gain, write the numbers into CLAUDE.md and go
on. Never wait for an answer: nobody is there. Decide, write down why, continue.

## What the agent may do alone

- Branch `agent/<topic>` from `origin/macos` (or from the previous unmerged agent branch when the work
  builds on it), commit, push, open PRs into `macos` with the measurement table.
- Not alone: merging PRs, tags/releases, force pushes, anything outside this repository, anything on the
  owner's real saves, changing a default that alters the picture (leave it default-off, say what to look at).
- A default may be flipped in a PR (separate commit) when: no picture change is possible, median gain
  >= 1.5% in at least one of truck / bridge / Seattle over >= 5 alternated runs, and no scene loses more
  than 0.5%.

## 0. Start of the session (10 min)

```sh
caffeinate -dimsu -t 36000 &          # keep the Mac and the display awake for the session
cd ~/iss-rel && git fetch origin && git status --short | grep -v 'deps\|venv\|fsr'
pmset -g batt | head -1               # must say AC Power; on battery numbers are ~15% lower: note it
ps -Ao pcpu,comm -r | head -5         # anything above ~30% that is not ours: wait or note it
export TLG_GAME_DIR=$HOME/Downloads/Infamous-windows-x86_64/game
```

If PR #4 (`agent/per-draw-2`) is merged, branch from `origin/macos`; if not, continue on `agent/per-draw-2`.
Build with LTO (`cd port && bash build_macos.sh`, ~5 min), then `bash scripts/macos/lib_ab.sh save head`.
Sanity: `bash scripts/macos/agent_run.sh truck sanity 70` must give 46-48 FPS on the M3 Max. Far lower
with nothing else running: the display may be off or the Mac on battery; fix that before measuring.

## 1. Morning re-measure on the quiet Mac (1.5 h)

Yesterday's evening numbers were taken with Chrome and VS Code busy. One build, switches only:

```sh
cd ~/iss-rel/port
for scene in truck bridge $HOME/iss_mac_port/port/out/save-backups/seattle-run-20261103; do
  bash scripts/macos/lib_ab.sh run -n 5 -s 60 $scene head head:TLG_GFX_FENCE_BATCH=8 \
      head:TLG_IDLE_FLUSH=16,TLG_IDLE_FLUSH_PASS=0 head:TLG_GFX_FENCE_BATCH=8,TLG_IDLE_FLUSH=16,TLG_IDLE_FLUSH_PASS=0
  cp out/ab/last.txt out/ab/morning-$(basename $scene).txt
done
```

(`bridge` = `out/bridge-fresh`. ~30 min a scene; run each scene as one background command and wait for
its completion notice.) Evening numbers to compare: FENCE_BATCH=8 truck 47.3 -> 48.3, bridge 66.1 -> 65.6;
IDLE_FLUSH=1 outside passes truck 43.8 -> 42.9, bridge 61.7 -> 64.2 (noisy). Apply the default rule above;
put the three tables into CLAUDE.md and the PR either way. `TLG_ACB_MAIN_WAIT=sync` and graphics event
fences are settled (no gain): do not repeat them.

## 2. CPU per draw in tlggpu-host: the critical path (4-5 h)

The game keeps one frame at the GPU (CLAUDE.md, 2026-11-09 night), so every microsecond per draw in the
command processor and draw recording threads is frame time. Loop:

1. `bash scripts/macos/host_profile.sh truck <label>` -> `out/profile/<label>.txt` (per thread: leaves and
   inclusive). `python3 tools/gpu_probes/time_profile.py out/profile/<label>.xml --callers <symbol>`.
2. Change one thing. Build (`TLG_LTO=OFF bash scripts/macos/build_gpu_arm64.sh` while iterating; LTO for the
   final numbers), `lib_ab.sh save fix`, `lib_ab.sh run -n 3 truck head fix`, then bridge. Watch `us/draw`.
3. Gain: commit, PR with the table. No gain: revert, one line with numbers in CLAUDE.md.

Candidates, in order (truck profile after PR #3; samples of ~6000 per thread in 6 s):
1. `MVKCommandBuffer::reset` inside `vkBeginCommandBuffer` on DrawRec (328 inclusive, 10% of the thread; one
   per submission, ~77 a frame): begin the next command buffer on another thread ahead of time, or keep two
   begun ones (Scheduler::AllocateWorkerCommandBuffers). The pool is externally synchronized: only begin a
   buffer no other thread touches.
2. MoltenVK `MVKCmdPushDescriptorSet::setContent` + `clearDescriptorWrites` allocate per call (700 of
   VkRecorder's ~3000; `PushChanged` in vk_pipeline_common.cpp): fewer push calls (skip unchanged sets is
   already there: check why it misses), or a MoltenVK patch reusing the storage.
3. `TextureCache::RetireReadbacks` copies into guest memory on DrawRec (185): off the recording thread, in
   order with `ResolveReadbacks`.
4. `Rasterizer::BindResources` 748 / `BindTextures` 410 / `BarrierTracker::InsertNode` 237 per draw.
5. Command processor: `CollectRingBindings` 789 (CopySparseMemory 265), `GetGraphicsPipeline` 630.

Also worth one look: VkRecorder spins most of its time (leaf in RecorderThread 1700): is the spin stealing a
performance core from DrawRec? Try parking sooner behind a switch.

## 3. Busy streets (1 h, after item 2 has landed something)

Save `~/iss_mac_port/port/out/save-backups/seattle-run-20261103` (agent_run.sh takes the folder; 46.5 FPS
standing with 0.1.6). Profile there with host_profile.sh: with ~2x the draws the same per-draw costs weigh
more; note what differs from the truck.

## 4. Fallback without game runs: launcher automatic updates (2-3 h)

Port `launcher/Model/Update.swift` and `launcher/App/Updater.swift` from ~/bloodborne_mac_port (GitHub
releases of satperm/iss_mac_port, tags mac-vX.Y.Z, asks once, test builds = pre-releases).
`bash scripts/macos/build_launcher.sh --test` must pass. Default off until the owner has tried it.

## At the end

Update CLAUDE.md (numbers of everything tried), AGENTS.md if a rule or priority changed, this file for the
next session. Final message in Russian: what changed and by how much, open PRs in merge order, what the
owner must check by eye, what did not work.
