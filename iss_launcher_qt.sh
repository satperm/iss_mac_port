#!/usr/bin/env bash
# ISS launcher, Qt Quick edition (the default launcher of the packages; iss_launcher.sh is the Tk one, kept as a fallback).
# Usa o venv ~/.../.venv-qt ou o python do sistema com PySide6-Essentials instalado.
here="$(dirname -- "$(readlink -f -- "$0")")"
py=python3
[ -x "$here/.venv-qt/bin/python" ] && py="$here/.venv-qt/bin/python"
exec "$py" "$here/port/launcher/iss_launcher_qt.py" "$@"
