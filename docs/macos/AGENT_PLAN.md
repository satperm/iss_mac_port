# Plan for unattended agent sessions (8-9 hours, nobody at the Mac)

Read `AGENTS.md` first ("Working unattended"). Work in order; each item has a time box. When a box runs
out without a measured gain, write the numbers into CLAUDE.md and go to the next item. Commit after each
step, open a PR into `macos` when a change is measured (fps_ab.sh, 3 runs, table in the PR) and needs no
eye check, or say in the PR what the owner must look at. No release tags, nothing in other repositories.

Tools: `scripts/macos/agent_run.sh <truck|bridge> <label> <s> [VAR=v ...]` (one bounded run + summary),
`fps_ab.sh -n 3 -s 60 [-u out/bridge-fresh] a "..." b "..."` (A/B, ~7 min per variant pair at 60 s),
`wait_game.sh`. Long jobs in the background; never an unbounded wait.

## 1. (done 2026-11-09, no gain) Graphics waiting only at the game's sync points

`TLG_ACB_MAIN_WAIT=sync` (graphics submissions wait for the compute queue only after a graphics
WAIT_REG_MEM met by a compute fence): truck 45.0 -> 43.9, bridge 64.0 -> 64.6 (3 x 60 s each).
Dropped; diff kept at ~/iss_mac_port/port/out/experiments/main-wait-sync.diff. Do not retry.

## 2. CPU per draw in tlggpu-host (the critical path; 3-4 h)

The game keeps one frame at the GPU (CLAUDE.md, 2026-11-09 night), so every microsecond per draw in the command
processor and draw recording threads is frame time. Method: `xctrace record --template 'Time Profiler'
--all-processes --time-limit 6s` during `agent_run.sh truck`, export `time-profile`, aggregate per thread
(parsers: thread/process nest in the row, frames sit directly in `tagged-backtrace`). Measure with two saved
library builds alternated between runs and the `us/draw` frame stat; a +1 FPS difference needs >= 3 runs each on
a quiet Mac (check `ps -Ao pcpu,comm -r`).
Done: dirty-block walk (truck 43.1 -> 46.8). Tried, no clear gain: PendingWriteOverlaps, TLG_GFX_FENCE_BATCH,
TLG_SUBMIT_DRAWS, TLG_IDLE_FLUSH, graphics event fences, sync-point waits. Candidates, in order:
1. MVKCommandBuffer::reset inside vkBeginCommandBuffer on DrawRec (10% of it): begin the next command buffer on
   another thread, or keep a pool of begun ones.
2. MoltenVK MVKCmdPushDescriptorSet (setContent + clearDescriptorWrites allocate per call; 25% of VkRecorder).
3. TextureCache::RetireReadbacks copies on DrawRec (5.5%): off the recording thread, in order with ResolveReadbacks.
4. BindResources / BindTextures / BarrierTracker::InsertNode per draw.
Busy streets: save `~/iss_mac_port/port/out/save-backups/seattle-run-20261103` (standing 46.5 FPS with 0.1.6).

## 4. Launcher automatic updates (fallback when runs are noisy, 2-3 h, no game runs)

Port `launcher/Model/Update.swift` and `launcher/App/Updater.swift` from ~/bloodborne_mac_port
(GitHub releases of satperm/iss_mac_port, tags mac-vX.Y.Z, asks once, test builds = pre-releases).
`bash scripts/macos/build_launcher.sh --test` must pass.

## 5. Render passes (~190 a frame): survey only, write findings

## At the end

Report in Russian: what changed, numbers, what the owner must check by eye, open PRs.
