#!/usr/bin/env bash
# Adapted from bloodborne_mac_port's scripts/macos/bootstrap_deps.sh (Mark Hall, GPL-2.0-or-later,
# before its 0.5.3 cleanup removed the Vulkan loader and MoltenVK), for this fork of iss_pc.
# Builds the macOS dependencies of the port into port/deps (no sudo, no x86 Homebrew).
#  - x86_64 (default) -> deps/x86_64: tlg-probe and the game run under Rosetta 2, so every
#    library linked into that process must be x86-64.
#  - arm64 (ISS_DEPS_ARCH=arm64) -> deps/arm64: for a later native GPU process (docs/macos/PLAN.md
#    M5). Same pinned set minus the x86-only Zydis/Zycore/xbyak.
# Each architecture has its own prefix, build directory and stamps; neither ever sees the
# other's libraries. Host tools (cmake, ninja, nasm, glslang, pkgconf) come from the native
# arm64 Homebrew.
#
#   bash port/scripts/macos/bootstrap_deps.sh                    # x86-64: build what is missing
#   ISS_DEPS_ARCH=arm64 bash port/scripts/macos/bootstrap_deps.sh # arm64
#   ISS_DEPS_CLEAN=1 bash port/scripts/macos/bootstrap_deps.sh    # rebuild everything (that arch)
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
ROOT=$PWD
DEPS=$ROOT/deps
SRC=$DEPS/src
ARCH=${ISS_DEPS_ARCH:-x86_64}
case $ARCH in
    x86_64) BUILD=$DEPS/build-x86_64; OTHER=$DEPS/arm64 ;;
    arm64) BUILD=$DEPS/build-arm64; OTHER=$DEPS/x86_64 ;;
    *) echo "ISS_DEPS_ARCH must be x86_64 or arm64, not $ARCH" >&2; exit 1 ;;
esac
PREFIX=$DEPS/$ARCH
# Rosetta runs AVX2 code from macOS 15 on.
export MACOSX_DEPLOYMENT_TARGET=15.0
JOBS=$(sysctl -n hw.ncpu)

SDL_TAG=release-3.4.18
FMT_TAG=11.2.0
XXHASH_TAG=v0.8.3
MINIZ_TAG=3.1.2
ZYDIS_TAG=v4.1.1
VULKAN_TAG=vulkan-sdk-1.4.363.0
MOLTENVK_TAG=v1.4.2
MAGIC_ENUM_TAG=v0.9.7
ROBIN_MAP_TAG=v1.4.0
VMA_TAG=v3.3.0
XBYAK_TAG=v7.43
FFMPEG_TAG=n7.1.5
FREETYPE_TAG=VER-2-13-3

if [[ ${ISS_DEPS_CLEAN:-} == 1 ]]; then rm -rf "$BUILD" "$PREFIX"; fi
mkdir -p "$SRC" "$BUILD" "$PREFIX"

