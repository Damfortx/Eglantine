// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/memory.h"
#include "video_core/nx2_profile.h"
#include "video_core/dma_pusher.h"
#include "video_core/control/channel_state.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/guest_memory.h"
#include "video_core/memory_manager.h"
#include "video_core/mt/gpuparam.h"
#include "video_core/nx2.h"
#include "video_core/rasterizer_interface.h"

namespace Tegra {

namespace {
// EDEN_NX2_DUMP_GPU_LISTS=<file>: appends every command list read from GPU-written pages
// (u64 address, u32 word count, u32 path, words) to find which words the GPU fills in.
void DumpGpuWrittenList(u64 address, std::span<const Tegra::CommandHeader> commands, u32 path) {
    static std::FILE* const file = [] {
        const char* name = std::getenv("EDEN_NX2_DUMP_GPU_LISTS");
        return name != nullptr ? std::fopen(name, "wb") : nullptr;
    }();
    static u64 written{};
    if (file == nullptr || written > (u64{512} << 20)) {
        return;
    }
    const u32 size = static_cast<u32>(commands.size());
    std::fwrite(&address, sizeof(address), 1, file);
    std::fwrite(&size, sizeof(size), 1, file);
    std::fwrite(&path, sizeof(path), 1, file);
    std::fwrite(commands.data(), sizeof(u32), size, file);
    written += 16 + u64{size} * 4;
}
} // namespace


constexpr u32 MacroRegistersStart = 0xE00;
constexpr u32 ComputeInline = 0x6D;

namespace {
// Compute dispatches and DMA copies write the command lists ReadGpuWrittenCommands reads. The
// register writes that set them up only store a value: marking those too made every list fetch
// after one wait for all the work queued before it (draws included).
bool WritesMemory(Engines::EngineInterface* engine, Engines::EngineTypes type, u32 method,
                  u32 count) {
    // Inline uploads and 2D blits: every method may end in a memory write, as before.
    if (type == Engines::EngineTypes::KeplerMemory || type == Engines::EngineTypes::Fermi2D) {
        return true;
    }
    if (type == Engines::EngineTypes::KeplerCompute) {
        const auto* const compute = static_cast<const Engines::KeplerCompute*>(engine);
        for (u32 i = 0; i < count; ++i) {
            if (compute->MayWriteMemory(method + i)) {
                return true;
            }
        }
        return false;
    }
    if (type == Engines::EngineTypes::MaxwellDMA) {
        for (u32 i = 0; i < count; ++i) {
            const u32 current = method + i;
            if (current >= MacroRegistersStart ||
                (current < engine->execution_mask.size() && engine->execution_mask[current])) {
                return true;
            }
        }
    }
    return false;
}
} // Anonymous namespace

DmaPusher::DmaPusher(Core::System& system_, MemoryManager& memory_manager_, Control::ChannelState& channel_state_)
    : system{system_}
    , memory_manager{memory_manager_}
    , channel_state{channel_state_}
    , signal_sync{false}
    , synced{false}
{}

DmaPusher::~DmaPusher() = default;

void DmaPusher::DispatchCalls() {
    // The sub-index is kept: a channel suspended on a semaphore resumes inside its command list.
    dma_state.is_last_call = true;
    while (system.IsPoweredOn()) {
        if (!Step()) {
            break;
        }
    }
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
        // GPU params v2: a held group of draws goes out before the submission ends.
        split.ReleaseFrontHold(VideoCommon::GpuSplit::HoldDispatchEnd);
        split.Defer([gpu = &system.GPU()] {
            gpu->FlushCommands();
            gpu->OnCommandListEnd();
        });
        split.Publish();
        return;
    }
    system.GPU().FlushCommands();
    system.GPU().OnCommandListEnd();
}

