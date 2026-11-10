#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# CPU profile of the GPU process (tlggpu-host, arm64) during one unattended run:
#
#   TLG_GAME_DIR=... bash port/scripts/macos/host_profile.sh <truck|bridge|user dir> <label> [VAR=value ...]
#
# Runs the scene for 85 s (agent_run.sh), records 6 s of Time Profiler for all processes from second 55
# (no attach: safe with the Rosetta game process), exports the samples and prints them per thread and
# function (tools/gpu_probes/time_profile.py). Files: out/profile/<label>.{trace,xml,txt}. Bounded.
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
scene=$1 label=$2; shift 2
mkdir -p out/profile
trace=out/profile/$label.trace
rm -rf "$trace"
(
    sleep 55
    # arm64 on purpose, and no `timeout` wrapper (an x86-64 one makes xctrace run under Rosetta:
    # "ktrace cannot trace the system under Rosetta translation"); the recording limits itself.
    arch -arm64 xcrun xctrace record --template 'Time Profiler' --all-processes --time-limit 6s \
        --output "$trace" > "out/profile/$label.record.log" 2>&1
) &
bash scripts/macos/agent_run.sh "$scene" "p-$label" 85 "$@" | grep -aE 'FPS|Crash|exit'
wait
arch -arm64 xcrun xctrace export --input "$trace" \
    --xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]' > "out/profile/$label.xml" 2>/dev/null
grep -q 'Recording failed\|\[Error\]' "out/profile/$label.record.log" && sed -n 3,5p "out/profile/$label.record.log"
python3 tools/gpu_probes/time_profile.py "out/profile/$label.xml" | tee "out/profile/$label.txt"
