# Plan for unattended agent sessions (8-9 hours, nobody at the Mac)

Read `AGENTS.md` first ("Working unattended"). Work in order; each item has a time box. When a box runs
out without a measured gain, write the numbers into CLAUDE.md and go to the next item. Commit after each
step, open a PR into `macos` when a change is measured (fps_ab.sh, 3 runs, table in the PR) and needs no
eye check, or say in the PR what the owner must look at. No release tags, nothing in other repositories.

Tools: `scripts/macos/agent_run.sh <truck|bridge> <label> <s> [VAR=v ...]` (one bounded run + summary),
`fps_ab.sh -n 3 -s 60 [-u out/bridge-fresh] a "..." b "..."` (A/B, ~7 min per variant pair at 60 s),
`wait_game.sh`. Long jobs in the background; never an unbounded wait.

## 1. Graphics waits only at the game's sync points (branch agent/main-wait, 1 h)

`TLG_ACB_MAIN_WAIT=sync|all`: the main queue's submissions wait for the compute queue only after a
graphics WAIT_REG_MEM met by a compute fence (liverpool.cpp RunGfxWaitsCompute), not before every
submission. Truck trace: ~0.5 s of 6.5 s GPU idle at compute -> graphics. A/B truck and bridge; if
sync is faster, check smoke flicker numerically (SHOT_SECONDS=1 at the truck, compare against `all`)
and leave the default flip for the owner's eye check.

## 2. Fewer main-queue command buffers (2 h)

~20 submissions a frame at the truck; each costs ~0.5 ms of GPU idle (Metal System Trace: gaps between
command buffers). Find what flushes (Scheduler::Flush callers, TLG_QUEUE_STATS=1, TLG_PASS_STATS=1),
merge where nothing waits on the CPU, behind a switch. Trace: `xcrun xctrace record --template
'Metal System Trace' --all-processes --time-limit 6s` while a run is going (never --attach to tlg-probe).

## 3. CPU per draw in busy streets (2 h)

Save: `~/iss_mac_port/port/out/save-backups/seattle-run-20261103` (copy, never the original;
agent_run.sh takes a user dir with savedata/). `sample tlggpu-host 10` (arm64 process: safe; never
sample or attach to tlg-probe). Look for per-draw hot spots in the GPU thread and stage B.

## 4. Launcher automatic updates (fallback when runs are noisy, 2-3 h, no game runs)

Port `launcher/Model/Update.swift` and `launcher/App/Updater.swift` from ~/bloodborne_mac_port
(GitHub releases of satperm/iss_mac_port, tags mac-vX.Y.Z, asks once, test builds = pre-releases).
`bash scripts/macos/build_launcher.sh --test` must pass.

## 5. Render passes (~190 a frame): survey only, write findings

## At the end

Report in Russian: what changed, numbers, what the owner must check by eye, open PRs.