bool DmaPusher::Step() {
    if (!ib_enable || dma_pushbuffer.empty()) {
        return false;
    }
    const u64 suspensions_before = suspensions.load(std::memory_order_relaxed);
    if (!puller.ResumeAcquire(*this)) {
        return false;
    }
    // Lists queued while the channel waited were read from memory, not as submitted: the game
    // writes the commands a list runs before it releases the acquire, so the words at submit time
    // could be older. Reading them as late as the parse is too late in Hyrule Warriors: Age of
    // Imprisonment fights: a command ring the game had already refilled for a later frame, read
    // as this frame's commands (methods on unbound subchannels, garbage QMDs, then a channel
    // waiting forever on a release it never got). The acquire is released now, so the commands
    // are written and the memory not yet reused: copy them here. EDEN_NX2_RESNAPSHOT=0: off.
    if (ResnapshotEnabled() && suspensions.load(std::memory_order_relaxed) != suspensions_before) {
        const u64 now = suspensions.load(std::memory_order_acquire);
        for (CommandList& list : dma_pushbuffer) {
            if (!list.submitted_offsets.empty()) {
                RefreshSnapshot(list, now);
            }
        }
    }

    CommandList& command_list = dma_pushbuffer.front();

    const size_t prefetch_size = command_list.prefetch_command_list.size();
    const size_t command_list_size = command_list.command_lists.size();

    if (prefetch_size == 0 && command_list_size == 0) {
        dma_pushbuffer.pop_front();
        dma_pushbuffer_subindex = 0;
        return true;
    }

    if (prefetch_size > 0) {
        const std::span<const CommandHeader> commands{command_list.prefetch_command_list};
        command_resume_offset +=
            ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        if (puller.HasPendingAcquire()) {
            return false;
        }
        command_resume_offset = 0;
        dma_pushbuffer.pop_front();
        return true;
    }

    auto& current_command = command_list.command_lists[dma_pushbuffer_subindex];
    const CommandListHeader& header = current_command;
    dma_state.dma_get = header.addr;

    if (signal_sync && !synced) {
        std::unique_lock lk(sync_mutex);
        sync_cv.wait(lk, [this]() { return synced; });
        signal_sync = false;
        synced = false;
    }

    if (header.size > 0) {
        if (subchannels[dma_state.subchannel] && dma_state.method_count) {
            const auto engine = subchannel_type[dma_state.subchannel];
            const bool kepler_payload = engine == Engines::EngineTypes::KeplerCompute && dma_state.method == ComputeInline && dma_state.non_incrementing;
            const bool macro_payload = engine == Engines::EngineTypes::Maxwell3D && dma_state.method >= MacroRegistersStart;
            if (kepler_payload || macro_payload) {
                const size_t words = std::min<size_t>(dma_state.method_count, header.size);
                auto* const engine_state = subchannels[dma_state.subchannel];
                if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
                    // The caches behind IsMemoryDirty belong to the split's back thread.
                    split.Defer([engine_state, memory = &memory_manager, address = GPUVAddr{dma_state.dma_get},
                                 bytes = words * sizeof(u32)] {
                        engine_state->current_dirty = memory->IsMemoryDirty(address, bytes);
                    });
                } else {
                    engine_state->current_dirty = memory_manager.IsMemoryDirty(dma_state.dma_get, words * sizeof(u32));
                }
            }
        }
        if (command_resume_offset == 0) {
            // A method's data continues across GP entries, whatever their level: title 4 ends a
            // main entry after a macro's first word and supplies the remaining parameters in a
            // 5-word subroutine entry. EDEN_NX2_PARK_SPLIT_METHODS restores the old guess (an
            // entry elsewhere is a call that starts clean; resume where the split one ended).
            static const bool park_split_methods = std::getenv("EDEN_NX2_PARK_SPLIT_METHODS") != nullptr;
            if (g_nx2_ampere) {
                if (static u32 split_logs{}; dma_state.method_count != 0 && split_logs < 64) {
                    ++split_logs;
                    LOG_DEBUG(HW_GPU, "Split method {:#x} ({} left) on ch {}: next entry {:#x} size {} raw {:#x} (level bit {}, priv bit {}), expected {:#x}, parked {}",
                             dma_state.method, dma_state.method_count, channel_state.bind_id,
                             u64{header.addr}, u64{header.size}, header.raw, u64{header.is_push_buffer},
                             u64{header.allow_flush}, split_method_end, parked_methods.size());
                    std::array<u32, 8> next_words{};
                    std::array<u32, 4> prev_words{};
                    memory_manager.ReadBlockUnsafe(header.addr, next_words.data(),
                                             std::min<size_t>(header.size, 8) * sizeof(u32));
                    memory_manager.ReadBlockUnsafe(split_method_end - sizeof(prev_words), prev_words.data(),
                                             sizeof(prev_words));
                    LOG_DEBUG(HW_GPU, "  previous entry ends {:08x}; next entry starts {:08x}",
                              fmt::join(prev_words, " "), fmt::join(next_words, " "));
                }
                if (park_split_methods) {
                    ResolveSplitMethod(header.addr);
                }
            }
            if (dma_pushbuffer_subindex < command_list.submit_hashes.size()) {
                CheckSubmitHash(header, command_list.submit_hashes[dma_pushbuffer_subindex]);
            }
        }
        // A resumed entry is read again: the words after the acquire may be new by now.
        // A safe read flushes the caches, which belong to the split's back thread while it runs
        // (the split only starts with unsafe reads; the settings can change mid-game).
        const bool use_safe = !VideoCommon::GpuSplit::Get().Active() &&
                              (Settings::IsDMALevelDefault() ? Settings::IsGPULevelHigh() : Settings::IsDMALevelSafe());
        // The split's back marks the pages GPU work writes as it runs that work: a page first
        // written by a dispatch or copy still queued for it reads as unwritten, and its words as
        // stale (Fast Fusion's culling fills in instanced draws' arguments; stale ones asked for
        // billions of instances and lost the device loading a race).
        bool gpu_written =
            IsGpuWrittenCommands(dma_state.dma_get, size_t{header.size} * sizeof(u32));
        bool writers_waited = false;
        if (!gpu_written && VideoCommon::GpuSplit::Get().Active() &&
            !IsKnownCpuCommandList(dma_state.dma_get, size_t{header.size} * sizeof(u32))) {
            // EDEN_NX2_MT_LITE_WRITERS: the committer only up to its queued guest writes (the
            // sequencer marks what the writers write at capture; GPU-written lists wait below).
            writers_waited = VideoCommon::GpuSplit::Get().WaitForWriters(
                VideoCommon::MT::LiteWritersSettle()
                    ? VideoCommon::GpuSplit::DrainReason::WritersCpuCommands
                    : VideoCommon::GpuSplit::DrainReason::WritersCommands);
            // EDEN_NX2_MT_LITE_KNOWNLIST: the writers were already done, which tells as much as
            // a wait for them: note the pages too (checked again, a writer may have been
            // captured since the first check), or no page is ever noted once waits stop waiting.
            if (!writers_waited && VideoCommon::MT::LiteKnownCpuLists()) {
                gpu_written =
                    IsGpuWrittenCommands(dma_state.dma_get, size_t{header.size} * sizeof(u32));
                if (!gpu_written) {
                    NoteCpuCommandList(dma_state.dma_get, size_t{header.size} * sizeof(u32));
                }
            }
        }
        if (writers_waited) {
            gpu_written =
                IsGpuWrittenCommands(dma_state.dma_get, size_t{header.size} * sizeof(u32));
            // EDEN_NX2_GPU_PROFILE: how often waiting for the writers found a GPU-written list.
            static u64 waited{}, found{};
            static auto window = std::chrono::steady_clock::now();
            ++waited;
            found += gpu_written ? 1 : 0;
            if (!gpu_written) {
                NoteCpuCommandList(dma_state.dma_get, size_t{header.size} * sizeof(u32));
            }
            if (VideoCommon::Nx2Profile::Enabled() &&
                std::chrono::steady_clock::now() - window > std::chrono::seconds{10}) {
                LOG_INFO(HW_GPU, "Writer waits: {} in 10 s, {} found GPU-written words", waited, found);
                waited = 0;
                found = 0;
                window = std::chrono::steady_clock::now();
            }
        }
        // The words as submitted, unless this is a resumed entry or the GPU has newer ones the
        // safe read below would flush (with the split, the front can't tell: it reads memory
        // unflushed anyway). Entries on pages the GPU writes (culling) always take the flushing
        // read: their words at submit time are an older frame's.
        // EDEN_NX2_MT_LITE_GPUPARAM / EDEN_NX2_GPUPARAM (mt/gpuparam.h): a GPU-written entry that
        // only carries the rest of a draw macro's parameters (NVN's indexed indirect draws) is
        // not read: the back draws indirectly from it. Entries it consumed after this one.
        int gpuparam_extra = -1;
        if (gpu_written && command_resume_offset == 0) {
            gpuparam_extra = TryGpuParamCall(command_list, header);
        }
        const bool submitted = command_resume_offset == 0 &&
                               dma_pushbuffer_subindex < command_list.submitted_offsets.size() &&
                               (command_list.snapshot_suspensions & 1) == 0 &&
                               command_list.snapshot_suspensions ==
                                   suspensions.load(std::memory_order_acquire) &&
                               !gpu_written &&
                               !(use_safe && memory_manager.IsMemoryDirty(dma_state.dma_get,
                                                                          header.size * sizeof(u32)));
        if (gpuparam_extra >= 0) {
            dma_pushbuffer_subindex += static_cast<size_t>(gpuparam_extra);
        } else if (submitted) {
            const std::span<const CommandHeader> commands{
                command_list.submitted_words.data() +
                    command_list.submitted_offsets[dma_pushbuffer_subindex],
                header.size};
            RecordParsedEntry(header, commands);
            command_resume_offset += ProcessCommands(commands, 0);
        } else if (use_safe) {
            Tegra::Memory::GpuGuestMemory<Tegra::CommandHeader, Tegra::Memory::GuestMemoryFlags::SafeRead>headers(memory_manager, dma_state.dma_get, header.size, &command_headers);
            const std::span<const CommandHeader> commands{headers.data(), headers.size()};
            RecordParsedEntry(header, commands);
            command_resume_offset +=
                ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        } else if (ReadGpuWrittenCommands(dma_state.dma_get, static_cast<u32>(header.size))) {
            const std::span<const CommandHeader> commands{command_headers.data(), header.size};
            RecordParsedEntry(header, commands);
            DumpGpuWrittenList(header.addr, commands, 1);
            if (VideoCommon::MT::ListDump::Enabled()) {
                RecordListDump(header, commands, 1);
            }
            command_resume_offset +=
                ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        } else {
            FlushGpuWrittenCommands(dma_state.dma_get, size_t{header.size} * sizeof(u32));
            Tegra::Memory::GpuGuestMemory<Tegra::CommandHeader, Tegra::Memory::GuestMemoryFlags::UnsafeRead>headers(memory_manager, dma_state.dma_get, header.size, &command_headers);
            const std::span<const CommandHeader> commands{headers.data(), headers.size()};
            RecordParsedEntry(header, commands);
            if (gpu_written) {
                DumpGpuWrittenList(header.addr, commands, 2);
                if (VideoCommon::MT::ListDump::Enabled()) {
                    RecordListDump(header, commands, 2);
                }
            }
            command_resume_offset +=
                ProcessCommands(commands.subspan(command_resume_offset), command_resume_offset);
        }
        if (puller.HasPendingAcquire()) {
            return false;
        }
        command_resume_offset = 0;
        if (dma_state.method_count != 0) {
            split_method_end = header.addr + header.size * sizeof(u32);
        }
    }

    if (++dma_pushbuffer_subindex >= command_list_size) {
        // A split method resumes within the submission that split it; ring addresses repeat
        // every frame, so a stale one could otherwise swallow the headers of a later entry.
        for (const auto& parked : parked_methods) {
            static std::atomic<u32> reported{};
            if (reported.fetch_add(1) < 16) {
                LOG_WARNING(HW_GPU,
                            "Submission ended with parked method {:#x} (sub {}, {} words left, "
                            "resume {:#x})",
                            parked.method, parked.subchannel, parked.method_count,
                            parked.resume_address);
            }
        }
        parked_methods.clear();
        dma_pushbuffer.pop_front();
        dma_pushbuffer_subindex = 0;
    } else {
        signal_sync = command_list.command_lists[dma_pushbuffer_subindex].sync && Settings::values.sync_memory_operations.GetValue();
    }

    if (signal_sync) {
        // SignalFence invalidates the GPU caches, which can record copies into the scheduler: the
        // back's state, which a drained back still submits from in its idle hook.
        VideoCommon::GpuSplit::Get().Run([this] {
            rasterizer->SignalFence([this]() {
                std::scoped_lock lk(sync_mutex);
                synced = true;
                sync_cv.notify_all();
            });
        }, VideoCommon::GpuSplit::DrainReason::Sync);
    }

    return true;
}

