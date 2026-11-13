#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# GPU time per kind of render pass during one unattended run (Metal System Trace):
#
#   TLG_GAME_DIR=... bash port/scripts/macos/pass_profile.sh <truck|bridge|user dir> <label> [VAR=value ...]
#
# Runs the scene for 50 s (agent_run.sh) with TLG_PASS_LABELS=1 and the keep-alive filler off, records
# 4 s of Metal System Trace for all processes from second 28 (before a clock-down could come), exports the
# GPU intervals and prints tools/gpu_probes/trace_passes.py's table (ms a frame per kind of pass; the frame
# count comes from the run's FPS). Files: out/profile/<label>.mst.{trace,xml,txt}. Bounded.
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
scene=$1 label=$2; shift 2
mkdir -p out/profile
trace=out/profile/$label.mst.trace
rm -rf "$trace"
(
    sleep 28
    arch -arm64 xcrun xctrace record --template 'Metal System Trace' --all-processes --time-limit 4s \
        --output "$trace" > "out/profile/$label.mst.record.log" 2>&1
) &
out=$(bash scripts/macos/agent_run.sh "$scene" "m-$label" 50 TLG_PASS_LABELS=1 TLG_GPU_KEEPALIVE=0 TLG_FRAME_STATS=1 "$@")
wait
echo "$out" | grep -aE 'FPS|Crash|exit'
d=$(echo "$out" | grep -o 'out/diag/[^ ]*' | head -1)
# The FPS of the 5 s window the recording fell into (the 6th line: 25-30 s after the first frame stats).
fps=$(grep -a 'Frame stats' "$d/game.log" | grep -o 'Frame stats: [0-9.]*' | awk '{print $3}' | sed -n '6p')
frames=$(awk -v f="${fps:-60}" 'BEGIN{printf "%.0f", f*4}')
arch -arm64 xcrun xctrace export --input "$trace" \
    --xpath '/trace-toc/run[@number="1"]/data/table[@schema="metal-gpu-intervals"]' > "out/profile/$label.mst.xml" 2>/dev/null
echo "frames in the trace: $frames (at $fps FPS)"
python3 tools/gpu_probes/trace_passes.py "out/profile/$label.mst.xml" "$frames" "${PASS_ARGS:---by-shader}" | tee "out/profile/$label.mst.txt" | head -${PASS_LINES:-45}
