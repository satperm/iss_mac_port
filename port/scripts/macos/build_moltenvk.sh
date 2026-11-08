#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds MoltenVK v1.4.2 with patches/moltenvk-1.4.2-*.patch: drawable-race (MoltenVK issue #2791:
# _mtlDrawable released from two threads without a lock -> objc_msgSend on a freed drawable in
# Metal's present callbacks, ~1 start in 6-8 here) and installs it over deps/x86_64/lib.
# Needs Xcode. Takes ~15 minutes the first time (MoltenVK's own dependencies).
set -euo pipefail
cd "$(dirname "$0")/../.."
src=deps/src/MoltenVK-src
if [[ ! -d $src ]]; then
    git clone --depth 1 --branch v1.4.2 https://github.com/KhronosGroup/MoltenVK.git "$src"
fi
# patches/moltenvk-1.4.2-*.patch in name order: the drawable race fix, and concurrent compute
# encoders (MVK_TLG_CONCURRENT_DISPATCH=1, run_macos.sh) for the game's unordered compute queue.
for patch in "$PWD"/scripts/macos/patches/moltenvk-1.4.2-*.patch; do
    if ! (cd "$src" && git apply --check --reverse "$patch" 2>/dev/null); then
        (cd "$src" && git apply "$patch")
    fi
done
(cd "$src" && ./fetchDependencies --macos && make macos)
lib=deps/x86_64/lib/libMoltenVK.dylib
[[ -f $lib.orig-1.4.2 ]] || cp "$lib" "$lib.orig-1.4.2"
cp "$src/Package/Release/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib" "$lib"
echo "Installed the patched MoltenVK: $lib (original kept as $lib.orig-1.4.2)"
