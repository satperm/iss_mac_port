#!/usr/bin/env bash
# Unattended diagnostic run (macOS): starts the game through run_macos.sh for a fixed time with
# frame statistics, samples its memory, and writes a short report next to the full log, so a
# change can be measured without anyone at the Mac.
#
#   TLG_GAME_DIR=/path/to/game bash port/scripts/macos/diag_run.sh <label> [seconds] [VAR=value ...]
#
# Output: port/out/diag/<time>-<label>/{game.log,memory.csv,report.txt}; the report is printed.
# Scripted input: PAD_SCRIPT=<file> plays "<seconds since start> <pad tokens>" lines into the
# game's TLG_PAD_FILE (src/runtime_pad.c; example: scripts/macos/pad/graffiti_walk.txt).
# Frame dumps: shots/*.jpg every SHOT_SECONDS (default 5; SHOT_SECONDS=0: none).
set -uo pipefail
[[ -n ${PAD_SCRIPT:-} ]] && PAD_SCRIPT=$(cd -- "$(dirname -- "$PAD_SCRIPT")" && pwd)/$(basename -- "$PAD_SCRIPT")
cd -- "$(dirname -- "$0")/../.."
# Tests start from a copied save: no first-start compile unless asked (TLG_PRECOMPILE=1).
export TLG_PRECOMPILE=${TLG_PRECOMPILE:-0}
label=${1:?usage: diag_run.sh <label> [seconds] [VAR=value ...]}; shift
seconds=60
if [[ ${1:-} =~ ^[0-9]+$ ]]; then seconds=$1; shift; fi
dir=out/diag/$(date +%Y%m%d-%H%M%S)-$label
mkdir -p "$dir"
log=$dir/game.log
extra=()
if [[ ${SHOT_SECONDS:-5} != 0 ]]; then
    extra+=(TLG_SHOT_DIR="$PWD/$dir/shots" TLG_SHOT_SECONDS="${SHOT_SECONDS:-5}")
fi
if [[ -n ${PAD_SCRIPT:-} ]]; then
    pad=$PWD/$dir/pad.txt
    : > "$pad"
    extra+=(TLG_PAD_FILE="$pad")
fi
env TLG_PREBUILT=${TLG_PREBUILT:-1} TLG_FRAME_STATS=1 TLG_FULLSCREEN=0 TLG_STRETCH=0 ${extra[@]+"${extra[@]}"} "$@" \
    timeout "$seconds" bash run_macos.sh > "$log" 2>&1 &
runner=$!
if [[ -n ${PAD_SCRIPT:-} ]]; then
    # Each line's tokens replace the pad file's at its time (the game re-reads it on change).
    (
        start=$(date +%s)
        grep -v '^#' "$PAD_SCRIPT" | while read -r at tokens; do
            [[ -z $at ]] && continue
            wait_s=$((at - ($(date +%s) - start)))
            ((wait_s > 0)) && sleep "$wait_s"
            kill -0 $runner 2>/dev/null || break
            printf '%s\n' "$tokens" > "$pad"
            echo "$at ${tokens:-(release)}" >> "$PWD/$dir/pad.log"
        done
    ) &
fi
echo "seconds,mem,compressed,footprint_mib" > "$dir/memory.csv"
for ((t = 5; t <= seconds; t += 5)); do
    sleep 5
    pid=$(pgrep -x 'tlg-probe(-v2)?' | head -1)
    [[ -z $pid ]] && { kill -0 $runner 2>/dev/null || break; continue; }
    read -r mem compressed < <(top -l 1 -pid "$pid" -stats mem,cmprs 2>/dev/null | tail -1)
    footprint=$(python3 scripts/macos/footprint.py "$pid" 2>/dev/null || echo 0)
    # The native GPU process (TLG_GPU_PROCESS=1) holds the renderer's memory: counted with the game.
    host_pid=$(pgrep -f 'out/tlggpu-host' | head -1)
    if [[ -n $host_pid ]]; then
        footprint=$((footprint + $(python3 scripts/macos/footprint.py "$host_pid" 2>/dev/null || echo 0)))
    fi
    echo "$t,$mem,$compressed,$footprint" >> "$dir/memory.csv"
done
wait $runner; status=$?
if [[ -d $dir/shots ]]; then
    for f in "$dir"/shots/*.ppm; do
        [[ -e $f ]] && sips -s format jpeg -Z "${SHOT_SIZE:-960}" "$f" --out "${f%.ppm}.jpg" >/dev/null 2>&1 && rm -f "$f"
    done
fi
{
    echo "run: $label, ${seconds} s, exit $status ($( ((status == 124)) && echo 'ran until the time limit' || echo 'stopped early'))"
    echo "FPS: $(grep 'Frame stats:' "$log" | awk -F'Frame stats: ' '{split($2,a," "); printf "%s ", a[1]}')"
    grep '^Stall:' "$log" | sed -E 's/.*GPU ticks ([0-9.]+) ms.*/\1/' |
        awk '{s+=$1; n++; if ($1>m) m=$1} END {if (n) printf "GPU stalls: %d, average %.0f ms, worst %.0f ms\n", n, s/n, m; else print "GPU stalls: none"}'
    echo "memory peak: $(tail -n +2 "$dir/memory.csv" | cut -d, -f2 | sort -h | tail -1) ($(tail -n +2 "$dir/memory.csv" | cut -d, -f4 | sort -n | tail -1) MiB footprint; samples in memory.csv)"
    hangs=$(grep -c 'GPU Hang\|Lost VkDevice' "$log")
    echo "GPU hangs / lost device: $hangs"
    if ((hangs)); then
        first=$(grep -n -m1 'GPU Hang\|Lost VkDevice' "$log" | cut -d: -f1)
        echo "--- before the first hang (log line $first):"
        sed -n "$((first > 12 ? first - 12 : 1)),${first}p" "$log" | grep -v '^Runtime: semaphore id=' | cut -c1-200
    fi
    echo "frame dumps: $(ls "$dir"/shots/*.jpg 2>/dev/null | wc -l | tr -d ' ') in $dir/shots"
    echo "--- fatal lines:"
    grep -E '^STOP|Guest fault|Host fault|^Fault|Assertion Failed' "$log" | head -5 | cut -c1-200
    echo "--- MoltenVK errors (count, first seen):"
    grep -E 'mvk-error' "$log" | sed -E 's/0x[0-9a-f]+/0x?/g' | sort | uniq -c | sort -rn | head -5 | cut -c1-200
} > "$dir/report.txt"
cat "$dir/report.txt"
echo "full log: $PWD/$log"
