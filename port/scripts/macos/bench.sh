#!/usr/bin/env bash
# Fixed benchmark (macOS): the same save and scripted route every time, so changes can be compared.
#
#   TLG_GAME_DIR=/path/to/game bash port/scripts/macos/bench.sh <label> [VAR=value ...]
#
# Copies port/user (the save, without caches) to port/out/bench-user, keeps a shader cache between
# runs in port/out/bench-cache (delete it after shader changes), plays scripts/macos/pad/powers.txt
# for 170 s with diag_run.sh and prints average FPS per phase and the memory peak:
#   interior idle = the median of the last 60 s (the hero standing in the fish cannery).
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
label=${1:?usage: bench.sh <label> [VAR=value ...]}; shift
user=$PWD/out/bench-user
rm -rf "$user" && mkdir -p "$user"
for x in user/*; do
    case $(basename "$x") in cache|shader|gpu-*) ;; *) cp -R "$x" "$user/";; esac
done
# BENCH_FRESH=1: no shader cache (switches read when shaders are built); the run does not save one.
# BENCH_CACHE=<name>: a cache of its own (out/bench-cache-<name>) for such a switch: run twice and
# compare the second run.
cache=out/bench-cache${BENCH_CACHE:+-$BENCH_CACHE}
[[ -d $cache && -z ${BENCH_FRESH:-} ]] && cp -R "$cache" "$user/cache"
# No input by default: the save's cutscene, then the hero standing in the fish cannery, the same
# every time (the scripted route lands in different places with load-time differences;
# BENCH_PAD=1 plays it anyway). The Mac's own gamepads and keyboard are ignored (TLG_HOST_INPUT=0).
# Cool-down: long series heat the Mac and throttle it. Before each run, wait (at most
# BENCH_COOL_MAX seconds, 300) until macOS reports the nominal thermal state; the result line
# names the state before and after the run (0 nominal, 1 fair, 2 serious, 3 critical).
therm=out/thermal_state
[[ -x $therm ]] || swiftc -O scripts/macos/thermal_state.swift -o "$therm" 2>/dev/null || therm=
thermal() { [[ -n $therm ]] && "$therm" || echo '?'; }
waited=0
while [[ $(thermal) != 0 && $(thermal) != '?' && $waited -lt ${BENCH_COOL_MAX:-300} ]]; do
    sleep 10; waited=$((waited + 10))
done
therm_before=$(thermal)
pad=()
[[ -n ${BENCH_PAD:-} ]] && pad=(PAD_SCRIPT=scripts/macos/pad/powers.txt)
env ${pad[@]+"${pad[@]}"} SHOT_SECONDS=${SHOT_SECONDS:-30} \
    bash scripts/macos/diag_run.sh "bench-$label" 170 TLG_USER_DIR="$user" TLG_HOST_INPUT=0 "$@" > /dev/null 2>&1
dir=$(ls -td out/diag/*-bench-"$label" | head -1)
if [[ -z ${BENCH_FRESH:-} && -d $user/cache ]]; then rm -rf "$cache" && cp -R "$user/cache" "$cache"; fi
# FPS lines come every 5 s from the start of gameplay frames (diag_run.sh's report).
therm_after=$(thermal)
python3 - "$dir" "$label" "$therm_before/$therm_after, cooled ${waited} s" <<'EOF'
import re, sys
d, label, therm = sys.argv[1], sys.argv[2], sys.argv[3]
fps = [float(m.group(1)) for m in re.finditer(r'Frame stats: ([\d.]+) FPS', open(d + '/game.log', errors='ignore').read())]
def avg(a, b):
    xs = fps[a // 5:b // 5]
    return sum(xs) / len(xs) if xs else 0
rep = open(d + '/report.txt').read()
mem = re.search(r'memory peak: (\S+)', rep)
hang = re.search(r'GPU hangs / lost device: (\d+)', rep)
tail = sorted(fps[-12:])
median = tail[len(tail) // 2] if tail else 0
mean = sum(tail) / len(tail) if tail else 0
spread = (sum((x - mean) ** 2 for x in tail) / len(tail)) ** 0.5 if tail else 0
print(f"{label}: interior idle {median:.1f} FPS (median of the last 60 s, mean {mean:.1f} +- {spread:.1f}; cutscene {avg(30, 90):.1f}), "
      f"memory peak {mem.group(1) if mem else '?'}, hangs {hang.group(1) if hang else '?'}, thermal {therm}  ({d})")
EOF