// EDEN_NX2_SEM_WATCH=<gpu address>: log every method whose argument is the low word of an
// address in the 16 bytes there (finds the engine and method that release a semaphore).
static const u32 g_semaphore_watch = [] {
    const char* value = std::getenv("EDEN_NX2_SEM_WATCH");
    return value ? static_cast<u32>(std::strtoull(value, nullptr, 16)) : 0U;
}();

// LOCAL TEST (Zelda): EDEN_NX2_FRONT_BATCH=1 (see ProcessCommands). On by default under
// EDEN_NX2_MT_LITE (EDEN_NX2_MT_LITE_FBATCH), which also sends single words this way.
static bool FrontBatchEnabled() {
    static const bool on = [] {
        const char mode = Tegra::Nx2EnvMode("EDEN_NX2_FRONT_BATCH");
        return mode != '0' && (Tegra::Nx2Switch(mode, false) || VideoCommon::MT::LiteFrontBatch());
    }();
    return on;
}
// The shortest run FrontWriteRun takes: one word under EDEN_NX2_MT_LITE_FBATCH (CallMethod's
// per-word dispatch costs more than the run call), else two as before.
static size_t FrontBatchMinimum() {
    static const size_t minimum = VideoCommon::MT::LiteFrontBatch() ? 1 : 2;
    return minimum;
}

// EDEN_NX2_PB_CHECK also keeps the last parsed entries, for DumpCurrentEntry.
static const bool g_record_parsed_entries = std::getenv("EDEN_NX2_PB_CHECK") != nullptr;

size_t DmaPusher::ProcessCommands(std::span<const CommandHeader> commands, size_t word_offset) {
    for (size_t index = 0; index < commands.size();) {
        // Data word of methods command
        if (dma_state.method_count && dma_state.non_incrementing) {
            auto const& command_header = commands[index]; //must ref (MUltiMethod re)
            dma_state.dma_word_offset = u64((word_offset + index) * sizeof(u32));
            u32 max_write = u32(std::min<std::size_t>(index + dma_state.method_count, commands.size()) - index);
            if (g_nx2_ampere && dma_state.method < non_puller_methods) {
                max_write = 1; // a semaphore acquire may suspend the channel after any word
            }
            CallMultiMethod(&command_header.argument, max_write);
            dma_state.method_count -= max_write;
            dma_state.is_last_call = true;
            index += max_write;
        } else if (dma_state.method_count) {
            // LOCAL TEST (Zelda), EDEN_NX2_FRONT_BATCH=1: with the split's front running the 3D
            // engine (FrontCallMethod), a run of incrementing register writes goes to it in one
            // call instead of CallMethod's per-word dispatch (engine lookup, writer and effect
            // checks, Front()). A 3D register write can't suspend the channel, so the acquire
            // check after the run is enough.
            if (!dma_state.non_incrementing && !dma_increment_once &&
                dma_state.method >= non_puller_methods && dma_state.method < MacroRegistersStart &&
                g_semaphore_watch == 0 && FrontBatchEnabled()) {
                auto& split = VideoCommon::GpuSplit::Get();
                auto* const subchannel = subchannels[dma_state.subchannel];
                if (subchannel != nullptr && split.Active() && split.FrontMacros() &&
                    subchannel_type[dma_state.subchannel] == Engines::EngineTypes::Maxwell3D) {
                    auto* const maxwell3d = static_cast<Engines::Maxwell3D*>(subchannel);
                    const size_t available =
                        (std::min)({size_t{dma_state.method_count}, commands.size() - index,
                                    size_t{MacroRegistersStart - dma_state.method}});
                    if (maxwell3d->GetNativeMacroEngine().native_turing &&
                        available >= FrontBatchMinimum()) {
                        const u32 count = static_cast<u32>(available);
                        const bool ends = count == dma_state.method_count;
                        dma_state.dma_word_offset = u64((word_offset + index + count - 1) * sizeof(u32));
                        dma_state.is_last_call = ends;
                        maxwell3d->FrontWriteRun(system, split, dma_state.method,
                                                 &commands[index].argument, count, ends);
                        dma_state.method += count;
                        dma_state.method_count -= count;
                        index += count;
                        continue;
                    }
                }
            }
            // Fast-path batching for consecutive non-execution methods in increasing mode
            if (!dma_state.non_incrementing && !dma_increment_once &&
                dma_state.method >= non_puller_methods &&
                !VideoCommon::GpuSplit::Get().Active() &&
                g_semaphore_watch == 0) {
                auto subchannel = subchannels[dma_state.subchannel];
                if (subchannel != nullptr) {
                    const size_t available = std::min<size_t>(dma_state.method_count, commands.size() - index);
                    u32 batch_count = 0;
                    while (batch_count < available &&
                           (dma_state.method + batch_count) < subchannel->execution_mask.size() &&
                           !subchannel->execution_mask[dma_state.method + batch_count]) {
                        ++batch_count;
                    }
                    if (batch_count > 1) {
                        subchannel->method_sink.reserve(subchannel->method_sink.size() + batch_count);
                        for (u32 i = 0; i < batch_count; ++i) {
                            subchannel->method_sink.emplace_back(dma_state.method + i, commands[index + i].argument);
                        }
                        dma_state.method += batch_count;
                        dma_state.method_count -= batch_count;
                        index += batch_count;
                        continue;
                    }
                }
            }

            auto const command_header = commands[index]; //can copy
            dma_state.dma_word_offset = u64((word_offset + index) * sizeof(u32));
            dma_state.is_last_call = dma_state.method_count <= 1;
            CallMethod(command_header.argument);
            dma_state.method += !dma_state.non_incrementing ? 1 : 0;
            dma_state.non_incrementing |= dma_increment_once;
            dma_state.method_count--;
            index++;
        } else {
            auto const command_header = commands[index]; //can copy
            // No command active - this is the first word of a new one
            switch (command_header.mode) {
            case SubmissionMode::Increasing:
                SetState(command_header);
                dma_state.non_incrementing = false;
                dma_increment_once = false;
                break;
            case SubmissionMode::NonIncreasing:
                SetState(command_header);
                dma_state.non_incrementing = true;
                dma_increment_once = false;
                break;
            case SubmissionMode::Inline:
                dma_state.method = command_header.method;
                dma_state.subchannel = command_header.subchannel;
                dma_state.dma_word_offset = u64(-s64(dma_state.dma_get)); // negate to set address as 0
                CallMethod(command_header.arg_count);
                dma_state.non_incrementing = true;
                dma_increment_once = false;
                break;
            case SubmissionMode::IncreaseOnce:
                SetState(command_header);
                dma_state.non_incrementing = false;
                dma_increment_once = true;
                break;
            default:
                break;
            }
            index++;
        }
        if (puller.HasPendingAcquire()) {
            return index;
        }
    }
    return commands.size();
}

