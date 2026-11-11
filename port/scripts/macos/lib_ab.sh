#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# A/B of GPU library builds and/or settings, alternating between runs (for changes no switch selects):
#
#   bash port/scripts/macos/lib_ab.sh save <name>      # keep the library just built as out/ab/<name>.dylib
#   TLG_GAME_DIR=... bash port/scripts/macos/lib_ab.sh run [-n runs] [-s seconds] <truck|bridge|user dir> \
#       <name>[:VAR=value[,VAR=value...]] [<name>[:...] ...]
#
# Typical: build the clean tree, `save base`; apply the change, build, `save fix`; `run truck base fix`.
# One saved build with different settings works too: `run truck base base:TLG_X=1`. No git stash anywhere.
# Each run is bounded (agent_run.sh). Lines go to stdout and to out/ab/last.txt as they come, so a result
# survives the caller being killed. The library that was in place before `run` is put back at the end.
# Prints per run: median FPS of the last 40 s and the GPU thread's us/draw; then the median per variant.
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
lib=out/gpu-arm64/libtlggpu.dylib
mkdir -p out/ab
case ${1:-} in
save)
    [[ -n ${2:-} ]] || { sed -n 3,9p "$0"; exit 2; }
    cp "$lib" "out/ab/$2.dylib" && echo "saved out/ab/$2.dylib ($(git rev-parse --short HEAD)$(git diff --quiet -- gpu || echo ' + changes'))"
    exit ;;
run) shift ;;
*) sed -n 3,9p "$0"; exit 2 ;;
esac
runs=3 secs=60
while getopts "n:s:" opt; do
    case $opt in n) runs=$OPTARG ;; s) secs=$OPTARG ;; *) exit 2 ;; esac
done
shift $((OPTIND - 1))
scene=${1:-}; shift || true
(( $# >= 1 )) && [[ -n $scene ]] || { sed -n 3,9p "$0"; exit 2; }
[[ -n ${TLG_GAME_DIR:-} ]] || { echo "Set TLG_GAME_DIR." >&2; exit 2; }
for v in "$@"; do [[ -f out/ab/${v%%:*}.dylib ]] || { echo "No out/ab/${v%%:*}.dylib: lib_ab.sh save ${v%%:*}" >&2; exit 2; }; done
bash scripts/macos/wait_game.sh 300 || exit 1
cp "$lib" out/ab/.before.dylib
trap 'cp out/ab/.before.dylib "$lib"' EXIT
out=out/ab/last.txt
say() { echo "$*" | tee -a "$out"; }
: > "$out"
say "lib_ab $(date '+%F %T') scene $scene, $runs x $secs s: $*"
declare -a results
for ((r = 1; r <= runs; ++r)); do
    i=0
    for v in "$@"; do
        name=${v%%:*} settings=""
        [[ $v == *:* ]] && settings=${v#*:}
        cp "out/ab/$name.dylib" "$lib"
        label=ab$i
        # shellcheck disable=SC2086 # the settings are separate words on purpose
        report=$(bash scripts/macos/agent_run.sh "$scene" "$label" "$secs" ${settings//,/ } 2>/dev/null)
        fps=$(awk '/FPS median/ {print $3}' <<<"$report")
        slow=$(awk '/GPU clocked down/ {print $(NF-1)}' <<<"$report")
        dir=$(ls -td out/diag/*-a-"$label" 2>/dev/null | head -1)
        us=$(grep -a 'Frame stats' "$dir/game.log" 2>/dev/null | tail -1 | grep -o 'GPU thread [0-9.]* us' | awk '{print $3}')
        crash=$(grep -acE 'Crash:|STOP:' "$dir/game.log" 2>/dev/null)
        busy=$(ps -Ao pcpu,comm -r | grep -v 'tlg' | awk 'NR==2 {print $1 " " $2}' | cut -c1-60)
        say "run $r $v: ${fps:-0} FPS, ${us:-?} us/draw, crash lines ${crash:-?}, GPU clocked down ${slow:-?} s, busiest other process: ${busy}"
        results[$i]="${results[$i]:-} ${fps:-0}"
        i=$((i + 1))
    done
done
say "summary (median of $runs):"
i=0
for v in "$@"; do
    say "$(printf '  %-40s %s  [%s ]' "$v" \
        "$(tr ' ' '\n' <<<"${results[$i]}" | grep . | sort -n | awk '{v[NR]=$1} END{print v[int((NR+1)/2)]}')" "${results[$i]}")"
    i=$((i + 1))
done
