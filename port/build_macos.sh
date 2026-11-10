#!/usr/bin/env bash
# macOS (Apple Silicon) build, after bloodborne_mac_port's first build_macos.sh (Mark Hall,
# GPL-2.0-or-later). Everything is x86-64 and runs under Rosetta 2, because the game's eboot.bin
# is x86-64 code executed directly. Dependencies come from port/deps/x86_64
# (scripts/macos/bootstrap_deps.sh); nothing from the arm64 Homebrew may be linked.
#
#   bash port/build_macos.sh             # libtlggpu.dylib, tlg-probe, tlg-gpu-capabilities
#   bash port/build_macos.sh --test      # also build and run the unit tests
#   bash port/build_macos.sh --gpu-only  # only the GPU library
#
# TLG_LTO=OFF (or ISS_LTO=OFF) disables link-time optimization of the GPU library. PGO is
# GCC-specific (.gcda profiles) and stays off on macOS.
set -euo pipefail
cd -- "$(dirname -- "$0")"
ROOT=$PWD
PREFIX=$ROOT/deps/x86_64
export MACOSX_DEPLOYMENT_TARGET=15.0
[[ -n ${ISS_LTO:-} && -z ${TLG_LTO:-} ]] && export TLG_LTO=$ISS_LTO
if [[ ! -f $PREFIX/.built-MoltenVK || ! -f $PREFIX/.built-freetype ]]; then
    echo 'x86-64 dependencies missing: run bash port/scripts/macos/bootstrap_deps.sh' >&2; exit 1
fi
for tool in cmake ninja glslang pkg-config; do
    command -v $tool >/dev/null || { echo "Missing $tool (brew install cmake ninja glslang pkgconf)" >&2; exit 1; }
done
# The commit the probe logs on its first line (as build.sh).
if [[ -z ${TLG_BUILD_COMMIT:-} ]]; then
    TLG_BUILD_COMMIT=${GITHUB_SHA:-$(git rev-parse HEAD 2>/dev/null || echo unknown)}
    TLG_BUILD_COMMIT=${TLG_BUILD_COMMIT:0:12}
    [[ -z ${GITHUB_SHA:-} ]] && ! git diff --quiet HEAD -- . 2>/dev/null && TLG_BUILD_COMMIT+="+dirty"
fi
export TLG_BUILD_COMMIT
mkdir -p out
CC=${CC:-/usr/bin/clang}
ARCH=(-arch x86_64 -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET)
export PKG_CONFIG_LIBDIR=$PREFIX/lib/pkgconfig:$PREFIX/share/pkgconfig
unset PKG_CONFIG_PATH
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3 libavcodec libavutil libswscale freetype2)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3 libavcodec libavutil libswscale freetype2)"

# GPU library (shadPS4 video core + drivers): out/gpu/libtlggpu.dylib.
if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ]]; then
    git submodule update --init --recursive