// NVN on Switch 2 ends GPFIFO entries in the middle of a method's data and calls another command
// buffer (e.g. a compute dispatch) before the entry that carries the rest, which starts right
// where the first one ended. An entry starting anywhere else is such a call: run it with a clean
// method state and resume the unfinished method when the stream continues at that address.
void DmaPusher::ResolveSplitMethod(GPUVAddr entry_address) {
    const auto continued = std::ranges::find(parked_methods, entry_address,
                                             &ParkedMethod::resume_address);
    std::optional<ParkedMethod> resume;
    if (continued != parked_methods.end()) {
        resume = *continued;
        parked_methods.erase(continued);
    }
    if (dma_state.method_count != 0 && entry_address != split_method_end) {
        if (parked_methods.size() == MaxParkedMethods) {
            static std::atomic<u32> reported{};
            if (reported.fetch_add(1) < 8) {
                const auto& oldest = parked_methods.front();
                LOG_WARNING(HW_GPU,
                            "Dropping parked method {:#x} (sub {}, {} words left, resume {:#x})",
                            oldest.method, oldest.subchannel, oldest.method_count,
                            oldest.resume_address);
            }
            parked_methods.erase(parked_methods.begin());
        }
        parked_methods.push_back(ParkedMethod{
            .method = dma_state.method,
            .subchannel = dma_state.subchannel,
            .method_count = dma_state.method_count,
            .non_incrementing = dma_state.non_incrementing,
            .increment_once = dma_increment_once,
            .resume_address = split_method_end,
        });
        dma_state.method_count = 0;
        dma_increment_once = false;
    }
    if (resume && dma_state.method_count == 0) {
        dma_state.method = resume->method;
        dma_state.subchannel = resume->subchannel;
        dma_state.method_count = resume->method_count;
        dma_state.non_incrementing = resume->non_incrementing;
        dma_increment_once = resume->increment_once;
    }
}

void DmaPusher::SetState(const CommandHeader& command_header) {
    dma_state.method = command_header.method;
    dma_state.subchannel = command_header.subchannel;
    dma_state.method_count = command_header.method_count;
}


void DmaPusher::WatchArgument(u32 method, u32 argument) const {
    const u32 watch = g_semaphore_watch;
    if ((argument & ~0xfU) == watch) {
        LOG_INFO(HW_GPU, "Semaphore watch: channel {} subchannel {} method {:#x} argument {:#x}",
                 channel_state.bind_id, dma_state.subchannel, method, argument);
    }
}

bool DmaPusher::WantsSegment() const {
    // Only macro parameters and compute uploads read the engine's current_dma_segment.
    const auto engine = subchannel_type[dma_state.subchannel];
    return engine == Engines::EngineTypes::KeplerCompute ||
           (engine == Engines::EngineTypes::Maxwell3D && dma_state.method >= MacroRegistersStart);
}

void DmaPusher::CallMethod(u32 argument) {
    if (g_semaphore_watch != 0) [[unlikely]] {
        WatchArgument(dma_state.method, argument);
    }
    auto& split = VideoCommon::GpuSplit::Get();
    if (dma_state.method < non_puller_methods) {
        VideoCommon::Nx2Profile::Scope puller_scope{VideoCommon::Nx2Profile::Zone::PullerMethod};
        puller.CallPullerMethod(*this, Engines::Puller::MethodCall{
            dma_state.method,
            argument,
            dma_state.subchannel,
            dma_state.method_count,
        });
    } else {
        auto subchannel = subchannels[dma_state.subchannel];
        if (subchannel == nullptr) {
            ReportUnboundSubchannel();
            return;
        }
        // EDEN_NX2_MT_LITE_FCALL: the 3D front first (WritesMemory is false for Maxwell3D, so
        // nothing below applies to it but this call).
        if (VideoCommon::MT::LiteFrontCalls() &&
            subchannel_type[dma_state.subchannel] == Engines::EngineTypes::Maxwell3D &&
            split.Active() && split.FrontMacros()) {
            auto* const maxwell3d = static_cast<Engines::Maxwell3D*>(subchannel);
            if (maxwell3d->GetNativeMacroEngine().native_turing) {
                VideoCommon::Nx2Profile::Scope front_scope{VideoCommon::Nx2Profile::Zone::FrontMethod};
                maxwell3d->FrontCallMethod(system, split, dma_state.method, argument,
                                           dma_state.is_last_call);
                return;
            }
        }
        const bool writer = split.Active() &&
                            WritesMemory(subchannel, subchannel_type[dma_state.subchannel],
                                         dma_state.method, 1);
        const auto writer_kind = static_cast<u8>(subchannel_type[dma_state.subchannel]);
        SCOPE_EXIT {
            if (writer) {
                split.NoteWriter(writer_kind);
            }
        };
        if (split.Active()) {
            VideoCommon::Nx2Profile::Scope front_scope{VideoCommon::Nx2Profile::Zone::FrontMethod};
            const GPUVAddr segment = dma_state.dma_get + dma_state.dma_word_offset;
            if (split.FrontMacros()) {
                const auto type = subchannel_type[dma_state.subchannel];
                if (type == Engines::EngineTypes::Maxwell3D) {
                    auto* const maxwell3d = static_cast<Engines::Maxwell3D*>(subchannel);
                    if (maxwell3d->GetNativeMacroEngine().native_turing) {
                        maxwell3d->FrontCallMethod(system, split, dma_state.method, argument,
                                                   dma_state.is_last_call);
                        return;
                    }
                } else if (type == Engines::EngineTypes::KeplerCompute) {
                    auto* const compute = static_cast<Engines::KeplerCompute*>(subchannel);
                    if (compute->FrontRunsMacros()) {
                        compute->FrontCallMethod(system, split, dma_state.method, argument,
                                                 dma_state.is_last_call, &segment);
                        return;
                    }
                }
            }
            split.Method(subchannel, dma_state.method, argument, dma_state.is_last_call,
                         WantsSegment() ? &segment : nullptr);
            return;
        }
        if (!subchannel->execution_mask[dma_state.method]) {
            subchannel->method_sink.emplace_back(dma_state.method, argument);
        } else {
            VideoCommon::Nx2Profile::Scope method_scope{VideoCommon::Nx2Profile::Zone::Method};
            subchannel->ConsumeSink(system);
            subchannel->current_dma_segment = dma_state.dma_get + dma_state.dma_word_offset;
            subchannel->CallMethod(system, dma_state.method, argument, dma_state.is_last_call);
        }
    }
}

