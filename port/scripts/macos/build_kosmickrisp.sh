#!/usr/bin/env bash
# Builds LunarG's KosmicKrisp (Mesa's Vulkan-on-Metal driver) for x86-64 into deps/kosmickrisp, as an
# alternative to MoltenVK (run_macos.sh: TLG_VULKAN_DRIVER=kosmickrisp). LunarG ships it for Apple
# Silicon only; tlg-probe runs under Rosetta, so it is cross-built here: Mesa's shader tools
# (mesa_clc, kk_clc) natively for arm64 first, then the driver for x86-64 with those tools.
# Needs macOS 26 (Metal 4) and arm64 Homebrew: meson llvm libclc spirv-llvm-translator bison flex.
# 2026-10-28, Mesa 26.2.5, cannery benchmark: 19.2 FPS (MoltenVK 28.2): the driver's per-draw
# work on the recording thread (kk_draw, render encoders) is the limit under Rosetta.
#
#   bash port/scripts/macos/build_kosmickrisp.sh
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
version=${MESA_VERSION:-26.2.5}
root=$PWD/deps/kosmickrisp
src=$PWD/deps/src/mesa-$version
build=$PWD/deps/build-kosmickrisp
mkdir -p "$PWD/deps/src" "$build"
if [[ ! -d $src ]]; then
    curl -fsSL "https://archive.mesa3d.org/mesa-$version.tar.xz" | tar xJ -C "$PWD/deps/src"
fi
[[ -x $build/venv/bin/python ]] || { python3 -m venv "$build/venv" && "$build/venv/bin/pip" -q install mako pyyaml packaging; }
brew=$(brew --prefix)
export PATH=$build/tools/bin:$build/venv/bin:$brew/opt/bison/bin:$brew/opt/flex/bin:$PATH
common=(-Dvulkan-drivers=kosmickrisp -Dgallium-drivers= -Dglx=disabled -Degl=disabled -Dgles1=disabled
    -Dgles2=disabled -Dopengl=false -Dbuildtype=release)
if [[ ! -x $build/tools/bin/kk_clc ]]; then
    PATH=$brew/opt/llvm/bin:$PATH PKG_CONFIG_PATH=$brew/opt/llvm/lib/pkgconfig:$brew/lib/pkgconfig \
        meson setup --wipe "$build/native" "$src" -Dprefix="$build/tools" -Dplatforms= -Dllvm=enabled \
        -Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dprecomp-compiler=enabled \
        -Dinstall-precomp-compiler=true "${common[@]}" >/dev/null
    PATH=$brew/opt/llvm/bin:$PATH ninja -C "$build/native" install >/dev/null
fi
flags="'-arch', 'x86_64', '-mmacosx-version-min=15.0'"
cat > "$build/x86_64.cross" <<CROSS
[binaries]
c = ['clang', $flags]
cpp = ['clang++', $flags]
objc = ['clang', $flags]
objcpp = ['clang++', $flags]
ar = 'ar'
strip = 'strip'
pkg-config = 'false'
[properties]
needs_exe_wrapper = false
[host_machine]
system = 'darwin'
subsystem = 'macos'
kernel = 'xnu'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
CROSS
env -u PKG_CONFIG_PATH meson setup --wipe "$build/x86_64" "$src" --cross-file "$build/x86_64.cross" \
    -Dprefix="$root" -Dplatforms=macos -Dllvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system \
    -Dzstd=disabled -Dexpat=disabled "${common[@]}" >/dev/null
ninja -C "$build/x86_64" install >/dev/null
echo "KosmicKrisp: $root/share/vulkan/icd.d/kosmickrisp_mesa_icd.x86_64.json"
