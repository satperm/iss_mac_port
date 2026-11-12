#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# One unattended run started the way players start the game: through ISS.app (LaunchServices), its
# launcher hidden, full screen. agent_run.sh starts tlg-probe from a shell: macOS then sees anonymous,
# not-visible processes, and in Seattle the GPU dropped from P13 to P1 within a minute in many runs.
#
#   TLG_GAME_DIR=... bash port/scripts/macos/app_run.sh <truck|bridge|<user dir>> <label> <seconds> [VAR=value ...]
#
# Uses a copy of the save (out/agent-user), its own launch.conf (TLG_LAUNCH_CONF; the real one is never
# touched) and TLG_TIMEOUT. Prints the same summary lines as agent_run.sh. Bounded: gives up after
# <seconds> + 240 s. Quits the launcher afterwards. Never kills the game (Rosetta).
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
scene=$1 label=$2 secs=$3; shift 3
[[ -n ${TLG_GAME_DIR:-} ]] || { echo "Set TLG_GAME_DIR." >&2; exit 2; }
app=$PWD/../ISS.app
[[ -d $app ]] || { echo "No $app (scripts/macos/make_app.sh)." >&2; exit 2; }
bash scripts/macos/wait_game.sh 300 || exit 1
case $scene in
truck) template=benchmark/savedata ;;
bridge) template=out/bridge-fresh/savedata ;;
*) template=$scene/savedata ;;
esac
user=$PWD/out/agent-user
cache=${AGENT_CACHE:-out/fps-ab-cache}
rm -rf "$user" && mkdir -p "$user" && cp -R "$template" "$user/" && mkdir -p "$cache" &&
    ln -s "$(cd "$cache" && pwd)" "$user/cache"
dir=out/diag/$(date +%Y%m%d-%H%M%S)-app-$label
mkdir -p "$dir"
conf=$PWD/$dir/launch.conf
{
    echo "TLG_GAME_DIR='$TLG_GAME_DIR'"
    echo "TLG_SHOW_LAUNCHER=0"
    echo "TLG_FULLSCREEN=1"
    echo "TLG_STRETCH=1"
    echo "TLG_SHOW_FPS=0"
} > "$conf"
env_args=(--env "TLG_LAUNCH_CONF=$conf" --env "TLG_USER_DIR=$user" --env "TLG_TIMEOUT=$secs"
    --env "TLG_FRAME_STATS=1" --env "TLG_HOST_INPUT=0")
for a in "$@"; do env_args+=(--env "$a"); done
pstate=out/tools/gpu_pstate
plog=out/agent-pstate-app-$label.txt
logs=$HOME/Library/Logs/ISS
before=$(ls -t "$logs"/game-*.log 2>/dev/null | head -1)
open -n "${env_args[@]}" "$app"
# Wait (bounded) for the game to start, then for it to end.
for _ in $(seq 1 120); do pgrep -x tlg-probe >/dev/null && break; /bin/sleep 1; done
[[ -x $pstate ]] && { "$pstate" $((secs + 20)) > "$plog" 2>&1 & disown; }
end=$(( $(date +%s) + secs + 240 ))
while pgrep -x tlg-probe >/dev/null && (( $(date +%s) < end )); do /bin/sleep 2; done
pgrep -x tlg-probe >/dev/null && echo "WARNING: the game is still running after the run"
/bin/sleep 3
osascript -e 'tell application id "io.github.satperm.iss-mac-port" to quit' >/dev/null 2>&1 || true
log=$(ls -t "$logs"/game-*.log 2>/dev/null | head -1)
[[ -n $log && $log != "$before" ]] && cp "$log" "$dir/game.log" || echo "WARNING: no new session log in $logs"
echo "$label $dir"
[[ -f $dir/game.log ]] || exit 1
grep -aE 'Crash:|STOP:|Assertion|Device lost' "$dir/game.log" | head -3 | cut -c1-200
grep -a 'Frame stats:' "$dir/game.log" | awk -F'Frame stats: ' '{split($2,a," "); print a[1]}' | tail -8 | sort -n |
    awk '{v[NR]=$1} END{if (NR) printf "FPS median %.1f min %.1f max %.1f\n", v[int((NR+1)/2)], v[1], v[NR]; else print "FPS: no frame stats (did not reach gameplay)"}'
echo "FPS: $(grep -a 'Frame stats:' "$dir/game.log" | awk -F'Frame stats: ' '{split($2,a," "); printf "%s ", a[1]}')"
[[ -s $plog ]] && echo "GPU clocked down (state < 3 of 15 at > 60% load): $(awk '{a=$(NF-5); gsub("%,","",a); if ($(NF-2)+0<3 && a+0>60) n++} END{print n+0}' "$plog") s"