void DmaPusher::CallMultiMethod(const u32* base_start, u32 num_methods) {
    if (g_semaphore_watch != 0) [[unlikely]] {
        for (u32 i = 0; i < num_methods; ++i) {
            WatchArgument(dma_state.method + (dma_state.non_incrementing ? 0 : i), base_start[i]);
        }
    }
    auto& split = VideoCommon::GpuSplit::Get();
    if (dma_state.method < non_puller_methods) {
        VideoCommon::Nx2Profile::Scope puller_scope{VideoCommon::Nx2Profile::Zone::PullerMethod};
        puller.CallMultiMethod(*this, dma_state.method, dma_state.subchannel, base_start, num_methods, dma_state.method_count);
    } else {
        auto subchannel = subchannels[dma_state.subchannel];
        if (subchannel == nullptr) {
            ReportUnboundSubchannel();
            return;
        }
        // EDEN_NX2_MT_LITE_FCALL: as in CallMethod.
        if (VideoCommon::MT::LiteFrontCalls() &&
            subchannel_type[dma_state.subchannel] == Engines::EngineTypes::Maxwell3D &&
            split.Active() && split.FrontMacros()) {
            auto* const maxwell3d = static_cast<Engines::Maxwell3D*>(subchannel);
            if (maxwell3d->GetNativeMacroEngine().native_turing) {
                VideoCommon::Nx2Profile::Scope front_scope{VideoCommon::Nx2Profile::Zone::FrontMethod};
                maxwell3d->FrontCallMultiMethod(system, split, dma_state.method, base_start,
                                                num_methods, dma_state.method_count);
                return;
            }
        }
        const bool writer = split.Active() &&
                            WritesMemory(subchannel, subchannel_type[dma_state.subchannel],
                                         dma_state.method, dma_state.non_incrementing ? 1 : num_methods);
        const auto writer_kind = static_cast<u8>(subchannel_type[dma_state.subchannel]);
        SCOPE_EXIT {
            if (writer) {
                split.NoteWriter(writer_kind);
            }
        };
        if (split.Active()) {
            VideoCommon::Nx2Profile::Scope front_scope{VideoCommon::Nx2Profile::Zone::FrontMethod};
            const GPUVAddr segment = dma_state.dma_get + dma_state.dma_word_offset;
            if (split.FrontMacros()) {
                const auto type = subchannel_type[dma_state.subchannel];
                if (type == Engines::EngineTypes::Maxwell3D) {
                    auto* const maxwell3d = static_cast<Engines::Maxwell3D*>(subchannel);
                    if (maxwell3d->GetNativeMacroEngine().native_turing) {
                        maxwell3d->FrontCallMultiMethod(system, split, dma_state.method, base_start,
                                                        num_methods, dma_state.method_count);
                        return;
                    }
                } else if (type == Engines::EngineTypes::KeplerCompute) {
                    auto* const compute = static_cast<Engines::KeplerCompute*>(subchannel);
                    if (compute->FrontRunsMacros()) {
                        compute->FrontCallMultiMethod(system, split, dma_state.method, base_start,
                                                      num_methods, dma_state.method_count, &segment);
                        return;
                    }
                }
            }
            split.MultiMethod(subchannel, dma_state.method, base_start, num_methods,
                              dma_state.method_count, WantsSegment() ? &segment : nullptr);
            return;
        }
        VideoCommon::Nx2Profile::Scope method_scope{VideoCommon::Nx2Profile::Zone::Method};
        subchannel->ConsumeSink(system);
        subchannel->current_dma_segment = dma_state.dma_get + dma_state.dma_word_offset;
        subchannel->CallMultiMethod(system, dma_state.method, base_start, num_methods, dma_state.method_count);
    }
}

void DmaPusher::RefreshSnapshot(CommandList& list, u64 now) {
    list.snapshot_suspensions = now;
    for (size_t i = 0; i < list.command_lists.size() && i < list.submitted_offsets.size(); ++i) {
        const CommandListHeader& entry = list.command_lists[i];
        const u32 offset = list.submitted_offsets[i];
        if (offset + entry.size > list.submitted_words.size()) {
            continue;
        }
        memory_manager.ReadBlockUnsafe(entry.addr, list.submitted_words.data() + offset,
                                       entry.size * sizeof(u32));
    }
}


bool DmaPusher::IsGpuWrittenCommands(GPUVAddr address, size_t size) {
    if (!Tegra::g_nx2_ampere.load(std::memory_order_relaxed) || size == 0) {
        return false;
    }
    for (GPUVAddr page = address & ~GPUVAddr{0xfff}; page < address + size; page += 0x1000) {
        if (const auto device = memory_manager.GpuToCpuAddress(page);
            device && Tegra::Nx2IsGpuWritten(*device)) {
            return true;
        }
    }
    return false;
}

// Every entry not on GPU-written pages waited for the compute and copy work queued before it, in
// case that work writes the entry's pages for the first time (Fast Fusion's culling lists). Title
// 1 waited ~6,000 times a second and never found GPU-written words: each wait left the back with
// nothing queued while the front caught up, so the two threads took turns. Pages already read as
// CPU command lists after such a wait skip it: title 1's attract demo went from 33-50 fps to
// 56-59 (vsync off). Default for title 1 only (g_nx2_skip_known_writer_waits);
// EDEN_NX2_WRITER_WAIT=0 skips for every title, =1 always waits, =a switches every 20 s.
bool DmaPusher::IsKnownCpuCommandList(GPUVAddr address, size_t size) {
    static const char mode = [] {
        const char* value = std::getenv("EDEN_NX2_WRITER_WAIT");
        return value != nullptr ? value[0] : 'd';
    }();
    if (mode == '1' ||
        (mode == 'd' && !Tegra::g_nx2_skip_known_writer_waits.load(std::memory_order_relaxed))) {
        return false;
    }
    if (mode == 'a') {
        static bool skip = true;
        static auto toggled = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (now - toggled >= std::chrono::seconds{20}) {
            skip = !skip;
            toggled = now;
            LOG_INFO(HW_GPU, "Writer wait skip for known CPU command lists: {}", skip ? "on" : "off");
        }
        if (!skip) {
            return false;
        }
    }
    if (known_cpu_list_pages.empty()) {
        return false;
    }
    for (GPUVAddr page = address & ~GPUVAddr{0xfff}; page < address + size; page += 0x1000) {
        const auto device = memory_manager.GpuToCpuAddress(page);
        if (!device) {
            return false;
        }
        const u64 index = *device >> 12;
        if (index / 64 >= known_cpu_list_pages.size() ||
            ((known_cpu_list_pages[index / 64] >> (index % 64)) & 1) == 0) {
            return false;
        }
    }
    return true;
}

void DmaPusher::NoteCpuCommandList(GPUVAddr address, size_t size) {
    if (known_cpu_list_pages.empty()) {
        known_cpu_list_pages.resize((u64{1} << (36 - 12)) / 64);
    }
    for (GPUVAddr page = address & ~GPUVAddr{0xfff}; page < address + size; page += 0x1000) {
        if (const auto device = memory_manager.GpuToCpuAddress(page)) {
            const u64 index = *device >> 12;
            if (index / 64 < known_cpu_list_pages.size()) {
                known_cpu_list_pages[index / 64] |= u64{1} << (index % 64);
            }
        }
    }
}

