#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds port/dist/ISS.app and port/dist/ISS-macOS-<version>.zip: a self-contained app for release downloads
# (unlike make_app.sh, which makes a launcher for this checkout), after bloodborne_mac_port's
# package_app.sh. Contains the built port (x86-64 game process, arm64 GPU process), universal
# libraries with the patched MoltenVK, the launch scripts and a standalone Python for them. No game
# files and no game artwork: the player picks their own game folder. Settings, saves and caches go
# to ~/Library/Application Support/ISS. Ad-hoc signed, not notarised: downloaded copies are
# quarantined, see the README.
#
# usage: bash port/scripts/macos/package_app.sh [version]     (build first: cd port && bash build_macos.sh)
#
# ISS.app/Contents:
#   MacOS/ISS, tlg-probe, tlggpu-host, tlggpu-host-macos27
#   Frameworks/*.dylib     universal
#   Resources/port/        run_macos.sh, scripts, tools/xpps, patches, the MoltenVK ICD file
#   Resources/python/      standalone CPython (arm64) with iced-x86 for the scripts
#   Resources/licenses/
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
root=$PWD
version=${1:-$(git -C .. describe --tags --always 2>/dev/null || echo dev)}

# Standalone Python (python-build-standalone, PSF licence), pinned as in bloodborne_mac_port.
py_release=20260924
py_name=cpython-3.12.14+20260924-aarch64-apple-darwin-install_only_stripped.tar.gz
py_sha256=c2edb321cd32ec2b170df208db0446dccc4398db602ca27cf2079098fb1f7d9d
iced=iced-x86==1.22.0

for f in out/tlg-probe out/tlggpu-host out/tlggpu-host-macos27 out/gpu/libtlggpu.dylib \
         out/gpu-arm64/libtlggpu.dylib out/launcher/ISS; do
    [[ -f $f ]] || { echo "Missing $f: build first (build_macos.sh, build_launcher.sh)." >&2; exit 1; }
done

dist=$root/dist
mkdir -p "$dist"
dist=$(cd "$dist" && pwd)
app=$dist/ISS.app
contents=$app/Contents
rm -rf "${app:?}"
mkdir -p "$contents/MacOS" "$contents/Frameworks" "$contents/Resources/port/scripts" \
    "$contents/Resources/port/tools" "$contents/Resources/licenses"

install -m755 out/launcher/ISS out/tlg-probe out/tlggpu-host out/tlggpu-host-macos27 "$contents/MacOS/"
set_rpath() { # file new-rpath
    local file=$1 path
    while read -r path; do
        install_name_tool -delete_rpath "$path" "$file" 2>/dev/null || true
    done < <(otool -l "$file" | awk '/cmd LC_RPATH/ { getline; getline; print $2 }')
    install_name_tool -add_rpath "$2" "$file"
}
for exe in tlg-probe tlggpu-host tlggpu-host-macos27; do
    set_rpath "$contents/MacOS/$exe" @executable_path/../Frameworks
done

# One universal file each: the x86-64 slice for tlg-probe, arm64 for the GPU process.
universal() { # name x86_64-file arm64-file
    lipo -create "$2" "$3" -output "$contents/Frameworks/$1"
    chmod 755 "$contents/Frameworks/$1"
}
universal libtlggpu.dylib out/gpu/libtlggpu.dylib out/gpu-arm64/libtlggpu.dylib
set_rpath "$contents/Frameworks/libtlggpu.dylib" @loader_path
for lib in libvulkan.1 libSDL3.0 libavformat.61 libavcodec.61 libswscale.8 libswresample.5 libavutil.59; do
    universal "$lib.dylib" "deps/x86_64/lib/$lib.dylib" "deps/arm64/lib/$lib.dylib"
