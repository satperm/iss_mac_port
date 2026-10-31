#!/usr/bin/env bash
# Clang PGO profile for the GPU library: an instrumented build plays the benchmark route
# (bench.sh with BENCH_PAD=1, profiles flushed every 30 s), the profiles are merged into
# pgo/tlggpu.profdata and the library is rebuilt with it. build_macos.sh then keeps using it.
# Retrain after larger renderer changes (a stale profile only loses the gain).
#
#   TLG_GAME_DIR=/path/to/game bash port/scripts/macos/pgo_train.sh
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
rm -rf pgo && mkdir -p pgo
TLG_PGO=generate bash build_macos.sh --gpu-only
BENCH_PAD=1 bash scripts/macos/bench.sh pgotrain
xcrun llvm-profdata merge -o pgo/tlggpu.profdata pgo/*.profraw
rm -f pgo/*.profraw
TLG_PGO=use bash build_macos.sh
