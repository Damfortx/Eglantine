// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/mt/mt_switches.h"
#include "video_core/nx2.h"

/// Self-contained (gpuparam.h includes it first): switches, group cap and v2 statistics only.
/// GPU params v2 (docs/mt-lite-plan6.md, W1): the deferred macro-40 calls (mt/gpuparam.h) draw
/// without the constant buffer copy: when the call's constant buffer plan is exactly the driver
/// constants {pre +0x640 = GPU word 3 (base vertex), pre +0x644 = GPU word 4 (base instance),
/// post constants}, the back draws with Maxwell3D::engine_state = EngineHint::OnGpuParamDraw and
/// gpuparam_cb_bank = the vertex stage's bank of the selected constant buffer: the pipeline
/// variant reads those two words as the BaseVertex / BaseInstance built-ins (fixed table, no
/// replace_table). On the split front with the MT back, consecutive such calls whose records
/// follow each other in GPU memory become one multi-draw (the "front hold" of GpuSplit).
namespace VideoCommon::MT::GpuParam {

// ---------------------------------------------------------------------------------- switches

/// EDEN_NX2_MT_LITE_GPUPARAM2 (=0/1/a): on by default under EDEN_NX2_MT_LITE=1 (with
/// EDEN_NX2_MT). Implies v1's deferral (TryGpuParamCall / GpuParamCall) on the split front.
[[nodiscard]] inline bool V2MtSwitch() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_LITE_GPUPARAM2");
    return Enabled() && Tegra::Nx2Switch(mode, Lite());
}
/// EDEN_NX2_GPUPARAM2 (=0/1/a, default off): copy-free draws without the MT back (no groups).
[[nodiscard]] inline bool V2PlainSwitch() {
    static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_GPUPARAM2");
    return Tegra::Nx2Switch(mode, false);
}
[[nodiscard]] inline bool V2On(bool mt) {
    return mt ? V2MtSwitch() : V2PlainSwitch();
}
/// EDEN_NX2_GPUPARAM2_GROUP=<max> (default 64; 1 disables grouping): records a multi-draw of
/// consecutive calls may take.
[[nodiscard]] inline u32 GroupCap() {
    static const u32 cap = [] {
        const char* const value = std::getenv("EDEN_NX2_GPUPARAM2_GROUP");
        if (value == nullptr || *value == '\0') {
            return 64U;
        }
        const long parsed = std::strtol(value, nullptr, 10);
        return static_cast<u32>(std::clamp<long>(parsed, 1, 4096));
    }();
    return cap;
}

/// Bytes of one indirect record (VkDrawIndexedIndirectCommand).
inline constexpr u32 RecordBytes = 5 * sizeof(u32);

// ---------------------------------------------------------------------------- statistics

/// Back thread (GpuParamDraw) counters, read and cleared by the front's stats line.
inline std::atomic<u64> g_v2_builtin_draws{};   ///< draws with the built-ins
inline std::atomic<u64> g_v2_builtin_records{}; ///< records those draws took
inline std::atomic<u64> g_v2_fallback_no_bank{};  ///< no vertex bank holds const_buffer address
inline std::atomic<u64> g_v2_fallback_bank{};     ///< the vertex bank is not 0
inline std::atomic<u64> g_v2_fallback_records{};  ///< records drawn by the v1 copy fallback

/// Why a v2 call took v1's copy path on the front.
enum class V2Fallback : u8 {
    Plan,   ///< constant buffer plan not exactly the built-ins'
    Kind,   ///< multi-draw call
    Count,
};
inline constexpr std::array<std::string_view, static_cast<size_t>(V2Fallback::Count)>
    V2FallbackNames{"plan", "multi"};

/// Release reasons: GpuSplit::Hold* >> 16.
inline constexpr std::array<std::string_view, 14> HoldReasonNames{
    "none",       "method",       "multi method", "macro write",  "state write",
    "defer",      "defer function", "drain",      "wait",         "run",
    "dispatch end", "non-joinable", "full",       "other"};

