// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/spin_recursive_mutex.h"
#include "video_core/mt/mt_switches.h"

/// Phase 6 of docs/mt-plan.md: the buffer cache's memory tracker and GPU-modified ranges get their
/// own lock (tracker_mutex), so the sequencer's captures and guest CPU write traps stop queueing
/// behind the committer's whole-draw hold of the buffer cache mutex.
///
/// Lock order: BufferCache::mutex, then tracker_mutex. Nothing that holds tracker_mutex takes
/// either cache mutex. The sequencer and the trap fast path only take tracker_mutex.
namespace VideoCommon::MT {

/// EDEN_NX2_MT_TRAPLOCK (on by default under EDEN_NX2_MT): the tracker lock above. Off, the
/// sequencer and the traps take the buffer cache mutex as in phase 4.
[[nodiscard]] inline bool TrapLock() {
    static const bool on = [] {
        static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_TRAPLOCK");
        return Enabled() && Tegra::Nx2Switch(mode, true);
    }();
    return on;
}

/// EDEN_NX2_MT_LOCKSCOPE (on by default under EDEN_NX2_MT): the committer holds the cache
/// mutexes only for a packet's uploads (buffer) and its resolve (Configure: both), not for the
/// dynamic state, query segment and recording that follow.
[[nodiscard]] inline bool LockScope() {
    static const bool on = [] {
        static const char mode = Tegra::Nx2EnvMode("EDEN_NX2_MT_LOCKSCOPE");
        return Enabled() && Tegra::Nx2Switch(mode, true);
    }();
    return on;
}

/// Holds the tracker lock (recursively) when EDEN_NX2_MT_TRAPLOCK is on; free otherwise.
class TrackerScope {
public:
    explicit TrackerScope(Common::SpinRecursiveMutex& tracker_mutex)
        : mutex{TrapLock() ? &tracker_mutex : nullptr} {
        if (mutex != nullptr) {
            mutex->lock();
        }
    }
    ~TrackerScope() {
        if (mutex != nullptr) {
            mutex->unlock();
        }
    }
    TrackerScope(const TrackerScope&) = delete;
    TrackerScope& operator=(const TrackerScope&) = delete;

private:
    Common::SpinRecursiveMutex* mutex;
};

} // namespace VideoCommon::MT
