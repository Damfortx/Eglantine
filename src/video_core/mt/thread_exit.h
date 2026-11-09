// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <exception>
#include <stop_token>

#include "common/logging.h"

namespace VideoCommon::MT {

/// Runs a pipeline thread's body and makes every way out of it visible in the log: a normal
/// exit says whether a stop was requested (a loop that returns without one is a bug: the
/// threads it feeds wait forever), an exception is logged before it ends the process.
template <typename Body>
void RunLoudThread(const char* name, std::stop_token stop, Body&& body) {
    try {
        body();
    } catch (const std::exception& e) {
        LOG_CRITICAL(HW_GPU, "MT: thread {} died with an exception: {}", name, e.what());
        throw;
    } catch (...) {
        LOG_CRITICAL(HW_GPU, "MT: thread {} died with an unknown exception", name);
        throw;
    }
    if (stop.stop_requested()) {
        LOG_INFO(HW_GPU, "MT: thread {} exits (stop requested)", name);
    } else {
        LOG_CRITICAL(HW_GPU, "MT: thread {} exits WITHOUT a stop request", name);
    }
}

} // namespace VideoCommon::MT
