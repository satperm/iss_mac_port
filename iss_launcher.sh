#!/usr/bin/env bash
# ISS launcher (pt_BR/en_US): game folder, languages, video, Play.
exec python3 "$(dirname -- "$(readlink -f -- "$0")")/port/launcher/iss_launcher.py" "$@"