done
install -m755 deps/x86_64/lib/libMoltenVK.dylib "$contents/Frameworks/"   # already universal, patched
# Every @rpath dependency must be in the bundle, and nothing may link outside it and the system.
for file in "$contents"/MacOS/* "$contents"/Frameworks/*; do
    for arch in x86_64 arm64; do
        lipo -info "$file" | grep -q "$arch" || continue
        while read -r dep; do
            [[ -f $contents/Frameworks/${dep#@rpath/} ]] || { echo "$file ($arch) needs $dep, not bundled" >&2; exit 1; }
        done < <(otool -arch "$arch" -L "$file" | awk 'NR > 1 && $1 ~ /^@rpath\// { print $1 }')
        if otool -arch "$arch" -L "$file" | awk 'NR > 1 { print $1 }' | grep -vE '^(@rpath/|@executable_path/|@loader_path/|/System/|/usr/lib/)'; then
            echo "$file ($arch) links a library outside the bundle and the system" >&2; exit 1
        fi
    done
done

# Launch scripts (run_macos.sh runs them; the launcher sets TLG_PREBUILT=1, so nothing builds).
port=$contents/Resources/port
install -m755 run_macos.sh "$port/"
install -m644 scripts/*.py "$port/scripts/"
# The benchmark save (launcher "Benchmark": the truck crash, standing still).
if [[ -d benchmark/savedata ]]; then
    mkdir -p "$port/benchmark" && cp -R benchmark/savedata "$port/benchmark/"
fi
mkdir -p "$port/scripts/macos"
install -m644 scripts/macos/prepare_shader_jobs.py "$port/scripts/macos/"
cp -R tools/xpps "$port/tools/xpps"
if [[ -d patches ]]; then cp -R patches "$port/patches"; fi
find "$port" -name __pycache__ -type d -prune -exec rm -rf {} +
icd=$port/deps/x86_64/share/vulkan/icd.d
mkdir -p "$icd"
cat > "$icd/MoltenVK_icd.json" <<'EOF'
{"file_format_version": "1.0.0", "ICD": {"library_path": "../../../../../../../Frameworks/libMoltenVK.dylib", "api_version": "1.4.0", "is_portability_driver": true}}
EOF

# Python, with iced-x86 for the ISA rewrite (installed before pip goes).
cache=$root/deps/cache
mkdir -p "$cache"
if [[ ! -f $cache/$py_name ]]; then
    curl -fsSL --retry 3 -o "$cache/$py_name.part" \
        "https://github.com/astral-sh/python-build-standalone/releases/download/$py_release/${py_name//+/%2B}"
    mv "$cache/$py_name.part" "$cache/$py_name"
fi
echo "$py_sha256  $cache/$py_name" | shasum -a 256 -c - >/dev/null || { echo "Python archive checksum mismatch" >&2; exit 1; }
tar -xzf "$cache/$py_name" -C "$contents/Resources"
py=$contents/Resources/python
stdlib=$py/lib/python3.12
PYTHONDONTWRITEBYTECODE=1 "$py/bin/python3" -m pip install --quiet --no-compile --disable-pip-version-check \
    --only-binary=:all: --target "$stdlib/site-packages" "$iced"
rm -rf "${stdlib:?}"/site-packages/pip* "${py:?}/include" "${py:?}/share" "${py:?}/bin/idle"* "${py:?}/bin/pip"* \
    "${py:?}/bin/pydoc"* "${py:?}/bin/2to3"* "${py:?}/bin/python3-config" "${py:?}/bin/python3.12-config" \
    "${py:?}"/lib/libtcl* "${py:?}"/lib/libtk* "${py:?}"/lib/tcl* "${py:?}"/lib/tk* "${py:?}"/lib/itcl* \
    "${py:?}"/lib/thread* "${py:?}/lib/pkgconfig"
rm -rf "${stdlib:?}"/{test,idlelib,tkinter,turtledemo,ensurepip,lib2to3,pydoc_data} "${stdlib:?}"/config-3.12-darwin \
    "${stdlib:?}"/lib-dynload/_tkinter* "${stdlib:?}"/lib-dynload/_test* "${stdlib:?}"/lib-dynload/_ctypes_test* \
    "${stdlib:?}"/lib-dynload/xxlimited*
find "$py" -name __pycache__ -type d -prune -exec rm -rf {} +
PYTHONDONTWRITEBYTECODE=1 "$py/bin/python3" -c 'import argparse, hashlib, json, multiprocessing, sqlite3, struct, iced_x86' \
    || { echo "The bundled Python lacks a module the scripts use" >&2; exit 1; }

# Licences.
lic=$contents/Resources/licenses
cp LICENSE "$lic/LICENSE-iss-mac-port.txt"
cp "$stdlib/LICENSE.txt" "$lic/LICENSE-Python.txt"
for src in deps/src/*/ gpu/third_party/*/; do
    name=$(basename "$src")
    for file in LICENSE LICENSE.txt LICENSE.md COPYING.LGPLv2.1 NOTICE.md; do
        if [[ -f $src/$file ]]; then cp "$src/$file" "$lic/LICENSE-$name-$file"; fi
    done
done

cat > "$contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>ISS</string>
    <key>CFBundleDisplayName</key><string>ISS</string>
    <key>CFBundleIdentifier</key><string>io.github.satperm.iss-mac-port</string>
    <key>CFBundleExecutable</key><string>ISS</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$version</string>
    <key>CFBundleVersion</key><string>$version</string>
    <key>NSHumanReadableCopyright</key><string>iss_mac_port contributors. GPL-2.0-or-later.</string>
    <key>LSMinimumSystemVersion</key><string>15.0</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.action-games</string>
    <key>LSSupportsGameMode</key><true/>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
EOF

# Inner code first, then the app: the changed rpaths invalidated the build's signatures.
find "$py" -type f \( -name '*.so' -o -name '*.dylib' \) -exec codesign --force --sign - {} + >/dev/null
codesign --force --sign - "$py/bin/python3.12" >/dev/null 2>&1 || true
for file in "$contents"/Frameworks/* "$contents"/MacOS/tlg*; do codesign --force --sign - "$file" >/dev/null 2>&1; done
codesign --force --sign - "$app" >/dev/null
codesign --verify --deep --strict "$app" || { echo "The signature does not verify" >&2; exit 1; }

# No game data may ship: this repository is public and so are its releases. The one save allowed is the
# benchmark's (an autosave at the truck crash, without the game's icon).
if find "$app" \( -name 'eboot.bin' -o -name '*.xpps' -o -name 'shader-jobs.bin' -o -name '*.spv' -o -name 'icon0.png' \) | grep . ||
   find "$app" -name 'savedata' ! -path "$port/benchmark/savedata" | grep .; then
    echo "Game data in the bundle" >&2; exit 1
fi

zip=$dist/ISS-macOS-$version.zip
rm -f "$zip"
ditto -c -k --sequesterRsrc --keepParent "$app" "$zip"
du -sh "$app" "$zip"
echo "Built $zip"
