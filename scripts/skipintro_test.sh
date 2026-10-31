#!/usr/bin/env bash
# Teste automatizado: menu -> New Game -> pausa na intro -> pular -> observa o início do gameplay.
# Uso: scripts/skipintro_test.sh [segundos_apos_pular=60] ; TOGGLES=<mascara> desliga otimizações do port.
set -uo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
after=${1:-60}
pad=$root/port/out/pad_input.txt; : > "$pad"; export TLG_PAD_FILE=$pad
if [[ -n ${TOGGLES:-} ]]; then echo "$TOGGLES" > "$root/port/out/toggles.txt"; export TLG_TOGGLE_FILE=$root/port/out/toggles.txt; fi
press() { echo "$1" > "$pad"; sleep "${2:-0.25}"; : > "$pad"; sleep 0.8; echo "[$(date +%T)] $1"; }
"$root/run_iss.sh" $((45 + 30 + after + 20)) > /dev/null 2>&1 &
game=$!
sleep 45; press up; press cross          # menu: New Game
sleep 25; press options; sleep 1.5       # intro: abre o menu de pausa
press cross; sleep 1.5; press cross      # "pular" + confirmação
wait "$game"; echo "fim (exit $?)"
