// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Stand-in for common/assert.h used only by the standalone MT selftest build: every assert
// (DEBUG_ASSERT included) is fatal and prints its location.

#pragma once

#include <cstdio>
#include <cstdlib>

#include "common/logging.h"

namespace MtSelftestStub {
[[noreturn]] inline void AssertFail(const char* file, int line, const char* what) {
    std::fprintf(stderr, "ASSERT FAILED %s:%d: %s\n", file, line, what);
    std::abort();
}
} // namespace MtSelftestStub

#define ASSERT_MSG(_a_, ...)                                                                       \
    ((_a_) ? (void)0 : ::MtSelftestStub::AssertFail(__FILE__, __LINE__, #_a_))
#define ASSERT(_a_) ASSERT_MSG(_a_, "")
#define UNREACHABLE_MSG(...) ::MtSelftestStub::AssertFail(__FILE__, __LINE__, "unreachable")
#define UNREACHABLE() UNREACHABLE_MSG("")
#define DEBUG_ASSERT(_a_) ASSERT(_a_)
#define DEBUG_ASSERT_MSG(_a_, ...) ASSERT(_a_)
#define UNIMPLEMENTED() ASSERT(false && "Unimplemented!")
#define UNIMPLEMENTED_MSG(...) ASSERT(false && "Unimplemented!")
#define UNIMPLEMENTED_IF(cond) ASSERT(!(cond))
#define UNIMPLEMENTED_IF_MSG(cond, ...) ASSERT(!(cond))
#define ASSERT_OR_EXECUTE_MSG(_a_, _b_, ...)                                                       \
    do {                                                                                           \
        ASSERT(_a_);                                                                               \
    } while (0)
#define ASSERT_OR_EXECUTE(_a_, _b_) ASSERT_OR_EXECUTE_MSG(_a_, _b_, "")
