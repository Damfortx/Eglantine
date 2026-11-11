// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <mutex>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/mt/lite_switches.h"
#include "video_core/nx2.h"

/// Cycle 11, workstream W2 "no back bubbles" (docs/cycle11-plan.md #2): channel binds and the
/// collision-map dispatch stop running as Serial items (the sequencer waiting until the committer
/// drained, then the committer idling until the sequencer refilled it). On by default under
/// EDEN_NX2_MT=1 EDEN_NX2_MT_LITE=1; EDEN_NX2_C11_<X>=0 turns one off ('1' turns it on without
/// LITE; nothing without EDEN_NX2_MT). Read once. With a switch at '0' the code path is the one
/// before cycle 11.
///
/// S2 audit (R17, docs/mt-phase4-spec.md: sequencer code reading channel state the committer
/// owns). With EDEN_NX2_C11_CHANCMD the sequencer switches at once: the rasterizer's own
/// ChannelSetupCaches pointers (maxwell3d, kepler_compute, gpu_memory, channel_state,
/// program_id) and the pipeline cache's (CurrentGraphicsPipeline / CurrentComputePipeline run on
/// the sequencer). The committer switches in stream order (a Command): texture / buffer / query
/// cache BindToChannel, bound_channel_id, its mirrors, the state tracker flags. Readers of the
/// sequencer-switched state on the committer, for work queued before the bind:
///   - rasterizer.maxwell3d / kepler_compute (ThreadEnginePtr, e.g. ConfigureDrawPipeline's
///     SetEngine(maxwell3d.Real())) and every other ThreadEnginePtr: the committer pins its view
///     to its current channel's mirrors (EngineOverrides::pinned, engine_ptr.h), so the stored
///     pointer, whichever channel it names, resolves to the committer's channel.
///   - rasterizer.gpu_memory (DispatchComputeBody, draws) and the pipeline cache's gpu_memory:
///     refused (Serial, "address space") unless both channels use the same MemoryManager, so the
///     pointer does not change.
///   - rasterizer.program_id (depth bias table): refused unless equal ("program").
///   - resolvers copy the channel at the Serial's barrier: refused when any runs ("resolvers").
///   - the query runtime's 3D engine (Bind3DEngine) is bound in the Command against the real
///     engine, as the Serial did.
///   - sequencer reads of committer-switched state: buffer_cache.MtSkipCacheSize reads the
///     buffer cache's channel_state (a heuristic for the staged/upload decision the committer
///     obeys; it may be the previous channel's value for the binds in flight), MtStorageBinding
///     translates through the buffer cache's gpu_memory (same MemoryManager, see above).
/// ReleaseChannel stays a Serial (rare).
namespace VideoCommon::MT {

/// EDEN_NX2_C11_SAMECHAN (S1): RasterizerVulkan::BindChannel to the channel already bound (same
/// engines and address space) is a no-op on both threads: no Serial, no cache rebinds and no
/// state_tracker.InvalidateState() (dirty tracking is per register, nothing changed).
[[nodiscard]] inline bool C11SameChan() {
    static const bool on = LiteDetail::Read("EDEN_NX2_C11_SAMECHAN");
    return on;
}

/// EDEN_NX2_C11_CHANCMD (S2): a real channel switch is an ordered committer Command (the
/// sequencer snapshots the new channel's engine state, see the audit above) instead of a Serial.
[[nodiscard]] inline bool C11ChanCmd() {
    static const bool on = LiteDetail::Read("EDEN_NX2_C11_CHANCMD");
    return on;
}

/// EDEN_NX2_C11_DISPATCH_CMD (S3): the collision-map program (it publishes its output image into
/// guest memory after it runs, texture_cache.PublishComputeCollisionImage) goes as a compute
/// packet instead of a forced Serial ("indirect/collision"). Its CB0 handle word and TIC entry
/// are captured, the publish runs inside the packet's commit against them, the front's reads
/// after it wait for the committer (NoteCommitterWrite, bulk). Guard: the sequencer's next guest
/// memory write (a payload report, I2M, DMA...) first waits until the committer ran the packet,
/// so no guest-visible write the old Serial ordered after the publish lands before it.
[[nodiscard]] inline bool C11DispatchCmd() {
    static const bool on = LiteDetail::Read("EDEN_NX2_C11_DISPATCH_CMD");
    return on;
}

/// EDEN_NX2_C11_DISPATCH_CMD_NOGUARD=1 (contingent, off; not part of LITE): S3 without the
/// sequencer write guard (the plan's version: only the front's reads wait). Use only if the test
/// round shows the guard waiting on most collision packets ("guard waits") and screenshots /
/// water collisions stay right without it.
[[nodiscard]] inline bool C11DispatchNoGuard() {
    static const bool on =
        Enabled() && Tegra::Nx2EnvMode("EDEN_NX2_C11_DISPATCH_CMD_NOGUARD") == '1';
    return on;
}

inline constexpr u64 C11CollisionMapProgram = 0x6e0ae200ee9aa220ULL;

/// Why a real channel switch still went as a Serial (S2).
enum class C11ChanRefuse : u32 {
    Off,          ///< EDEN_NX2_C11_CHANCMD=0
    Resolvers,    ///< resolvers copy the channel at the Serial's barrier
    NoChannel,    ///< nothing bound before, or the new channel has no engines
    AddressSpace, ///< another MemoryManager: rasterizer.gpu_memory would change under the committer
    Program,      ///< another program id
    Count,
};
inline constexpr std::array<const char*, static_cast<size_t>(C11ChanRefuse::Count)>
    C11ChanRefuseNames{"off", "resolvers", "no-channel", "address-space", "program"};

/// Counters (relaxed; sequencer/committer, read by the MT stats line every 10 s).
struct C11SerialStats {
    std::atomic<u64> bind_same{};      ///< S1: elided rebinds of the bound channel (sequencer)
    std::atomic<u64> bind_same_back{}; ///< S1: elided on the committer / single-thread path
    std::atomic<u64> bind_cmd{};       ///< S2: real switches queued as a Command
    std::array<std::atomic<u64>, static_cast<size_t>(C11ChanRefuse::Count)> bind_serial{};
    std::atomic<u64> mirror_created{}; ///< S2: committer created mirrors for a new channel
    std::atomic<u64> mirror_diff_regs{}; ///< S2: registers that differed at a rebind (expect 0)
    std::atomic<u64> collision_packets{}; ///< S3: collision dispatches sent as packets
    std::atomic<u64> collision_serial{};  ///< S3 off or capture refused: Serial
    std::atomic<u64> guard_checks{};      ///< S3: sequencer writes while a publish was pending
    std::atomic<u64> guard_waits{};       ///< ... of which waited for the committer
    std::atomic<u64> guard_wait_ns{};
};
inline C11SerialStats g_c11_serial{};

/// S3, sequencer-owned: stream position after the last collision packet whose publish the
/// sequencer's next guest write must wait for (0: none pending).
inline u64 g_c11_publish_position = 0;

/// The engine state a channel bind hands to the committer (S2): taken by the sequencer, which
/// owns the real engines, at the bind; the committer applies it to its mirrors in stream order.
struct C11ChannelSnapshot {
    using Maxwell3D = Tegra::Engines::Maxwell3D;
    using KeplerCompute = Tegra::Engines::KeplerCompute;
    Maxwell3D* real3d{};
    KeplerCompute* real_compute{};
    Maxwell3D::Regs regs{};
    decltype(Maxwell3D::state) state{};
    decltype(Maxwell3D::root_tables) root_tables{};
    Maxwell3D::DrawManager::State draw_state{};
    bool native_turing{};
    KeplerCompute::Regs compute_regs{};
    decltype(KeplerCompute::launch_description) launch_description{};
};

/// Start line (once, at the first channel bind or dispatch on the sequencer).
inline void C11SerialLogStart() {
    static std::once_flag once;
    std::call_once(once, [] {
        LOG_INFO(Render_Vulkan,
                 "Cycle 11 serial: samechan {}, chancmd {} (refused as Serial: resolvers, "
                 "no channel, other address space, other program id; ReleaseChannel stays "
                 "Serial), dispatchcmd {} (sequencer write guard {})",
                 C11SameChan(), C11ChanCmd(), C11DispatchCmd(),
                 C11DispatchCmd() && !C11DispatchNoGuard());
    });
}

} // namespace VideoCommon::MT
