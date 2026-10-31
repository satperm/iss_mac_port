#!/usr/bin/env bash
# Teste automatizado: inicia o jogo, espera o menu inicial, seleciona New Game e observa por N segundos.
# Entrada roteirizada via TLG_PAD_FILE (runtime_pad.c): o arquivo lista os botões mantidos apertados.
# Uso: scripts/newgame_test.sh [segundos_de_jogo=120] [espera_menu=45]
set -uo pipefail
root=$(cd -- "$(dirname -- "$0")/.." && pwd)
play=${1:-120}; menu_wait=${2:-45}
pad=$root/port/out/pad_input.txt
: > "$pad"
export TLG_PAD_FILE=$pad
press() { echo "$1" > "$pad"; sleep "${2:-0.25}"; : > "$pad"; sleep 0.6; echo "[$(date +%T)] apertou $1"; }
"$root/run_iss.sh" $((menu_wait + play + 15)) > /dev/null 2>&1 &
game=$!
sleep "$menu_wait"
press up; press cross
sleep 6; press cross      # eventual confirmação ("iniciar novo jogo?") ou aviso de autosave
sleep 6; press cross
wait "$game"
echo "fim (exit $?); log: $root/port/out/last_run.log"
