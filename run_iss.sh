#!/usr/bin/env bash
# Runs ISS from a terminal with the launcher settings: ./run_iss.sh [seconds]
exec python3 "$(dirname -- "$(readlink -f -- "$0")")/port/launcher/iss_run.py" "$@"
