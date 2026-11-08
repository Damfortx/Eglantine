// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/logging.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/mt/maxwell3d_reg_names.h"
#include "video_core/mt/mt_switches.h"
#include "video_core/mt/reg_delta.h"
#include "video_core/mt/role.h"

/// EDEN_NX2_MT_MIRROR=N (docs/mt-plan.md phase 3): a Maxwell3D mirror fed only by the deltas the
/// multithreaded back would send (registers marked in mt_written, constant buffer binds, root
/// table methods), compared with the real engine every N draws. A difference is a register
/// write the capture misses.
namespace VideoCommon::MT {

class Mirror3D {
    using Maxwell3D = Tegra::Engines::Maxwell3D;

public:
    explicit Mirror3D(Maxwell3D& real_)
        : real{real_}, mirror{std::make_unique<Maxwell3D>(real_.GetMemoryManager())} {
        real.mt_record = true;
        Resync();
    }

    /// Mirror = real, nothing pending.
    void Resync() {
        mirror->regs = real.regs;
        mirror->state = real.state;
        mirror->root_tables = real.root_tables;
        mirror->dirty.tables = real.dirty.tables;
        real.mt_written.Clear();
        real.mt_cb_binds.clear();
        real.mt_root_ops.clear();
    }

    /// The deltas the sequencer would emit before a packet, applied to the mirror.
    void ApplyPending() {
        pairs.resize(RegWriteSet::NumBits);
        const u32 count =
            real.mt_written.Drain(real.regs.reg_array.data(), Maxwell3D::Regs::NUM_REGS, pairs.data());
        MirrorApplier<Maxwell3D>::ApplyPairs(*mirror, pairs.data(), count);
        for (const CbBindDelta& bind : real.mt_cb_binds) {
            Maxwell3D::ApplyCbBind(mirror->state, bind);
        }
        real.mt_cb_binds.clear();
        for (const u64 op : real.mt_root_ops) {
            Maxwell3D::ApplyRootTableMethod(mirror->root_tables, static_cast<u32>(op),
                                            static_cast<u32>(op >> 32));
        }
        real.mt_root_ops.clear();
        delta_words += count;
    }

    /// Returns whether they matched; logs the first difference of each kind once.
    bool Compare() {
        ++checks;
        bool matched = true;
        const auto& real_regs = real.regs.reg_array;
        const auto& mirror_regs = mirror->regs.reg_array;
        if (std::memcmp(real_regs.data(), mirror_regs.data(), sizeof(real_regs)) != 0) {
            u32 first = 0;
            u32 differing = 0;
            for (u32 i = Maxwell3D::Regs::NUM_REGS; i-- > 0;) {
                if (real_regs[i] != mirror_regs[i]) {
                    first = i;
                    ++differing;
                }
            }
            Report(first, fmt::format("register {:#x} {} real {:#x} mirror {:#x} ({} differ)", first,
                                      Maxwell3DRegName(first), real_regs[first], mirror_regs[first],
                                      differing));
            matched = false;
        }
        for (size_t stage = 0; stage < real.state.shader_stages.size(); ++stage) {
            const auto& real_cbs = real.state.shader_stages[stage].const_buffers;
            const auto& mirror_cbs = mirror->state.shader_stages[stage].const_buffers;
            for (size_t slot = 0; slot < real_cbs.size(); ++slot) {
                const auto& a = real_cbs[slot];
                const auto& b = mirror_cbs[slot];
                if (a.address != b.address || a.size != b.size || a.enabled != b.enabled) {
                    Report(0x10000 | static_cast<u32>(stage * 32 + slot),
                           fmt::format("const buffer stage {} slot {}: real {:#x}/{:#x}/{} mirror "
                                       "{:#x}/{:#x}/{}",
                                       stage, slot, a.address, a.size, a.enabled, b.address,
                                       b.size, b.enabled));
                    matched = false;
                }
            }
        }
        const auto& rt = real.root_tables;
        const auto& mt = mirror->root_tables;
        if (rt.tables != mt.tables || rt.selected != mt.selected || rt.offset != mt.offset ||
            rt.loaded != mt.loaded) {
            Report(0x20000, fmt::format("root tables: selected {}/{} offset {}/{} loaded {}/{}",
                                        rt.selected, mt.selected, rt.offset, mt.offset, rt.loaded,
                                        mt.loaded));
            matched = false;
        }
        if (!matched) {
            ++mismatches;
            Resync(); // keep looking for other misses
        }
        return matched;
    }

    u64 checks = 0;
    u64 mismatches = 0;
    u64 delta_words = 0;

private:
    void Report(u32 key, const std::string& message) {
        if (reported.size() < 256 && reported.insert(key).second) {
            LOG_WARNING(HW_GPU, "MT mirror mismatch (check {}): {}", checks, message);
        }
    }

