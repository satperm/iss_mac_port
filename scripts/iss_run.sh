#!/usr/bin/env bash
# ISS: prepares the image, runs the game for N seconds and, optionally, presses buttons
# from a script and saves screenshots (raw BGRA display dumps converted to PNG).
# Usage: scripts/iss_run.sh [seconds=120] [shots_dir] [press_script]
#   press_script: lines "<second> <buttons...>" (e.g. "40 cross", "60 options"); each press lasts 0.3 s.
# Only one game instance may run at a time on this machine: the script waits for any other.
# ISS_GAME_DIR: the merged game folder (default game/CUSA00223, as in the launcher).
set -uo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
port=$root/port
seconds=${1:-120}; shots=${2:-}; script=${3:-}
dump=${ISS_GAME_DIR:-$root/game/CUSA00223}
# One game instance at a time on this machine (GAME_LOCK: a lock file shared with other runs).
lock=${GAME_LOCK:-${XDG_RUNTIME_DIR:-/tmp}/iss-game.lock}
if [[ -z ${ISS_RUN_LOCKED:-} ]]; then
    touch "$lock"; exec env ISS_RUN_LOCKED=1 flock "$lock" "$0" "$@"
fi
(cd "$port/scripts" &&
 python3 prepare.py "$dump" --out ../out >/dev/null &&
 python3 link_modules.py "$dump" --out ../out --modules libc.prx libSceFios2.prx >/dev/null &&
 python3 content_profile.py "$dump" --out ../out >/dev/null) || { echo "prepare failed" >&2; exit 1; }
# ISS_ISA_REWRITE=1 or a list of sets (e.g. sse4a): rewrite those instructions even on a CPU that has
# them, to test the emulation (needs iced-x86; ISA_PYTHON picks the Python that has it).
isa=()
if [[ -n ${ISS_ISA_REWRITE:-} && ${ISS_ISA_REWRITE} != 0 ]]; then
    rm -f "$port/out/isa-rewrite.bin"
    (cd "$port/scripts" && "${ISA_PYTHON:-python3}" isa_rewrite.py ../out/boot-linked.bin --out ../out) &&
        isa=(--isa-rewrite "$port/out/isa-rewrite.bin") || { echo "isa_rewrite.py failed" >&2; exit 1; }
fi
mkdir -p "$port/user" "$port/out/logs"
log=$port/out/logs/iss-$(date +%Y%m%d-%H%M%S).log
ln -sfn "$log" "$port/out/last_run.log"
export ISS_PIPELINE_BINARIES=0 ISS_READBACKS=${ISS_READBACKS:-2} ISS_EAGER_READBACK=${ISS_EAGER_READBACK:-0} ISS_WRITE_MERGE=${ISS_WRITE_MERGE:-1} ISS_GC_BUDGET_MB=${ISS_GC_BUDGET_MB:-3500} ISS_UPSCALER=off ISS_JITTER=0 \
       ISS_OBJECT_MOTION=0 ISS_FRAME_STATS=1 ISS_LANGUAGE=${ISS_LANGUAGE:-1}
pad=$port/out/pad_input.txt; : > "$pad"; export ISS_PAD_FILE=$pad
if [[ -n $shots ]]; then
    mkdir -p "$shots"; export ISS_FRAME_DUMP_TRIGGER=$shots/trigger ISS_DUMP_DIR=$shots
fi
export ISS_APP_NAME="ISS" ISS_APP_ID=iss-pc
(cd "$port/user" && exec "$port/out/tlg-probe" "$port/out/boot-linked.bin" --app0 "$dump" --user . \
    --content-profile "$port/out/content.bin" "${isa[@]}" --timeout "$seconds") > "$log" 2>&1 &
game=$!
start=$(date +%s)
if [[ -n $script && -f $script ]]; then
    while read -r at buttons; do
        [[ -z $at || $at == \#* ]] && continue
        while (( $(date +%s) - start < at )); do sleep 0.2; done
        kill -0 "$game" 2>/dev/null || break
        if [[ $buttons == shot ]]; then [[ -n $shots ]] && touch "$shots/trigger"; continue; fi
        echo "$buttons" > "$pad"; sleep 0.3; : > "$pad"
        echo "[$(( $(date +%s) - start ))s] $buttons"
    done < "$script"
fi
wait "$game"; code=$?
if [[ -n $shots ]]; then
    python3 - "$shots" <<'EOF'
import sys, re
from pathlib import Path
from PIL import Image
for raw in Path(sys.argv[1]).glob('display_*.raw'):
    m = re.search(r'_(\d+)x(\d+)\.raw$', raw.name); w, h = int(m[1]), int(m[2])
    d = raw.read_bytes()
    if len(d) >= w * h * 4:
        r, g, b, _ = Image.frombytes('RGBA', (w, h), d[:w*h*4]).split()  # the display buffer is BGRA
        Image.merge('RGB', (b, g, r)).save(raw.with_suffix('.png'))
EOF
fi
echo "exit $code; log: $log"