/// Why the next call did not join the pending group.
enum class NoJoin : u8 {
    Other,    ///< another engine's group / not a copy-free call
    Macro,
    Begin,
    Plan,
    Address,  ///< records not right after the group's last
    Unsafe,   ///< classifier: the zero-word call may emit more than cb data / latches
    Count,
};
inline constexpr std::array<std::string_view, static_cast<size_t>(NoJoin::Count)> NoJoinNames{
    "other", "macro", "begin", "plan", "address", "unsafe"};

struct V2Stats {
    u64 calls{};      ///< deferred calls with v2 on
    u64 copy_free{};  ///< of them drawn with the built-ins (no copy)
    std::array<u64, static_cast<size_t>(V2Fallback::Count)> fallbacks{};
    u64 groups{};     ///< groups emitted (held draws, any size)
    u64 grouped_records{};
    std::array<u64, 7> sizes{}; ///< 1, 2, 3, 4, 5-8, 9-16, 17+
    std::array<u64, HoldReasonNames.size()> releases{};
    std::unordered_map<u32, u64> release_methods; ///< method number -> releases
    std::array<u64, static_cast<size_t>(NoJoin::Count)> no_join{};
    u64 zero_call_breaks{}; ///< a joining call's zero call emitted something else
    u64 check_compared{};
    u64 check_mismatch{};   ///< macro's cb words != record words 3/4

    void NoteGroup(u32 size, u32 why) {
        ++groups;
        grouped_records += size;
        const size_t bucket = size <= 4 ? size - 1 : size <= 8 ? 4 : size <= 16 ? 5 : 6;
        ++sizes[std::min<size_t>(bucket, sizes.size() - 1)];
        const size_t kind = std::min<size_t>(why >> 16, releases.size() - 1);
        ++releases[kind];
        if (kind >= 1 && kind <= 4) {
            ++release_methods[why & 0xffff];
        }
    }

    [[nodiscard]] std::string Format() {
        std::string fallback_text;
        for (size_t i = 0; i < fallbacks.size(); ++i) {
            if (fallbacks[i] != 0) {
                fallback_text += fmt::format(" {} {},", V2FallbackNames[i], fallbacks[i]);
            }
        }
        std::string release_text;
        for (size_t i = 1; i < releases.size(); ++i) {
            if (releases[i] != 0) {
                release_text += fmt::format(" {} {},", HoldReasonNames[i], releases[i]);
            }
        }
        std::vector<std::pair<u32, u64>> methods(release_methods.begin(), release_methods.end());
        std::sort(methods.begin(), methods.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        std::string method_text;
        for (size_t i = 0; i < methods.size() && i < 6; ++i) {
            method_text += fmt::format(" {:#x} {},", methods[i].first, methods[i].second);
        }
        std::string no_join_text;
        for (size_t i = 0; i < no_join.size(); ++i) {
            if (no_join[i] != 0) {
                no_join_text += fmt::format(" {} {},", NoJoinNames[i], no_join[i]);
            }
        }
        return fmt::format(
            "; v2: {} calls, {} copy-free, v1 copy:{} back: {} built-in draws ({} records), "
            "fallback to copies {} no vertex bank, {} bank not 0 ({} records); groups {} "
            "({} records; sizes 1:{} 2:{} 3:{} 4:{} 5-8:{} 9-16:{} 17+:{}), released by:{} "
            "methods:{} not joined:{} zero-call breaks {}; v2 check {} compared, {} mismatched",
            calls, copy_free, fallback_text.empty() ? " none" : fallback_text,
            g_v2_builtin_draws.exchange(0, std::memory_order_relaxed),
            g_v2_builtin_records.exchange(0, std::memory_order_relaxed),
            g_v2_fallback_no_bank.exchange(0, std::memory_order_relaxed),
            g_v2_fallback_bank.exchange(0, std::memory_order_relaxed),
            g_v2_fallback_records.exchange(0, std::memory_order_relaxed), groups,
            grouped_records, sizes[0], sizes[1], sizes[2], sizes[3], sizes[4], sizes[5], sizes[6],
            release_text.empty() ? " none" : release_text,
            method_text.empty() ? " none" : method_text,
            no_join_text.empty() ? " none" : no_join_text, zero_call_breaks, check_compared,
            check_mismatch);
    }
};

} // namespace VideoCommon::MT::GpuParam
