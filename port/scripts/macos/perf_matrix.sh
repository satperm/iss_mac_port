#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Scene matrix (docs/PERF_STRATEGY.md "Scene matrix"): the same scenes, N runs of S seconds each, through
# the existing runners (agent_run.sh, lib_ab.sh, app_run.sh). One JSON line per run is appended to
# port/out/perf/history.jsonl (PERF_HISTORY=<file> overrides it).
#
#   TLG_GAME_DIR=/path/to/game bash port/scripts/macos/perf_matrix.sh [-n runs] [-s seconds] [-l label]
#       [--lib NAME | --app] [--dry-run] [truck|seattle|fire|bridge ...]
#
# Defaults: 3 runs x 60 s, all four scenes in that order, the bundle or library in place (agent_run.sh
# uses ISS.app when it is there).
#   truck, seattle   1080p (render_resolution=native)
#   fire, bridge     4K (render_resolution=4K)
#   fire = the burning village (owner's save copy), bridge = the bridge; both are user dirs with a
#   savedata/ inside, kept out of git. Folders relative to port/: seattle = out/play-seattle,
#   fire = out/owner-copy-20261114, bridge = out/bridge-fresh. Override with ISS_SCENE_SEATTLE,
#   ISS_SCENE_FIRE, ISS_SCENE_BRIDGE (absolute paths are fine). truck = benchmark/savedata (the runners' own).
# --lib NAME: a library saved with `lib_ab.sh save NAME`, run through `lib_ab.sh run` (one variant).
# --app: start through ISS.app the way players do (app_run.sh) instead of agent_run.sh.
# --dry-run: print the runs and their commands; start nothing.
# Each run's fps_median is the runners' own FPS median (the last 8 "Frame stats" lines = 40 s). perf holds the
# median of every key of the last 8 "Perf: key=value ..." lines of the run (absent when a run has none).
# --record LOG SCENE RES LABEL VARIANT RUN: append the line for one finished game.log (what the loop calls).
set -uo pipefail
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
history=${PERF_HISTORY:-$root/out/perf/history.jsonl}

record() { # LOG SCENE RESOLUTION LABEL VARIANT RUN
    mkdir -p "$(dirname -- "$history")"
    local git
    git=$(git -C "$root" describe --tags --always --dirty 2>/dev/null || echo unknown)
    python3 -I - "$1" "$2" "$3" "$4" "$5" "$6" "$git" "$history" <<'PY'
import datetime, json, os, re, sys
log, scene, res, label, variant, run, git, history = sys.argv[1:9]
try:
    text = open(log, errors='ignore').read().splitlines()
except OSError:
    text = []
def lower_median(values):  # the runners' median: sorted, element (n+1)//2 (1-based)
    values = sorted(values)
    return values[(len(values) + 1) // 2 - 1] if values else None
fps = [float(m.group(1)) for m in (re.search(r'Frame stats: ([\d.]+) FPS', l) for l in text) if m]
fps_tail = fps[-8:]
perf_lines = []
for line in text:
    m = re.search(r'\bPerf: (.*)$', line)
    if m:
        perf_lines.append([t for t in m.group(1).split() if '=' in t])
perf_tail = perf_lines[-8:]
series = {}
for tokens in perf_tail:
    for tok in tokens:
        key, _, value = tok.partition('=')
        try:
            number = float(value)
        except ValueError:
            continue
        series.setdefault(key, []).append(number)
perf = {k: round(lower_median(v), 4) for k, v in sorted(series.items())}
rec = {
    'date': datetime.datetime.now().astimezone().isoformat(timespec='seconds'),
    'git': git, 'label': label, 'variant': variant, 'scene': scene, 'resolution': res,
    'run': int(run), 'fps_median': lower_median(fps_tail), 'fps_samples': len(fps_tail),
    'perf_samples': len(perf_tail), 'perf': perf,
    'crash_lines': sum(1 for l in text if re.search(r'Crash:|STOP:', l)),
    'log': os.path.abspath(log) if text else None,
}
with open(history, 'a') as f:
    f.write(json.dumps(rec) + '\n')
print(f"{scene} run {run}: fps_median {rec['fps_median']}, fps_samples {len(fps_tail)}, "
      f"perf keys {len(perf)} ({len(perf_tail)} lines), crash lines {rec['crash_lines']}")
PY
}

if [[ ${1:-} == --record ]]; then shift; record "$@"; exit; fi

runs=3 secs=60 label=matrix lib= app=0 dry=0
scenes=()
usage() { sed -n 3,27p "$0"; exit 2; }
while (( $# )); do
    case $1 in
        -n) runs=${2:-}; shift 2 ;;
        -s) secs=${2:-}; shift 2 ;;
        -l) label=${2:-}; shift 2 ;;
        --lib) lib=${2:-}; shift 2 ;;
        --app) app=1; shift ;;
        --dry-run) dry=1; shift ;;
        truck|seattle|fire|bridge) scenes+=("$1"); shift ;;
        -h|--help) usage ;;
        *) echo "Unknown argument: $1" >&2; usage ;;
    esac
