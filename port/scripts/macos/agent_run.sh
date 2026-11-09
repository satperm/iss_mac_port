#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# One unattended game run with a summary, for agents working without anyone at the Mac:
#
#   TLG_GAME_DIR=... bash port/scripts/macos/agent_run.sh <truck|bridge|<user dir>> <label> <seconds> [VAR=value ...]
#
# Never hangs: waits (bounded) for an earlier game to exit, the run itself is bounded by diag_run.sh's
# timeout, and this script gives up after <seconds> + 180 s whatever happens. Prints the median FPS of
# the last 40 s, crashes, compute-queue and fence stats. SHOT_SECONDS / PAD_SCRIPT pass through.
# bridge = out/bridge-fresh (a local copy of a save on the bridge; not in git).
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
scene=$1 label=$2 secs=$3; shift 3
[[ -n ${TLG_GAME_DIR:-} ]] || { echo "Set TLG_GAME_DIR." >&2; exit 2; }
bash scripts/macos/wait_game.sh 300 || exit 1
case $scene in
truck) template=benchmark/savedata ;;
bridge) template=out/bridge-fresh/savedata ;;
*) template=$scene/savedata ;;
esac
user=out/agent-user
rm -rf "$user" && mkdir -p "$user" && cp -R "$template" "$user/" && mkdir -p out/fps-ab-cache &&
    ln -s "$PWD/out/fps-ab-cache" "$user/cache"
SHOT_SECONDS=${SHOT_SECONDS:-0} timeout $((secs + 180)) bash scripts/macos/diag_run.sh "a-$label" "$secs" \
    TLG_USER_DIR="$PWD/$user" TLG_HOST_INPUT=0 TLG_ACB_PACKETS=1 TLG_FENCE_LATENCY=1 "$@" >/dev/null 2>&1
echo "exit $?"
bash scripts/macos/wait_game.sh 120 || echo "WARNING: the game is still running after the run"
d=$(ls -td out/diag/*-a-"$label" | head -1)
echo "$label $d"
grep -aE 'Crash:|STOP:|Assertion|Device lost' "$d/game.log" | head -3 | cut -c1-200
grep -a 'Frame stats:' "$d/game.log" | awk -F'Frame stats: ' '{split($2,a," "); print a[1]}' | tail -8 | sort -n |
    awk '{v[NR]=$1} END{if (NR) printf "FPS median %.1f min %.1f max %.1f\n", v[int((NR+1)/2)], v[1], v[NR]; else print "FPS: no frame stats (did not reach gameplay)"}'
grep -a 'ACB queue (5' "$d/game.log" | tail -1 | cut -c1-160
grep -a 'Fence latency' "$d/game.log" | tail -2 | cut -c1-130
echo "memory (MiB): $(sed -n 2p "$d/memory.csv" | cut -d, -f4) -> $(tail -1 "$d/memory.csv" | cut -d, -f4)"