need=()
for tool in cmake ninja nasm glslang pkg-config; do command -v $tool >/dev/null || need+=($tool); done
if ((${#need[@]})); then
    echo "Installing host tools: ${need[*]}"
    brew install cmake ninja nasm glslang pkgconf boost
fi
brew list boost >/dev/null 2>&1 || brew install boost
if [[ $ARCH == x86_64 ]]; then
    arch -x86_64 /usr/bin/true 2>/dev/null || { echo 'Rosetta 2 is required: softwareupdate --install-rosetta' >&2; exit 1; }
fi

# Nothing from the arm64 Homebrew prefix may leak into these builds (headers-only Boost aside),
# and nothing from the other architecture's prefix (or an x86 Homebrew in /usr/local).
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig
unset PKG_CONFIG_PATH
CMAKE_COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=$ARCH
    -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET -DCMAKE_INSTALL_PREFIX="$PREFIX"
    -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local;$OTHER"
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -Wno-dev)

fetch() { # name url tag
    local dir=$SRC/$1
    if [[ ! -d $dir ]]; then
        echo "== fetch $1 $3"
        git clone -q --depth 1 --branch "$3" --recurse-submodules --shallow-submodules "$2" "$dir"
    fi
}
stamp() { [[ -f $PREFIX/.built-$1 ]]; }
done_stamp() { touch "$PREFIX/.built-$1"; }
cmake_dep() { # name srcsubdir args...
    local name=$1 sub=$2; shift 2
    stamp "$name" && return
    echo "== build $name"
    cmake -S "$SRC/$name/$sub" -B "$BUILD/$name" "${CMAKE_COMMON[@]}" "$@" >"$BUILD/$name.log" 2>&1 \
        && ninja -C "$BUILD/$name" install >>"$BUILD/$name.log" 2>&1 \
        || { tail -30 "$BUILD/$name.log" >&2; echo "$name failed (log: $BUILD/$name.log)" >&2; exit 1; }
    done_stamp "$name"
}

fetch SDL https://github.com/libsdl-org/SDL.git $SDL_TAG
fetch fmt https://github.com/fmtlib/fmt.git $FMT_TAG
fetch xxHash https://github.com/Cyan4973/xxHash.git $XXHASH_TAG
fetch miniz https://github.com/richgel999/miniz.git $MINIZ_TAG
[[ $ARCH == x86_64 ]] && fetch zydis https://github.com/zyantific/zydis.git $ZYDIS_TAG
fetch Vulkan-Headers https://github.com/KhronosGroup/Vulkan-Headers.git $VULKAN_TAG
fetch Vulkan-Loader https://github.com/KhronosGroup/Vulkan-Loader.git $VULKAN_TAG
fetch magic_enum https://github.com/Neargye/magic_enum.git $MAGIC_ENUM_TAG
fetch robin-map https://github.com/Tessil/robin-map.git $ROBIN_MAP_TAG
fetch VulkanMemoryAllocator https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git $VMA_TAG
[[ $ARCH == x86_64 ]] && fetch xbyak https://github.com/herumi/xbyak.git $XBYAK_TAG
fetch FFmpeg https://github.com/FFmpeg/FFmpeg.git $FFMPEG_TAG
fetch freetype https://gitlab.freedesktop.org/freetype/freetype.git $FREETYPE_TAG

cmake_dep Vulkan-Headers .
cmake_dep Vulkan-Loader . -DUPDATE_DEPS=OFF -DBUILD_TESTS=OFF -DVulkanHeaders_DIR="$PREFIX/share/cmake/VulkanHeaders"
cmake_dep SDL . -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
cmake_dep fmt . -DFMT_TEST=OFF -DFMT_DOC=OFF
cmake_dep xxHash cmake_unofficial -DBUILD_SHARED_LIBS=OFF -DXXHASH_BUILD_XXHSUM=OFF
cmake_dep miniz . -DBUILD_SHARED_LIBS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DBUILD_FUZZERS=OFF
if [[ $ARCH == x86_64 ]]; then
    # Zydis builds against its bundled Zycore, but its installed CMake config also needs an
    # installed Zycore (find_dependency): install the same copy separately.
    cmake_dep zycore ../zydis/dependencies/zycore -DZYCORE_BUILD_SHARED_LIB=OFF
    cmake_dep zydis . -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_EXAMPLES=OFF -DZYDIS_BUILD_DOXYGEN=OFF
fi
cmake_dep magic_enum . -DMAGIC_ENUM_OPT_BUILD_EXAMPLES=OFF -DMAGIC_ENUM_OPT_BUILD_TESTS=OFF
cmake_dep robin-map .
cmake_dep VulkanMemoryAllocator .
# FreeType (the probe's text rendering): static, without the optional codecs, so nothing else
# (zlib, libpng, HarfBuzz, Brotli) has to be built or found.
cmake_dep freetype . -DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_ZLIB=TRUE -DFT_DISABLE_BZIP2=TRUE \
    -DFT_DISABLE_PNG=TRUE -DFT_DISABLE_HARFBUZZ=TRUE -DFT_DISABLE_BROTLI=TRUE
[[ $ARCH == x86_64 ]] && cmake_dep xbyak .

# Boost is used header-only; headers are architecture-independent.
if [[ ! -e $PREFIX/include/boost ]]; then
    ln -s "$(brew --prefix boost)/include/boost" "$PREFIX/include/boost"
fi

if ! stamp FFmpeg; then
    echo "== build FFmpeg (several minutes)"
    mkdir -p "$BUILD/FFmpeg"
    ffarch=$ARCH; [[ $ARCH == arm64 ]] && ffarch=aarch64
    (cd "$BUILD/FFmpeg" && "$SRC/FFmpeg/configure" --prefix="$PREFIX" \
        --arch=$ffarch --target-os=darwin --enable-cross-compile \
        --cc="clang -arch $ARCH" --cxx="clang++ -arch $ARCH" \
        --extra-cflags="-mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" \
        --extra-ldflags="-arch $ARCH -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET" \
        --enable-shared --disable-static --disable-programs --disable-doc \
        --disable-autodetect --enable-videotoolbox --enable-audiotoolbox \
        --install-name-dir='@rpath' >"$BUILD/FFmpeg.log" 2>&1 \
        && make -j"$JOBS" >>"$BUILD/FFmpeg.log" 2>&1 && make install >>"$BUILD/FFmpeg.log" 2>&1) \
        || { tail -30 "$BUILD/FFmpeg.log" >&2; echo "FFmpeg failed" >&2; exit 1; }
    done_stamp FFmpeg
fi

# MoltenVK: the release dylib is universal (arm64 + x86_64). Registered with the loader
# through an ICD manifest; run scripts point VK_DRIVER_FILES at it.
if ! stamp MoltenVK; then
    if [[ ! -f $SRC/MoltenVK-macos.tar ]]; then
        echo "== fetch MoltenVK $MOLTENVK_TAG"
        curl -fsSL -o "$SRC/MoltenVK-macos.tar" \
            "https://github.com/KhronosGroup/MoltenVK/releases/download/$MOLTENVK_TAG/MoltenVK-macos.tar"
    fi
    rm -rf "$BUILD/MoltenVK" && mkdir -p "$BUILD/MoltenVK"
    tar -xf "$SRC/MoltenVK-macos.tar" -C "$BUILD/MoltenVK"
    dylib=$(find "$BUILD/MoltenVK" -name libMoltenVK.dylib -path '*macOS*' | head -1)
    lipo "$dylib" -verify_arch $ARCH || { echo "MoltenVK dylib lacks $ARCH" >&2; exit 1; }
    cp "$dylib" "$PREFIX/lib/"
    mkdir -p "$PREFIX/share/vulkan/icd.d"
    cat > "$PREFIX/share/vulkan/icd.d/MoltenVK_icd.json" <<'JSON'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "../../../lib/libMoltenVK.dylib",
        "api_version": "1.4.0",
        "is_portability_driver": true
    }
}
JSON
    done_stamp MoltenVK
fi
# The release MoltenVK crashes at start-up in its present callbacks (MoltenVK #2791); with Xcode,
# replace it by v1.4.2 built with patches/moltenvk-1.4.2-drawable-race.patch (build_moltenvk.sh).
if [[ $ARCH == x86_64 ]] && ! stamp MoltenVK-patched && command -v xcodebuild >/dev/null; then
    bash "$(dirname "$0")/build_moltenvk.sh" && done_stamp MoltenVK-patched
fi

echo
echo "$ARCH dependencies ready in $PREFIX"
for lib in libvulkan.1.dylib libSDL3.0.dylib libMoltenVK.dylib libavcodec.dylib; do
    printf '  %-22s %s\n' "$lib" "$(lipo -archs "$PREFIX/lib/$lib" 2>/dev/null || echo missing)"
done