bool DmaPusher::ReadGpuWrittenCommands(GPUVAddr address, u32 count) {
    // The pages are marked as command list pages: from the next write on, the buffer cache copies
    // them to host memory right after the dispatch or copy that writes them and submits. The
    // front then waits only for the back to run that work and for the GPU to finish the copy, not
    // for everything queued and a cache flush (FlushGpuWrittenCommands: ~400 GPU round trips a
    // second in a Mario Kart World race, during which the back had nothing to do). Pages
    // FlushGpuWrittenCommands flushed whole and no GPU work wrote since are read from guest
    // memory.
    if (!Tegra::g_nx2_ampere.load(std::memory_order_relaxed) || count == 0 ||
        rasterizer == nullptr) {
        return false;
    }
    if (VideoCommon::MT::ListDump::Enabled()) {
        list_dump.fetch.sampled = false;
    }
    const GPUVAddr end = address + u64{count} * sizeof(u32);
    bool written = false;
    bool new_page = false;
    // Device addresses of the list's pages (EDEN_NX2_MT_LITE_LISTPOS/LISTKNOWN, statistics).
    std::array<u64, 4> pages{};
    size_t page_count = 0;
    bool pages_fit = true;
    for (GPUVAddr page = address & ~GPUVAddr{0xfff}; page < end; page += 0x1000) {
        const auto device = memory_manager.GpuToCpuAddress(page);
        if (device && Tegra::Nx2IsGpuWritten(*device)) {
            new_page |= Tegra::Nx2MarkCommandListPage(*device);
            written = true;
        }
        if (!device || page_count == pages.size()) {
            pages_fit = false;
        } else {
            pages[page_count++] = *device & ~u64{0xfff};
        }
    }
    if (!written) {
        return false;
    }
    const std::span<const u64> list_pages{pages.data(), page_count};
    const auto page_bound = [list_pages] {
        u64 bound = 0;
        for (const u64 page : list_pages) {
            bound = std::max(bound, Tegra::Nx2ListWriterPosition(page));
        }
        return bound;
    };
    auto& split = VideoCommon::GpuSplit::Get();
    const bool mt = split.Active() && split.MtActive();
    // Statistics (EDEN_NX2_GPU_PROFILE, EDEN_NX2_LIST_STATS=1): positions before the wait.
    const bool stats = VideoCommon::Nx2Profile::Enabled() || VideoCommon::MT::ListStatsFull();
    const bool full_stats = VideoCommon::MT::ListStatsFull();
    // EDEN_NX2_LIST_DUMP: the same positions for a sampled fetch.
    const bool dump = VideoCommon::MT::ListDump::Enabled() && list_dump.Sample();
    u64 consumed_before = 0;
    u64 settled_before = 0;
    u64 writers_target = 0;
    std::array<u8, 4> state_before{};
    if (stats || dump) {
        if (split.Active()) {
            writers_target = split.WritersTarget();
            consumed_before = split.ConsumedPosition();
            settled_before = split.SettledPosition();
        }
        if (full_stats || dump) {
            for (size_t i = 0; i < page_count; ++i) {
                state_before[i] = rasterizer->PeekGpuWrittenCommandList(pages[i]);
            }
        }
    }
    if (stats) {
        auto& counters = list_stats;
        ++counters.fetches;
        counters.new_pages += new_page ? 1 : 0;
        counters.multi_page += page_count > 1 ? 1 : 0;
        if (full_stats) {
            for (size_t i = 0; i < page_count; ++i) {
                ++counters.state_before[std::min<size_t>(state_before[i], 4)];
            }
        }
    }
    const auto wait_start = dump ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};

    // EDEN_NX2_MT_LITE_LISTKNOWN (L25): every page trusted as a CPU list page, and no writer the
    // sequencer captured on them still uncommitted: no writer wait.
    // Same policy as IsKnownCpuCommandList: EDEN_NX2_WRITER_WAIT=1 never skips, =0 (and =a)
    // always may, the default only for the titles that skip known CPU lists.
    static const char writer_wait_mode = Tegra::Nx2EnvMode("EDEN_NX2_WRITER_WAIT");
    const bool skip_policy =
        writer_wait_mode == '0' || writer_wait_mode == 'a' ||
        (writer_wait_mode == 'd' &&
         Tegra::g_nx2_skip_known_writer_waits.load(std::memory_order_relaxed));
    bool skipped = false;
    bool shortened = false;
    bool waited = false;
    if (split.Active()) {
        if (mt && VideoCommon::MT::LiteListKnown() && !new_page && pages_fit && skip_policy &&
            std::ranges::all_of(list_pages,
                                [this](u64 page) { return list_trust.Trusted(page >> 12); }) &&
            split.SettledPosition() >= page_bound()) {
            skipped = true;
        } else if (mt && VideoCommon::MT::LiteListPos() && !new_page && pages_fit) {
            // EDEN_NX2_MT_LITE_LISTPOS (L24): the committer only up to these pages' writers.
            const auto wait = split.WaitForListWriters(page_bound);
            waited = wait.waited;
            shortened = wait.shortened;
        } else {
            waited = split.WaitForWriters();
        }
    }

    const u64 wait_ns =
        dump ? static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now() - wait_start)
                                    .count())
             : 0;

    command_headers.resize_destructive(count);
    std::array<u8, 4> results{};
    const std::span<u8> page_results{results.data(), page_count};
    u8* const out = reinterpret_cast<u8*>(command_headers.data());
    bool ok = ReadGpuWrittenChunks(address, count, out, page_results);
    if (skipped && (!ok || std::ranges::any_of(page_results, [](u8 r) { return r != 0; }))) {
        // A trusted page was written by the GPU after all (a committed write): never trust it
        // again, and read the list as before (every writer run).
        for (size_t i = 0; i < page_count; ++i) {
            if (list_trust.Written(pages[i] >> 12, results[i] == 1)) {
                ++list_stats.l25_banned;
            }
        }
        ++list_stats.l25_fallback;
        skipped = false;
        waited = split.WaitForWriters();
        results = {};
        ok = ReadGpuWrittenChunks(address, count, out, page_results);
    }

    if (stats) {
        auto& counters = list_stats;
        counters.l25_skipped += skipped ? 1 : 0;
        counters.l24_short += shortened ? 1 : 0;
        const bool behind = split.Active() && !skipped &&
                            (consumed_before < writers_target || settled_before < writers_target);
        if (behind) {
            ++counters.waited;
            const u64 bound = page_bound();
            const size_t reason = new_page                  ? 3
                                  : bound > consumed_before ? 0
                                  : bound > settled_before  ? 1
                                                            : 2;
            ++counters.reasons[reason];
            ++counters.writer_kind_waited[std::min<size_t>(split.LastWriterKind(), 6)];
            if (full_stats) {
                for (size_t i = 0; i < page_count; ++i) {
                    ++counters.state_before_waited[std::min<size_t>(state_before[i], 4)];
                    counters.NoteWaitedPage(pages[i] >> 12);
                }
            }
        }
        const bool copied = std::ranges::any_of(page_results, [](u8 r) { return r == 1; });
        ++counters.results[!ok ? 2 : copied ? 1 : 0];
    }

    if (dump) {
        auto& fetch = list_dump.fetch;
        fetch.ok = ok;
        fetch.new_page = new_page;
        fetch.waited = waited;
        fetch.skipped = skipped;
        fetch.shortened = shortened;
        fetch.page_count = static_cast<u8>(page_count);
        fetch.writer_kind = split.Active() ? split.LastWriterKind() : u8{6};
        fetch.results = results;
        fetch.state_before = state_before;
        fetch.page_bound = page_bound();
        fetch.writers_target = writers_target;
        fetch.consumed_before = consumed_before;
        fetch.settled_before = settled_before;
        fetch.wait_ns = wait_ns;
    }

    // L25 bookkeeping: a read with the writers waited for confirms or bans its pages.
    if (mt && VideoCommon::MT::LiteListKnown() && !skipped && pages_fit) {
        for (size_t i = 0; i < page_count; ++i) {
            const u64 page = pages[i] >> 12;
            if (ok && results[i] == 0) {
                if (list_trust.Confirm(page, Tegra::Nx2ListWriterPosition(pages[i]))) {
                    ++list_stats.l25_promoted;
                }
            } else if (results[i] != 0) {
                if (list_trust.Written(page, results[i] == 1)) {
                    ++list_stats.l25_banned;
                }
            }
        }
    }

    // EDEN_NX2_MT_LITE_LISTCHECK: read again after every writer ran and compare.
    if (ok && VideoCommon::MT::LiteListCheck() && (skipped || shortened)) {
        split.WaitForWriters();
        check_headers.resize_destructive(count);
        std::array<u8, 4> check_results{};
        if (ReadGpuWrittenChunks(address, count, reinterpret_cast<u8*>(check_headers.data()),
                                 std::span<u8>{check_results.data(), page_count})) {
            ++list_stats.check_compared;
            if (std::memcmp(check_headers.data(), command_headers.data(),
                            size_t{count} * sizeof(u32)) != 0) {
                if (list_stats.check_stale++ < 16) {
                    LOG_WARNING(HW_GPU,
                                "List check: stale GPU-written list {:#x} ({} words, {})",
                                address, count, skipped ? "L25 skip" : "L24 short settle");
                }
                std::memcpy(command_headers.data(), check_headers.data(),
                            size_t{count} * sizeof(u32));
            }
        } else {
            ++list_stats.check_unverifiable;
            ok = false;
        }
    }
    if (stats || VideoCommon::MT::LiteListCheck()) {
        list_stats.MaybeLog(stats);
    }
    (void)waited;
    return ok;
}

