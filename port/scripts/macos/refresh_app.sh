#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copies the freshly built game and GPU executables into a local ISS.app (make_app.sh, and
# build_macos.sh after each build), so they run from inside the bundle and macOS counts them as the
# game (Game Mode). usage: refresh_app.sh [path to ISS.app, default: the one in the repository folder]
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
app=${1:-$PWD/../ISS.app}
[[ -d $app/Contents/MacOS ]] || exit 0
for exe in tlg-probe tlggpu-host tlggpu-host-macos27; do
    [[ -f out/$exe ]] || continue
    rm -f "$app/Contents/MacOS/$exe"   # may be an old link into out/
    cp "out/$exe" "$app/Contents/MacOS/$exe"
    codesign --force --sign - "$app/Contents/MacOS/$exe" >/dev/null 2>&1 || true
done
codesign --force --sign - "$app" >/dev/null 2>&1 || true
