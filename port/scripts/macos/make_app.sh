#!/usr/bin/env bash
# Builds ISS.app in the repository folder (as bloodborne_mac_port's make_app.sh): double-click or
# keep it in the Dock to play without a Terminal. Its main executable is the launcher
# (port/launcher, scripts/macos/build_launcher.sh): settings in port/launch.conf, the game through
# port/run_macos.sh, session logs in ~/Library/Logs/ISS. It never checks for updates.
#
# The game and GPU processes run from inside the bundle (copies of out/tlg-probe and
# out/tlggpu-host*, refreshed by build_macos.sh) and the bundle is a game (LSApplicationCategoryType,
# LSSupportsGameMode), so macOS Game Mode gives them CPU and GPU priority in full screen. A link into
# out/ does not count: macOS then sees an anonymous process outside the bundle, without Game Mode,
# and may clock the GPU down under it (49 -> 28 FPS in Seattle; tools/gpu_probes/gpu_pstate.c). The icon is made from the game's own sce_sys/icon0.png, so the app is
# built locally and never committed (ISS.app is in .gitignore).
#
# usage: bash port/scripts/macos/make_app.sh   (again after moving the repository)
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
repo=$PWD
app=$repo/ISS.app
conf=$repo/port/launch.conf
bash port/scripts/macos/build_launcher.sh
rm -rf "$app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
version=$(git describe --tags --always --dirty 2>/dev/null || echo dev)
cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>ISS</string>
    <key>CFBundleDisplayName</key><string>ISS</string>
    <key>CFBundleIdentifier</key><string>io.github.satperm.iss-mac-port</string>
    <key>CFBundleExecutable</key><string>ISS</string>
    <key>CFBundleIconFile</key><string>ISS</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1</string>
    <key>CFBundleVersion</key><string>$version</string>
    <key>NSHumanReadableCopyright</key><string>iss_mac_port contributors. GPL-2.0-or-later.</string>
    <key>LSMinimumSystemVersion</key><string>15.0</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.action-games</string>
    <key>LSSupportsGameMode</key><true/>
    <key>NSHighResolutionCapable</key><true/>
    <key>ISSRepo</key><string>$repo</string>
</dict>
</plist>
EOF
cp port/out/launcher/ISS "$app/Contents/MacOS/ISS"
bash port/scripts/macos/refresh_app.sh "$app"
# Icon from the game's own artwork.
game=$([[ -f $conf ]] && (set -a; source "$conf"; printf '%s' "${TLG_GAME_DIR:-}") || true)
if [[ -n $game && -f $game/sce_sys/icon0.png ]]; then
    set_dir=$(mktemp -d)/ISS.iconset
    mkdir -p "$set_dir"
    for size in 16 32 128 256 512; do
        sips -z $size $size "$game/sce_sys/icon0.png" --out "$set_dir/icon_${size}x${size}.png" >/dev/null
        sips -z $((size * 2)) $((size * 2)) "$game/sce_sys/icon0.png" --out "$set_dir/icon_${size}x${size}@2x.png" >/dev/null
    done
    iconutil -c icns "$set_dir" -o "$app/Contents/Resources/ISS.icns"
fi
codesign --force --sign - "$app" >/dev/null 2>&1 || true
echo "Built $app"
