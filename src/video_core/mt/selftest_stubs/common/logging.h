// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Stand-in for common/logging.h used only by the standalone MT selftest build
// (src/video_core/mt/selftest_standalone.sh): LOG_* go to stderr through std::format, so the
// build needs neither fmt nor the logging backend.

#pragma once

#include <cstdio>
#include <format>
#include <string>
#include <utility>

namespace MtSelftestStub {
template <typename... Args>
inline void Log(const char* level, const char* cls, std::format_string<Args...> fmt,
                Args&&... args) {
    const std::string text = std::format(fmt, std::forward<Args>(args)...);
    std::fprintf(stderr, "[%s %s] %s\n", level, cls, text.c_str());
}
} // namespace MtSelftestStub

#define LOG_TRACE(log_class, ...) ((void)0)
#define LOG_DEBUG(log_class, ...) ((void)0)
#define LOG_INFO(log_class, ...) ::MtSelftestStub::Log("I", #log_class, __VA_ARGS__)
#define LOG_WARNING(log_class, ...) ::MtSelftestStub::Log("W", #log_class, __VA_ARGS__)
#define LOG_ERROR(log_class, ...) ::MtSelftestStub::Log("E", #log_class, __VA_ARGS__)
#define LOG_CRITICAL(log_class, ...) ::MtSelftestStub::Log("C", #log_class, __VA_ARGS__)
