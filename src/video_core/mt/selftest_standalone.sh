#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the MT primitive selftests (mt/selftest.h and what it includes, mt/selftest_lazy_wake.h)
# outside CMake, with stub common/assert.h and common/logging.h (mt/selftest_stubs), and runs them.
#
#   bash src/video_core/mt/selftest_standalone.sh [tsan|asan|release|all] [test-name-filter]
#
# Output (binary + log) goes to $OUT (default /tmp/claude-1000/mt-selftest). Exit status is
# non-zero when a test fails or the sanitizer reported anything. In the build image:
#   docker run --rm --security-opt seccomp=unconfined -u 1000:1000 -v $HOME/src:$HOME/src -w $HOME/src/Eglantine-mt \
#       eglantine-build:pgo bash src/video_core/mt/selftest_standalone.sh all
set -uo pipefail

MODE=${1:-tsan}
FILTER=${2:-}
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
OUT=${OUT:-/tmp/claude-1000/mt-selftest}
CXX=${CXX:-$(command -v clang++-23 || command -v clang++ || echo g++)}
TIMEOUT=${TIMEOUT:-300}

if [[ "$MODE" == all ]]; then
    status=0
    for m in tsan asan; do
        bash "${BASH_SOURCE[0]}" "$m" "$FILTER" || status=1
    done
    exit $status
fi

case "$MODE" in
tsan) FLAGS=(-O1 -g -fsanitize=thread) ;;
asan) FLAGS=(-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined
             -fno-sanitize-recover=undefined) ;;
release) FLAGS=(-O2 -g) ;;
*) echo "usage: $0 [tsan|asan|release|all] [filter]" >&2; exit 2 ;;
esac

mkdir -p "$OUT"
BIN="$OUT/mt_selftest_$MODE"
LOG="$OUT/mt_selftest_$MODE.log"

echo "== building $MODE with $CXX -> $BIN"
"$CXX" -std=c++20 -Wall -Wextra -Wno-unused-parameter "${FLAGS[@]}" -pthread \
    -I "$ROOT/src/video_core/mt/selftest_stubs" -I "$ROOT/src" \
    -x c++ - -o "$BIN" <<'EOF' || { echo "== $MODE: BUILD FAILED"; exit 1; }
#include <chrono>
#include <cstdio>
#include <cstring>
#include "video_core/mt/selftest.h"
#include "video_core/mt/selftest_lazy_wake.h"

int main(int argc, char** argv) {
    using namespace VideoCommon::MT::Selftest;
    struct Test {
        const char* name;
        bool (*fn)();
    };
    static constexpr Test tests[] = {
        {"spmc_stream", StreamTest},
        {"single_writer_map+epoch", MapTest},
        {"stable_slot_vector+epoch", SlotVectorTest},
        {"atomic_flags", FlagsTest},
        {"ring_arena", ArenaTest},
        {"engine_ptr", EnginePtrTest},
        {"reg_write_set", RegSetTest},
        {"p5_t1_slot_ebr", SlotEbrStressTest},
        {"p5_t2_multi_writer_map", MultiWriterMapStressTest},
        {"p5_t3_buffer_model", BufferModelStressTest},
        {"p5_t4_arena_worker", ArenaWorkerStressTest},
        {"p5_t5_batch_publish", BatchPublishStressTest},
        {"p5_t6_variant_array", VariantArrayStressTest},
        {"p5_t7_barrier", BarrierStressTest},
        {"t8_uniform_ring", UniformRingStressTest},
        {"t9_packet_replica", PacketReplicaStressTest},
        {"t10_replay_arena", ReplayArenaStressTest},
        {"t11_lazy_wake", LazyWakeStressTest},
    };
    const char* filter = argc > 1 && argv[1][0] != '\0' ? argv[1] : nullptr;
    int failed = 0;
    for (const Test& test : tests) {
        if (filter != nullptr && std::strstr(test.name, filter) == nullptr) {
            continue;
        }
        const auto start = std::chrono::steady_clock::now();
        const bool ok = test.fn();
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        std::printf("%-28s %s (%lld ms)\n", test.name, ok ? "PASS" : "FAIL", ms);
        std::fflush(stdout);
        failed += ok ? 0 : 1;
    }
    std::printf("RESULT: %s\n", failed == 0 ? "ALL PASS" : "FAILURES");
    return failed == 0 ? 0 : 1;
}
EOF

echo "== running $MODE (log: $LOG)"
export TSAN_OPTIONS="halt_on_error=0 second_deadlock_stack=1 history_size=4 ${TSAN_OPTIONS:-}"
export ASAN_OPTIONS="detect_leaks=1 ${ASAN_OPTIONS:-}"
export UBSAN_OPTIONS="print_stacktrace=1 ${UBSAN_OPTIONS:-}"
# TSan needs a low-entropy layout on recent kernels: run without ASLR when allowed (in docker
# this needs --security-opt seccomp=unconfined, otherwise personality() is refused).
LAUNCH=()
if [[ "$MODE" == tsan ]] && setarch "$(uname -m)" -R true 2>/dev/null; then
    LAUNCH=(setarch "$(uname -m)" -R)
fi
timeout "$TIMEOUT" "${LAUNCH[@]}" "$BIN" "$FILTER" 2>&1 | tee "$LOG"
rc=${PIPESTATUS[0]}
reports=$(grep -cE 'FATAL: ThreadSanitizer|WARNING: ThreadSanitizer|ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:' "$LOG")
echo "== $MODE: exit=$rc sanitizer_reports=$reports"
[[ $rc -eq 0 && $reports -eq 0 ]]
