#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# A/B frame rate measurement: alternating runs of two or more variants at the benchmark scene (the
# truck crash, port/benchmark/savedata, standing still, no input), median FPS of the last 40 s each.
#
#   bash port/scripts/macos/fps_ab.sh [-n runs] [-s seconds] [-u user_dir_template] \
#       base "" fix "TLG_SOMETHING=1 MVK_FOO=0" ...
#
# Each variant is a label and a string of VAR=value settings (empty: defaults). Runs alternate
# (base, fix, base, fix, ...) so slow drift (thermals, background indexing) hits every variant alike.
# Needs TLG_GAME_DIR. Prints one line per run and a summary per variant.
#
# Rules of thumb (see AGENTS.md): >= 3 runs per variant; the spread between identical runs is 2-5%
# on a quiet Mac and up to 30% when Spotlight (mds), mediaanalysisd or fseventsd are busy (check
# `ps -Ao pcpu,comm -r | head`); the first run after a renderer change compiles shaders: discard it
# (-w 1 runs one warm-up first).
set -uo pipefail
cd -- "$(dirname -- "$0")/../.."
runs=3 secs=80 warm=0 template=benchmark
while getopts "n:s:w:u:" opt; do
    case $opt in
    n) runs=$OPTARG ;; s) secs=$OPTARG ;; w) warm=$OPTARG ;; u) template=$OPTARG ;;
    *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
(( $# >= 2 && $# % 2 == 0 )) || { sed -n 3,9p "$0"; exit 2; }
[[ -n ${TLG_GAME_DIR:-} ]] || { echo "Set TLG_GAME_DIR to the game folder." >&2; exit 2; }
bash scripts/macos/wait_game.sh 300 || { echo "The game is running: close it first." >&2; exit 1; }

# The benchmark save plus a shader cache kept between runs (out/fps-ab-cache).
user=out/fps-ab-user cache=out/fps-ab-cache
mkdir -p "$cache"
prepare() {
    rm -rf "$user" && mkdir -p "$user"
    if [[ $template == benchmark ]]; then cp -R benchmark/savedata "$user/"
    else cp -R "$template"/. "$user/" && rm -rf "$user/cache"; fi
    ln -s "$PWD/$cache" "$user/cache"
}
one() { # label, settings -> prints the median FPS
    local label=$1 settings=$2 dir
    prepare
    # shellcheck disable=SC2086 # the settings are separate words on purpose
    bash scripts/macos/wait_game.sh 120 >/dev/null
    SHOT_SECONDS=0 timeout $((secs + 180)) bash scripts/macos/diag_run.sh "ab-$label" "$secs" TLG_USER_DIR="$PWD/$user" \
        TLG_HOST_INPUT=0 $settings >/dev/null 2>&1
    dir=$(ls -td out/diag/*-ab-"$label" | head -1)
    grep -a 'Frame stats:' "$dir/game.log" | awk -F'Frame stats: ' '{split($2,a," "); print a[1]}' |
        tail -8 | sort -n | awk '{v[NR]=$1} END{if (NR) printf "%.1f", v[int((NR+1)/2)]; else printf "0"}'
    grep -qaE 'Crash:|STOP:' "$dir/game.log" && printf " (CRASH, see %s)" "$dir"
}

labels=() settings=()
while (( $# )); do labels+=("$1"); settings+=("$2"); shift 2; done
for ((w = 0; w < warm; ++w)); do echo "warm-up: $(one warm "${settings[0]}")"; done
declare -a results
for ((r = 1; r <= runs; ++r)); do
    for i in "${!labels[@]}"; do
        fps=$(one "${labels[$i]}" "${settings[$i]}")
        echo "run $r ${labels[$i]}: $fps FPS"
        results[$i]="${results[$i]:-} ${fps%% *}"
    done
done
echo "summary (median of $runs runs, $secs s each):"
for i in "${!labels[@]}"; do
    printf '  %-12s %s  [%s ]\n' "${labels[$i]}" \
        "$(tr ' ' '\n' <<<"${results[$i]}" | grep . | sort -n | awk '{v[NR]=$1} END{print v[int((NR+1)/2)]}')" \
        "${results[$i]}"
done
rm -rf "$user"