    Maxwell3D& real;
    std::unique_ptr<Maxwell3D> mirror;
    std::vector<u64> pairs;
    std::unordered_set<u32> reported;
};

/// Called by the back at each draw (before the draw reads any state).
inline void MirrorCheckDraw(Tegra::Engines::Maxwell3D& real) {
    const u32 interval = MirrorInterval();
    if (interval == 0 || Active()) {
        return;
    }
    // Back thread only.
    static std::unordered_map<const Tegra::Engines::Maxwell3D*, std::unique_ptr<Mirror3D>> mirrors;
    static u64 draws = 0;
    static auto last_log = std::chrono::steady_clock::now();
    auto& slot = mirrors[&real];
    if (!slot) {
        slot = std::make_unique<Mirror3D>(real);
        LOG_INFO(HW_GPU, "MT mirror: tracking 3D engine {} (every {} draws)", fmt::ptr(&real),
                 interval);
        return;
    }
    slot->ApplyPending();
    if (++draws % interval == 0) {
        slot->Compare();
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_log >= std::chrono::seconds{10}) {
        last_log = now;
        for (const auto& [engine, mirror] : mirrors) {
            LOG_INFO(HW_GPU, "MT mirror: engine {} {} checks, {} mismatches, {} delta words",
                     fmt::ptr(engine), mirror->checks, mirror->mismatches, mirror->delta_words);
        }
    }
}

/// The compute engine's registers (its launch description travels whole in the packet).
class MirrorCompute {
    using KeplerCompute = Tegra::Engines::KeplerCompute;

public:
    explicit MirrorCompute(KeplerCompute& real_)
        : real{real_}, mirror{std::make_unique<KeplerCompute>(real_.GetMemoryManager())} {
        Resync();
    }

    void Resync() {
        mirror->regs = real.regs;
        real.mt_written.Clear();
    }

    void ApplyPending() {
        pairs.resize(RegWriteSet::NumBits);
        const u32 count = real.mt_written.Drain(real.regs.reg_array.data(),
                                                KeplerCompute::Regs::NUM_REGS, pairs.data());
        MirrorApplier<KeplerCompute>::ApplyPairs(*mirror, pairs.data(), count);
        delta_words += count;
    }

    bool Compare() {
        ++checks;
        const auto& real_regs = real.regs.reg_array;
        const auto& mirror_regs = mirror->regs.reg_array;
        if (std::memcmp(real_regs.data(), mirror_regs.data(), sizeof(real_regs)) == 0) {
            return true;
        }
        u32 first = 0;
        u32 differing = 0;
        for (u32 i = KeplerCompute::Regs::NUM_REGS; i-- > 0;) {
            if (real_regs[i] != mirror_regs[i]) {
                first = i;
                ++differing;
            }
        }
        if (reported.size() < 256 && reported.insert(first).second) {
            LOG_WARNING(HW_GPU,
                        "MT mirror mismatch (compute check {}): register {:#x} real {:#x} mirror "
                        "{:#x} ({} differ)",
                        checks, first, real_regs[first], mirror_regs[first], differing);
        }
        ++mismatches;
        Resync();
        return false;
    }

    u64 checks = 0;
    u64 mismatches = 0;
    u64 delta_words = 0;

private:
    KeplerCompute& real;
    std::unique_ptr<KeplerCompute> mirror;
    std::vector<u64> pairs;
    std::unordered_set<u32> reported;
};

/// Called by the back at each compute dispatch (before it reads any state).
inline void MirrorCheckCompute(Tegra::Engines::KeplerCompute& real) {
    const u32 interval = MirrorInterval();
    if (interval == 0 || Active()) {
        return;
    }
    static std::unordered_map<const Tegra::Engines::KeplerCompute*, std::unique_ptr<MirrorCompute>>
        mirrors;
    static u64 dispatches = 0;
    static auto last_log = std::chrono::steady_clock::now();
    auto& slot = mirrors[&real];
    if (!slot) {
        slot = std::make_unique<MirrorCompute>(real);
        LOG_INFO(HW_GPU, "MT mirror: tracking compute engine {}", fmt::ptr(&real));
        return;
    }
    slot->ApplyPending();
    if (++dispatches % interval == 0) {
        slot->Compare();
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_log >= std::chrono::seconds{10}) {
        last_log = now;
        for (const auto& [engine, mirror] : mirrors) {
            LOG_INFO(HW_GPU, "MT mirror: compute engine {} {} checks, {} mismatches, {} delta words",
                     fmt::ptr(engine), mirror->checks, mirror->mismatches, mirror->delta_words);
        }
    }
}

} // namespace VideoCommon::MT
