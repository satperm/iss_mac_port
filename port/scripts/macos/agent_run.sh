#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# One unattended game run with a summary, for agents working without anyone at the Mac:
#
#   TLG_GAME_DIR=... bash port/scripts/macos/agent_run.sh <truck|bridge|<user dir>> <label> <seconds> [VAR=value ...]
#
# Never hangs: waits (bounded) for an earlier game to exit, the run itself is bounded by diag_run.sh's
# timeout, and this script gives up after <seconds> + 180 s whatever happens. Prints the median FPS of
# the last 40 s, crashes, compute-queue and fence stats. SHOT_SECONDS / PAD_SCRIPT pass through.
# Do not edit a script while a run of it is going: bash reads it as it runs (a summary was lost so).
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
# AGENT_CACHE: another shader cache dir (for switches the cache key does not cover).
cache=${AGENT_CACHE:-out/fps-ab-cache}
# AGENT_CACHE=<dir> may also come as an argument (lib_ab.sh variants: name:AGENT_CACHE=dir,...).
args=()
for a in "$@"; do [[ $a == AGENT_CACHE=* ]] && cache=${a#AGENT_CACHE=} || args+=("$a"); done
set -- ${args[@]+"${args[@]}"}
rm -rf "$user" && mkdir -p "$user" && cp -R "$template" "$user/" && mkdir -p "$cache" &&
    ln -s "$(cd "$cache" && pwd)" "$user/cache"
# The GPU's performance state during the run (macOS clocks it down for a window behind another app).
pstate=out/tools/gpu_pstate
[[ -x $pstate ]] || { mkdir -p out/tools; clang -O2 tools/gpu_probes/gpu_pstate.c -o "$pstate" -framework CoreFoundation -lIOReport 2>/dev/null || true; }
plog=out/agent-pstate-$label.txt
[[ -x $pstate ]] && { "$pstate" $((secs + 20)) > "$plog" 2>&1 & disown; }
# As players run it (owner, 2026-11-12): from inside ISS.app, full screen, window in front. Executables in
# the bundle make gamepolicyd turn Game Mode on (refresh_app.sh copies them after each build; the GPU
# library still comes from out/gpu-arm64 through the rpath, so lib_ab.sh swaps work). AGENT_APP=0: out/.
app_args=()
bundle=$PWD/../ISS.app/Contents/MacOS/tlg-probe
if [[ ${AGENT_APP:-1} != 0 && -x $bundle ]]; then
    app_args=(TLG_PROBE="$bundle" TLG_FULLSCREEN=1 TLG_RAISE_WINDOW=1)
fi
started=$(date '+%Y-%m-%d %H:%M:%S')
SHOT_SECONDS=${SHOT_SECONDS:-0} timeout $((secs + 180)) bash scripts/macos/diag_run.sh "a-$label" "$secs" \
    TLG_USER_DIR="$PWD/$user" TLG_HOST_INPUT=0 TLG_ACB_PACKETS=1 TLG_FENCE_LATENCY=1 ${app_args[@]+"${app_args[@]}"} "$@" >/dev/null 2>&1
echo "exit $?"
bash scripts/macos/wait_game.sh 120 || echo "WARNING: the game is still running after the run"
d=$(ls -td out/diag/*-a-"$label" | head -1)
echo "$label $d"
grep -aE 'Crash:|STOP:|Assertion|Device lost' "$d/game.log" | head -3 | cut -c1-200
grep -a 'Frame stats:' "$d/game.log" | awk -F'Frame stats: ' '{split($2,a," "); print a[1]}' | tail -8 | sort -n |
    awk '{v[NR]=$1} END{if (NR) printf "FPS median %.1f min %.1f max %.1f\n", v[int((NR+1)/2)], v[1], v[NR]; else print "FPS: no frame stats (did not reach gameplay)"}'
grep -a 'ACB queue (5' "$d/game.log" | tail -1 | cut -c1-160
grep -a 'Fence latency' "$d/game.log" | tail -2 | cut -c1-130
echo "Game Mode: $(/usr/bin/log show --start "$started" --style compact --predicate 'process == "gamepolicyd" && eventMessage CONTAINS "Game mode status is now on"' 2>/dev/null | grep -c 'now on') time(s) on"
[[ -s $plog ]] && echo "GPU clocked down (state < 3 of 15 at > 60% load): $(awk '{a=$(NF-5); gsub("%,","",a); if ($(NF-2)+0<3 && a+0>60) n++} END{print n+0}' "$plog") s"
pkill -x gpu_pstate 2>/dev/null || true
echo "memory (MiB): $(sed -n 2p "$d/memory.csv" | cut -d, -f4) -> $(tail -1 "$d/memory.csv" | cut -d, -f4)"