fi
for patch in gpu/patches/fsr-vulkan/*.patch; do
    if ! git -C gpu/third_party/fsr-vulkan apply --reverse --check "$PWD/$patch" 2>/dev/null; then
        git -C gpu/third_party/fsr-vulkan apply "$PWD/$patch"
    fi
done
# Boost is header-only: the arm64 Homebrew headers are fine (nothing is linked from there).
boost=$(brew --prefix boost 2>/dev/null || echo /opt/homebrew/opt/boost)
# Clang PGO: with a profile in pgo/ (scripts/macos/pgo_train.sh) the GPU library is built with it
# (+4% FPS in the cannery benchmark); TLG_PGO=off builds without, TLG_PGO=generate instruments.
if [[ -z ${TLG_PGO:-} && -f pgo/tlggpu.profdata ]]; then TLG_PGO=use; fi
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="${CXX:-/usr/bin/clang++}" \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET \
    -DCMAKE_PREFIX_PATH="$PREFIX;$boost" -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" \
    -DBoost_INCLUDE_DIR="$boost/include" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DTLG_PGO="${TLG_PGO:-off}" -DTLG_LTO="${TLG_LTO:-ON}" -DTLG_PGO_DIR="$PWD/pgo" -Wno-dev >/dev/null
echo "GPU library: LTO ${TLG_LTO:-ON}, PGO ${TLG_PGO:-off}"
# A failed GPU build must stop here: an older libtlggpu.dylib would otherwise be used silently.
if ! ninja -C out/gpu tlggpu > out/gpu-build.log 2>&1; then
    grep -v '^\[' out/gpu-build.log | tail -40 >&2
    echo 'GPU library build failed (full log: out/gpu-build.log)' >&2; exit 1
fi
if [[ ${1:-} == --gpu-only ]]; then echo "Built $PWD/out/gpu/libtlggpu.dylib"; exit 0; fi

# @executable_path/gpu: packaged copies keep the library next to the binary without patching it.
gpu=(-Lout/gpu -ltlggpu -Wl,-rpath,@executable_path/gpu -Wl,-rpath,"$PWD/out/gpu"
    -Wl,-rpath,"$PREFIX/lib")
runtime=(src/runtime*.c)
# Third-party decoders: compiled once, without this project's -Werror policy.
atrac9=(third_party/LibAtrac9/C/src/*.c)
if [[ ! -f out/libatrac9.a || -n $(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c') ]]; then
    rm -rf out/atrac9 && mkdir -p out/atrac9
    for source in "${atrac9[@]}"; do "$CC" "${ARCH[@]}" -std=c99 -O2 -g -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"; done
    ar rcs out/libatrac9.a out/atrac9/*.o
fi
# _XOPEN_SOURCE: macOS hides the (deprecated) ucontext routines the fiber code uses without it;
# _DARWIN_C_SOURCE keeps every other Darwin API visible alongside.
cflags=(-std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${ARCH[@]}" -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE)
# isa_emulate runs inside guest code: no XMM/YMM.
"$CC" "${cflags[@]}" -mgeneral-regs-only -c src/isa_emulate.c -o out/isa_emulate.o
if [[ ${1:-} != --test-only ]]; then
    "$CC" "${cflags[@]}" -DTLG_BUILD_COMMIT="\"$TLG_BUILD_COMMIT\"" "${includes[@]}" -I. -Isrc src/probe.c "${runtime[@]}" \
        src/gpu_process.c src/cpu_check.c src/vulkan_smoke.c out/isa_emulate.o src/isa_rewrite.c out/libatrac9.a -lm \
        "${gpu[@]}" "${libraries[@]}" -framework CoreFoundation -o out/tlg-probe
    echo "Built $PWD/out/tlg-probe"
    # Native arm64 GPU process (docs/macos/GPU_PROCESS_PLAN.md, TLG_GPU_PROCESS=1): the GPU library
    # built arm64 (scripts/macos/build_gpu_arm64.sh, deps/arm64) linked into out/tlggpu-host.
    ARM64_PREFIX=$PWD/deps/arm64
    if [[ -f $ARM64_PREFIX/lib/pkgconfig/sdl3.pc && ${TLG_GPU_HOST:-1} != 0 ]]; then
        TLG_LTO=${TLG_LTO:-ON} bash scripts/macos/build_gpu_arm64.sh
        host_cc=(/usr/bin/clang -arch arm64 -mmacosx-version-min=$MACOSX_DEPLOYMENT_TARGET -std=c11 -O2 -g -Wall -Wextra
            -I. host/main.c host/host_runtime.c host/host_iss.c -Lout/gpu-arm64 -ltlggpu
            -Wl,-rpath,"$PWD/out/gpu-arm64" -Wl,-rpath,"$ARM64_PREFIX/lib" -framework CoreFoundation)
        "${host_cc[@]}" -o out/tlggpu-host
        # macOS 27: libmalloc takes addresses in the guest range before main(), so a second host
        # carries the range as a zero-fill segment the kernel maps at exec (host/guest_reservation.s).
        "${host_cc[@]}" host/guest_reservation.s -Wl,-segaddr,__GUESTRSV,0x7000000000 \
            -Wl,-segprot,__GUESTRSV,---,--- -o out/tlggpu-host-macos27
        echo "Built $PWD/out/tlggpu-host and out/tlggpu-host-macos27 (arm64)"
        # A local ISS.app runs copies of these from inside its bundle (Game Mode): keep them current.
        bash scripts/macos/refresh_app.sh || true
    fi
    # TLG_RENDERER_V2=1: also renderer v2 (gpu_v2/README.md) and its probe, out/tlg-probe-v2
    # (TLG_PROBE=out/tlg-probe-v2 run_macos.sh runs it).
    if [[ ${TLG_RENDERER_V2:-} == 1 ]]; then
        cmake -S gpu_v2 -B out/gpu_v2 -G Ninja -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="${CXX:-/usr/bin/clang++}" \
            -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET \
            -DCMAKE_PREFIX_PATH="$PREFIX;$boost" -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew;/usr/local" \
            -DBoost_INCLUDE_DIR="$boost/include" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -Wno-dev >/dev/null
        if ! ninja -C out/gpu_v2 issgpu > out/gpu_v2-build.log 2>&1; then
            grep -v '^\[' out/gpu_v2-build.log | tail -40 >&2
            echo 'Renderer v2 build failed (full log: out/gpu_v2-build.log)' >&2; exit 1
        fi
        "$CC" "${cflags[@]}" -DTLG_BUILD_COMMIT="\"$TLG_BUILD_COMMIT\"" "${includes[@]}" -I. -Isrc src/probe.c "${runtime[@]}" \
            src/cpu_check.c src/vulkan_smoke.c out/isa_emulate.o src/isa_rewrite.c out/libatrac9.a -lm \
            -Lout/gpu_v2 -lissgpu -Wl,-rpath,"$PWD/out/gpu_v2" -Wl,-rpath,"$PREFIX/lib" "${libraries[@]}" \
            -framework CoreFoundation -o out/tlg-probe-v2
        echo "Built $PWD/out/tlg-probe-v2 (renderer v2)"
    fi
    # GPU check for run.sh (live_resolution=auto): links only the Vulkan loader.
    "$CC" -std=c11 -O2 -Wall -Wextra -Werror "${ARCH[@]}" -Igpu/shadps4 "${includes[@]}" tools/gpu_capabilities.c \
        "${libraries[@]}" -Wl,-rpath,"$PREFIX/lib" -o out/tlg-gpu-capabilities
fi
if [[ ${1:-} == --test || ${1:-} == --test-only ]]; then
    rpath=(-Wl,-rpath,"$PREFIX/lib")
    "$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc tests/test_pad.c "${libraries[@]}" "${rpath[@]}" -lm -o out/pad-test
    out/pad-test
    "$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc tests/test_pad_map.c "${libraries[@]}" "${rpath[@]}" -lm -o out/pad-map-test
    out/pad-map-test
    "$CC" "${cflags[@]}" tests/test_mouse_stick.c -lm -o out/mouse-stick-test
    out/mouse-stick-test
    "$CC" "${cflags[@]}" -UNDEBUG tests/test_mouse_shake.c -lm -o out/mouse-shake-test
    out/mouse-shake-test
    "$CC" "${cflags[@]}" -UNDEBUG tests/test_mouse_touch.c -Isrc -lm -o out/mouse-touch-test
    out/mouse-touch-test
    "$CC" "${cflags[@]}" -UNDEBUG tests/test_rumble_gate.c -Isrc -o out/rumble-gate-test
    out/rumble-gate-test
    "$CC" "${cflags[@]}" -UNDEBUG "${includes[@]}" -I. -Isrc tests/test_mouse_pad.c "${libraries[@]}" "${rpath[@]}" -lm -o out/mouse-pad-test
    out/mouse-pad-test
    "$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc tests/test_runtime.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/runtime-test
    out/runtime-test
    "$CC" "${cflags[@]}" -I. -Isrc tests/test_file_mods.c -o out/file-mods-test
    out/file-mods-test
    "$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc tests/test_sema.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/sema-test
    out/sema-test
    "$CC" "${cflags[@]}" -Isrc tests/test_rwlock_rp.c -o out/rwlock-rp-test
    out/rwlock-rp-test
    "$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc tests/test_freeze.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/freeze-test
    out/freeze-test
    "$CC" "${cflags[@]}" -I. -Isrc tests/test_content.c src/runtime_content.c -o out/content-test
    out/content-test
    "$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc tests/test_mp3.c src/runtime_mp3.c out/libatrac9.a -lm "${libraries[@]}" "${rpath[@]}" -o out/mp3-test
    out/mp3-test tests/data/sine.mp3
    "$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc tests/test_videodec.c "${libraries[@]}" "${rpath[@]}" -o out/videodec-test
    out/videodec-test tests/data/tiny.h264
    "$CC" "${cflags[@]}" -I. -Isrc tests/test_services.c -o out/services-test
    out/services-test
    "$CC" "${cflags[@]}" -mbmi -mlzcnt -mmovbe -I. -Isrc tests/test_isa_emulate.c out/isa_emulate.o -o out/isa-emulate-test
    out/isa-emulate-test
    "$CC" "${cflags[@]}" -c src/isa_rewrite.c -I. -Isrc -o out/isa_rewrite.o
    "$CC" "${cflags[@]}" -mbmi -mlzcnt -I. -Isrc tests/test_isa_stub.c out/isa_rewrite.o out/isa_emulate.o -o out/isa-stub-test
    out/isa-stub-test tests/data/isa_fixture.bin
    "$CC" "${cflags[@]}" -I. -Isrc tests/test_red_zone.c out/isa_rewrite.o out/isa_emulate.o -o out/red-zone-test
    out/red-zone-test tests/data/red_zone_image.bin tests/data/red_zone_table.bin
    # SSE4A (EXTRQ/INSERTQ): Rosetta, like Intel CPUs, has none, so the recorded AMD results apply.
    "$CC" "${cflags[@]}" -msse4a -I. -Isrc tests/test_isa_sse4a.c out/isa_rewrite.o out/isa_emulate.o -o out/isa-sse4a-test
    out/isa-sse4a-test
    "$CC" "${cflags[@]}" -mavx -mf16c -I. -Isrc tests/test_isa_f16c.c out/isa_rewrite.o out/isa_emulate.o -o out/isa-f16c-test
    out/isa-f16c-test
    "$CC" "${cflags[@]}" -UNDEBUG -I. -Isrc tests/test_cpu_check.c -o out/cpu-check-test
    out/cpu-check-test
fi
