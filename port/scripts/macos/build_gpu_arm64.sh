#!/usr/bin/env bash
# Builds the GPU library (gpu/) natively for arm64, for the native GPU host process
# (docs/macos/GPU_PROCESS_PLAN.md; as bloodborne_mac_port's build_gpu_arm64.sh). The x86-64 build
# loaded by the game stays in build_macos.sh (out/gpu); this one goes to out/gpu-arm64.
# Dependencies come from deps/arm64 (ISS_DEPS_ARCH=arm64 bash scripts/macos/bootstrap_deps.sh).
#
#   bash port/scripts/macos/build_gpu_arm64.sh   # out/gpu-arm64/libtlggpu.dylib
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
ROOT=$PWD
PREFIX=$ROOT/deps/arm64
OUT=$ROOT/out/gpu-arm64
export MACOSX_DEPLOYMENT_TARGET=15.0
[[ -d $PREFIX/lib ]] || { echo 'arm64 dependencies missing: ISS_DEPS_ARCH=arm64 bash scripts/macos/bootstrap_deps.sh' >&2; exit 1; }
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig
unset PKG_CONFIG_PATH
boost=$(brew --prefix boost 2>/dev/null || echo /opt/homebrew/opt/boost)
cmake -S gpu -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET \
    -DCMAKE_PREFIX_PATH="$PREFIX;$boost" \
    -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local;$ROOT/deps/x86_64" \
    -DBoost_INCLUDE_DIR="$boost/include" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DTLG_PGO=off -DTLG_LTO="${TLG_LTO:-ON}" -DTLG_PGO_DIR="$ROOT/pgo" -Wno-dev >/dev/null
echo "GPU library (arm64): LTO ${TLG_LTO:-ON}"
if ! ninja -C "$OUT" -k 0 tlggpu > "$ROOT/out/gpu-arm64-build.log" 2>&1; then
    echo 'arm64 GPU library build failed (full log: out/gpu-arm64-build.log)' >&2; exit 1
fi
lipo "$OUT/libtlggpu.dylib" -verify_arch arm64
echo "Built $OUT/libtlggpu.dylib"