done
[[ $runs =~ ^[1-9][0-9]*$ && $secs =~ ^[1-9][0-9]*$ ]] || { echo "-n and -s take positive integers." >&2; exit 2; }
(( app )) && [[ -n $lib ]] && { echo "Use either --lib or --app." >&2; exit 2; }
(( ${#scenes[@]} )) || scenes=(truck seattle fire bridge)
[[ -n $label ]] || { echo "-l needs a label." >&2; exit 2; }
if [[ -n $lib && ! -f $root/out/ab/$lib.dylib ]]; then
    echo "No out/ab/$lib.dylib: lib_ab.sh save $lib" >&2; exit 2
fi
if (( ! dry )); then
    [[ -n ${TLG_GAME_DIR:-} ]] || { echo "Set TLG_GAME_DIR." >&2; exit 2; }
    command -v python3 >/dev/null || { echo "python3 is needed for the JSON." >&2; exit 2; }
fi
cd -- "$root"

scene_dir() {
    case $1 in
        truck) echo truck ;;
        seattle) echo "${ISS_SCENE_SEATTLE:-out/play-seattle}" ;;
        fire) echo "${ISS_SCENE_FIRE:-out/owner-copy-20261114}" ;;
        bridge) echo "${ISS_SCENE_BRIDGE:-out/bridge-fresh}" ;;
    esac
}
resolution() { case $1 in truck|seattle) echo 1080p ;; fire|bridge) echo 4K ;; esac; }

# Check every scene's save before the first run: a missing save must not cost a run.
mkdir -p out/perf
for s in "${scenes[@]}"; do
    dir=$(scene_dir "$s")
    if [[ $s == truck ]]; then
        [[ -d benchmark/savedata ]] || { echo "No benchmark/savedata in $root." >&2; exit 2; }
        continue
    fi
    case $dir in /*) ;; *) dir=$root/$dir ;; esac
    [[ -d $dir/savedata ]] || { echo "No $dir/savedata for $s (set ISS_SCENE_$(echo "$s" | tr a-z A-Z))." >&2; exit 2; }
done
# The render resolution of each class, as the game's own settings file (TLG_CONFIG): the owner's
# upscaler=off and live_resolution=0 are kept, render_resolution is the only difference.
for res in 1080p 4K; do
    case $res in 1080p) rr=native ;; 4K) rr=4K ;; esac
    printf 'render_resolution=%s\nlive_resolution=0\nupscaler=off\n' "$rr" > "out/perf/settings-$res.ini"
done

variant=${lib:-built}
stamp=$(date '+%Y%m%d-%H%M%S')
for s in "${scenes[@]}"; do
    res=$(resolution "$s")
    cfg=$root/out/perf/settings-$res.ini
    arg=$s
    if [[ $s != truck ]]; then
        arg=$(scene_dir "$s")
        case $arg in /*) ;; *) arg=$root/$arg ;; esac
    fi
    if [[ -n $lib ]]; then
        if (( dry )); then
            echo "would run: scripts/macos/lib_ab.sh run -n $runs -s $secs $arg $lib:TLG_CONFIG=$cfg"
            continue
        fi
        before=$(ls -d out/diag/*-a-ab0 2>/dev/null)
        bash scripts/macos/lib_ab.sh run -n "$runs" -s "$secs" "$arg" "$lib:TLG_CONFIG=$cfg" > "out/perf/lib_ab-$s-$stamp.txt" 2>&1
        after=$(ls -d out/diag/*-a-ab0 2>/dev/null)
        i=0
        # lib_ab labels every run ab0: the new session folders, oldest first, are the runs.
        for dir in $(printf '%s\n' "$after" | grep -vxF -f <(printf '%s\n' "$before")); do
            i=$((i + 1))
            record "$root/$dir/game.log" "$s" "$res" "$label" "$variant" "$i"
        done
        continue
    fi
    i=1
    while (( i <= runs )); do
        run_label="pm-$s-$i-$stamp"
        runner=scripts/macos/agent_run.sh
        (( app )) && runner=scripts/macos/app_run.sh
        if (( dry )); then
            echo "would run: $runner $arg $run_label $secs TLG_CONFIG=$cfg"
            i=$((i + 1)); continue
        fi
        report=$(bash "$runner" "$arg" "$run_label" "$secs" "TLG_CONFIG=$cfg" 2>/dev/null)
        printf '%s\n' "$report" > "out/perf/run-$run_label.txt"
        dir=$(awk -v l="$run_label" '$1 == l {print $2; exit}' <<<"$report")
        if [[ -z $dir || ! -f $dir/game.log ]]; then
            echo "$s run $i: no session log (see out/perf/run-$run_label.txt)" >&2
        else
            record "$root/$dir/game.log" "$s" "$res" "$label" "$variant" "$i"
        fi
        i=$((i + 1))
    done
done