void DmaPusher::RecordListDump(const CommandListHeader& header,
                               std::span<const CommandHeader> commands, u32 path) {
    if (!list_dump.fetch.sampled) {
        return;
    }
    const CommandList& list = dma_pushbuffer.front();
    VideoCommon::MT::ListDump::Entry entry{};
    entry.address = header.addr;
    entry.header_raw = header.raw;
    entry.path = path;
    entry.channel = channel_state.bind_id;
    entry.subindex = static_cast<u32>(dma_pushbuffer_subindex);
    entry.entries = static_cast<u32>(list.command_lists.size());
    if (dma_pushbuffer_subindex > 0) {
        const CommandListHeader& prev = list.command_lists[dma_pushbuffer_subindex - 1];
        entry.prev_address = prev.addr;
        entry.prev_size = static_cast<u32>(prev.size);
    }
    if (dma_pushbuffer_subindex + 1 < list.command_lists.size()) {
        const CommandListHeader& next = list.command_lists[dma_pushbuffer_subindex + 1];
        entry.next_address = next.addr;
        entry.next_size = static_cast<u32>(next.size);
    }
    entry.resume_offset = static_cast<u32>(command_resume_offset);
    entry.frame = g_nx2_presents.load(std::memory_order_relaxed);
    Engines::Maxwell3D* maxwell3d = nullptr;
    for (u32 i = 0; i < max_subchannels; ++i) {
        entry.engines[i] =
            subchannels[i] != nullptr ? static_cast<u8>(subchannel_type[i]) : u8{0xff};
        if (maxwell3d == nullptr && subchannels[i] != nullptr &&
            subchannel_type[i] == Engines::EngineTypes::Maxwell3D) {
            maxwell3d = static_cast<Engines::Maxwell3D*>(subchannels[i]);
        }
    }
    if (dma_state.method_count != 0) {
        entry.carry_method = dma_state.method;
        entry.carry_subchannel = dma_state.subchannel;
        entry.carry_count = dma_state.method_count;
        entry.carry_mode =
            // 'O' only before its first word (after it the method no longer increments).
            dma_state.non_incrementing ? 'N' : dma_increment_once ? 'O' : 'I';
    }
    // Diagnostic read of the native (Turing) macro code: the macro index's start and the words
    // up to the next macro's start (at most 256 instructions).
    const auto macro = [maxwell3d](u32 index) -> std::pair<u32, std::span<const u32>> {
        if (maxwell3d == nullptr) {
            return {0, {}};
        }
        auto& engine = maxwell3d->GetNativeMacroEngine();
        const auto positions = maxwell3d->GetNativeMacroPositions();
        const u32 start = positions[index % positions.size()];
        if (!engine.native_turing) {
            return {start, {}};
        }
        u32 end = start + 256;
        for (const u32 position : positions) {
            if (position > start && position < end) {
                end = position;
            }
        }
        const size_t first = std::min<size_t>(size_t{start} * 3, engine.native_code.size());
        const size_t last = std::min<size_t>(size_t{end} * 3, engine.native_code.size());
        return {start, std::span<const u32>{engine.native_code.data() + first, last - first}};
    };
    list_dump.Record(entry,
                     std::span<const u32>{reinterpret_cast<const u32*>(commands.data()),
                                          commands.size()},
                     macro);
}

bool DmaPusher::ReadGpuWrittenChunks(GPUVAddr address, u32 count, u8* out,
                                     std::span<u8> results) {
    using GpuWrittenList = VideoCore::RasterizerInterface::GpuWrittenList;
    const GPUVAddr end = address + u64{count} * sizeof(u32);
    size_t index = 0;
    for (GPUVAddr chunk = address; chunk < end; ++index) {
        const GPUVAddr chunk_end = (std::min)(end, (chunk & ~GPUVAddr{0xfff}) + 0x1000);
        const std::span<u8> part{out + (chunk - address), static_cast<size_t>(chunk_end - chunk)};
        const auto device = memory_manager.GpuToCpuAddress(chunk);
        if (!device) {
            return false;
        }
        GpuWrittenList where = GpuWrittenList::Synced;
        if (Tegra::Nx2IsGpuWritten(*device)) {
            where = rasterizer->ReadGpuWrittenCommandList(*device, part);
        }
        if (index < results.size()) {
            results[index] = where == GpuWrittenList::Synced   ? 0
                             : where == GpuWrittenList::Copied ? 1
                                                               : 2;
        }
        if (where == GpuWrittenList::Unknown) {
            return false;
        }
        if (where == GpuWrittenList::Synced) {
            memory_manager.ReadBlockUnsafe(chunk, part.data(), part.size());
        }
        chunk = chunk_end;
    }
    return true;
}

void DmaPusher::FlushGpuWrittenCommands(GPUVAddr address, size_t size) {
    // Title 1 runs small command lists its culling dispatches fill in each frame. Accurate GPU
    // mode reads every command list through the caches (safe reads); here only the ones on pages
    // the GPU has written are, or their draws run with an older frame's words (trees vanish).
    // These are the lists ReadGpuWrittenCommands can't take from a copy or guest memory (a page's
    // first list, or pages draws write too).
    if (!IsGpuWrittenCommands(address, size)) {
        return;
    }
    // The caches belong to the split's back: it first runs what the front queued (the dispatch
    // that writes these words among it). The whole pages are flushed: their next lists read
    // guest memory until GPU work writes them again.
    VideoCommon::GpuSplit::Get().Drain(VideoCommon::GpuSplit::DrainReason::GpuWrittenCommands);
    const GPUVAddr first_page = address & ~GPUVAddr{0xfff};
    const GPUVAddr end_page = (address + size + 0xfff) & ~GPUVAddr{0xfff};
    if (memory_manager.IsMemoryDirty(first_page, end_page - first_page,
                                     VideoCommon::CacheType::BufferCache)) {
        // The download records into the Vulkan scheduler, which the split's back owns: a drained
        // back still submits from its idle hook, so the front must not flush these pages itself.
        VideoCommon::GpuSplit::Get().RunFlush(
            [memory = &memory_manager, first_page, size = end_page - first_page] {
                memory->FlushRegion(first_page, size, VideoCommon::CacheType::BufferCache);
            },
            VideoCommon::GpuSplit::DrainReason::GpuWrittenCommands);
    }
    if (rasterizer == nullptr) {
        return;
    }
    for (GPUVAddr page = first_page; page < end_page; page += 0x1000) {
        if (const auto device = memory_manager.GpuToCpuAddress(page);
            device && Tegra::Nx2IsCommandListPage(*device)) {
            rasterizer->NoteGpuWrittenCommandListSynced(*device);
        }
    }
}

void DmaPusher::ReportUnboundSubchannel() {
    // Methods sent to a subchannel with no engine (no SetObject yet) are dropped.
    static std::atomic<u32> reported{};
    if (reported.fetch_add(1) < 8) {
        LOG_WARNING(HW_GPU, "Dropping method {:#x} on unbound subchannel {}", dma_state.method,
                    dma_state.subchannel);
    }
}

