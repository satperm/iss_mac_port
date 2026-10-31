#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds the native launcher (port/launcher, as bloodborne_mac_port's without the updater):
# out/launcher/ISS, ISS.app's main executable (make_app.sh).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
mkdir -p out/launcher
swiftc -O -target arm64-apple-macos15.0 launcher/Model/*.swift launcher/App/*.swift -o out/launcher/ISS