void DmaPusher::CheckSubmitHash(const CommandListHeader& header, u64 submit_hash) {
    std::vector<u32> words(header.size);
    memory_manager.ReadBlockUnsafe(header.addr, words.data(), words.size() * sizeof(u32));
    static std::atomic<u64> checked{};
    static std::atomic<u64> changed{};
    ++checked;
    if (HashPushbufferWords(words) != submit_hash) {
        if (changed.fetch_add(1) < 16) {
            LOG_WARNING(HW_GPU, "pushbuffer entry {:#x}+{:#x} changed between submit and execution",
                        header.addr.Value(), header.size.Value());
        }
    }
    if ((checked % 20000) == 0) {
        LOG_WARNING(HW_GPU, "pushbuffer check: {} of {} entries changed after submit",
                    changed.load(), checked.load());
    }
}

void DmaPusher::RecordParsedEntry(const CommandListHeader& header,
                                  std::span<const CommandHeader> commands) {
    if (!g_record_parsed_entries) {
        return;
    }
    u64 hash = 0xcbf29ce484222325ULL;
    for (const CommandHeader& command : commands) {
        hash = (hash ^ command.argument) * 0x100000001b3ULL;
    }
    parsed_entries[parsed_entry_count++ % parsed_entries.size()] = {header.addr, static_cast<u32>(header.size), hash};
}

void DmaPusher::DumpCurrentEntry(const char* reason) {
    // Entries whose memory changed after they were parsed (the CPU rewrote them meanwhile).
    for (size_t i = 0; i < std::min(parsed_entry_count, parsed_entries.size()); ++i) {
        const auto& entry = parsed_entries[(parsed_entry_count - 1 - i) % parsed_entries.size()];
        std::vector<u32> words(entry.size);
        memory_manager.ReadBlockUnsafe(entry.address, words.data(), words.size() * sizeof(u32));
        u64 hash = 0xcbf29ce484222325ULL;
        for (const u32 word : words) {
            hash = (hash ^ word) * 0x100000001b3ULL;
        }
        LOG_WARNING(HW_GPU, "  parsed -{}: {:#x}+{:#x} {}", i, entry.address, entry.size,
                    hash == entry.hash ? "unchanged" : "CHANGED since parsed");
    }
    // EDEN_NX2_PB_DUMP=<file>: also write the whole command list (u64 address, u32 size and the
    // current words of every entry, then the current entry index) for offline replay.
    if (static const char* const path = std::getenv("EDEN_NX2_PB_DUMP");
        path != nullptr && !dma_pushbuffer.empty()) {
        if (std::FILE* file = std::fopen(path, "wb")) {
            const auto& lists = dma_pushbuffer.front().command_lists;
            const u64 current = dma_pushbuffer_subindex;
            const u64 count = lists.size();
            std::fwrite(&count, sizeof(count), 1, file);
            std::fwrite(&current, sizeof(current), 1, file);
            for (const CommandListHeader& header : lists) {
                const u64 address = header.addr;
                const u32 size = static_cast<u32>(header.size);
                std::vector<u32> words(size);
                memory_manager.ReadBlockUnsafe(address, words.data(), words.size() * sizeof(u32));
                std::fwrite(&address, sizeof(address), 1, file);
                std::fwrite(&size, sizeof(size), 1, file);
                std::fwrite(words.data(), sizeof(u32), words.size(), file);
            }
            std::fclose(file);
        }
    }
    LOG_WARNING(HW_GPU, "pushbuffer dump ({}): method={:#x} sub={} count={} non_inc={} get={:#x}",
                reason, dma_state.method, dma_state.subchannel, dma_state.method_count,
                dma_state.non_incrementing, dma_state.dma_get);
    if (dma_pushbuffer.empty()) {
        return;
    }
    const auto& lists = dma_pushbuffer.front().command_lists;
    const size_t first = dma_pushbuffer_subindex >= 2 ? dma_pushbuffer_subindex - 2 : 0;
    for (size_t i = first; i <= dma_pushbuffer_subindex && i < lists.size(); ++i) {
        const CommandListHeader& header = lists[i];
        LOG_WARNING(HW_GPU, "  entry {} of {}: addr={:#x} size={:#x}", i, lists.size(),
                    header.addr.Value(), header.size.Value());
        for (u64 word = 0; word < std::min<u64>(header.size, 0x80); word += 8) {
            std::string gpu_line;
            std::string cpu_line;
            for (u64 j = word; j < std::min<u64>(header.size, word + 8); ++j) {
                const GPUVAddr address = header.addr + j * sizeof(u32);
                const u32 gpu_value = memory_manager.Read<u32>(address);
                const u32 cpu_value = system.ApplicationMemory().Read32(address);
                gpu_line += fmt::format(" {:08x}", gpu_value);
                cpu_line += gpu_value == cpu_value ? std::string(" ........")
                                                   : fmt::format(" {:08x}", cpu_value);
            }
            LOG_WARNING(HW_GPU, "    +{:03x}:{}", word * 4, gpu_line);
            LOG_WARNING(HW_GPU, "    cpu :{}", cpu_line);
        }
    }
}

void DmaPusher::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
}

int DmaPusher::TryGpuParamCall(const CommandList& command_list, const CommandListHeader& header) {
    namespace GpuParam = VideoCommon::MT::GpuParam;
    if (dma_state.method_count == 0 || !dma_state.non_incrementing ||
        dma_state.method < MacroRegistersStart || header.size == 0 ||
        header.size > dma_state.method_count || dma_state.subchannel >= max_subchannels) {
        return -1;
    }
    auto* const subchannel = subchannels[dma_state.subchannel];
    if (subchannel == nullptr ||
        subchannel_type[dma_state.subchannel] != Engines::EngineTypes::Maxwell3D) {
        return -1;
    }
    auto& split = VideoCommon::GpuSplit::Get();
    if (!GpuParam::On(split.Active() && split.FrontMacros() && split.MtActive())) {
        return -1;
    }
    auto* const maxwell3d = static_cast<Engines::Maxwell3D*>(subchannel);
    if (!maxwell3d->GetNativeMacroEngine().native_turing) {
        return -1;
    }
    // This entry and the next ones of the submission until the call's last word, all of them
    // parameters only and on GPU-written pages (a draw count entry, then the records).
    std::array<GpuParam::Segment, 4> segments{};
    size_t count = 0;
    segments[count++] = {GPUVAddr{header.addr}, static_cast<u32>(header.size)};
    u32 remaining = dma_state.method_count - static_cast<u32>(header.size);
    size_t next = dma_pushbuffer_subindex + 1;
    while (remaining != 0) {
        if (count == segments.size() || next >= command_list.command_lists.size()) {
            return -1;
        }
        const CommandListHeader& entry = command_list.command_lists[next];
        const u32 size = static_cast<u32>(entry.size);
        if (size == 0 || size > remaining || entry.sync ||
            !IsGpuWrittenCommands(entry.addr, size_t{size} * sizeof(u32))) {
            return -1;
        }
        segments[count++] = {GPUVAddr{entry.addr}, size};
        remaining -= size;
        ++next;
    }
    Engines::Maxwell3D::GpuParamReader reader;
    if (GpuParam::CheckOn()) {
        reader = [this](u64 address, u32 words, u32* out) {
            return ReadGpuWrittenWords(address, words, out);
        };
    }
    if (!maxwell3d->GpuParamCall(system, dma_state.method, segments.data(), count,
                                 GpuParam::CheckOn() ? &reader : nullptr)) {
        return -1;
    }
    dma_state.method_count = 0;
    return static_cast<int>(count - 1);
}

bool DmaPusher::ReadGpuWrittenWords(GPUVAddr address, u32 count, u32* out) {
    // As Step reads a GPU-written entry: the copy/guest read after the writer wait, else a flush.
    if (ReadGpuWrittenCommands(address, count)) {
        std::memcpy(out, command_headers.data(), size_t{count} * sizeof(u32));
        return true;
    }
    FlushGpuWrittenCommands(address, size_t{count} * sizeof(u32));
    memory_manager.ReadBlockUnsafe(address, out, size_t{count} * sizeof(u32));
    return true;
}

} // namespace Tegra
