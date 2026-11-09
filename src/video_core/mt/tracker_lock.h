// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/spin_recursive_mutex.h"
#include "video_core/mt/lite7_capture_switches.h"
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

/// EDEN_NX2_MT_LITE7_LOCKSKIP: the tracker mutex the calling thread holds through a
/// TrackerScope or a HeldTrackerNote (the innermost one; restored as they end), else null.
inline thread_local const Common::SpinRecursiveMutex* t_held_tracker = nullptr;

/// Notes a mutex the calling thread has just locked (and keeps until this note ends) for
/// EDEN_NX2_MT_LITE7_LOCKSKIP; the sequencer's SeqTrackerLock uses it. Strictly nested.
class HeldTrackerNote {
public:
    explicit HeldTrackerNote(const Common::SpinRecursiveMutex* mutex) : previous{t_held_tracker} {
        t_held_tracker = mutex;
    }
    ~HeldTrackerNote() {
        t_held_tracker = previous;
    }
    HeldTrackerNote(const HeldTrackerNote&) = delete;
    HeldTrackerNote& operator=(const HeldTrackerNote&) = delete;

private:
    const Common::SpinRecursiveMutex* previous;
};

/// Holds the tracker lock (recursively) when EDEN_NX2_MT_TRAPLOCK is on; free otherwise.
/// EDEN_NX2_MT_LITE7_LOCKSKIP: nested in a scope of the same thread on the same mutex, it does
/// not lock again (the outer scope holds it until after this one ends).
class TrackerScope {
public:
    explicit TrackerScope(Common::SpinRecursiveMutex& tracker_mutex)
        : mutex{TrapLock() ? &tracker_mutex : nullptr} {
        if (mutex == nullptr) {
            return;
        }
        static const char skip_mode = Lite7LockSkipMode();
        if (Lite7Detail::On(skip_mode)) {
            if (t_held_tracker == mutex) {
                if (Lite7Stats()) [[unlikely]] {
                    g_lite7_lock_skips.fetch_add(1, std::memory_order_relaxed);
                }
                if (Lite7CaptureCheck() && !mutex->HeldByCurrentThread()) [[unlikely]] {
                    g_lite7_lock_skip_mismatches.fetch_add(1, std::memory_order_relaxed);
                    mutex->lock(); // stay correct: lock as before
                    return;
                }
                mutex = nullptr;
                return;
            }
            mutex->lock();
            noted = true;
            previous = t_held_tracker;
            t_held_tracker = mutex;
            return;
        }
        mutex->lock();
    }
    ~TrackerScope() {
        if (mutex == nullptr) {
            return;
        }
        if (noted) {
            t_held_tracker = previous;
        }
        mutex->unlock();
    }
    TrackerScope(const TrackerScope&) = delete;
    TrackerScope& operator=(const TrackerScope&) = delete;

private:
    Common::SpinRecursiveMutex* mutex;
    const Common::SpinRecursiveMutex* previous = nullptr;
    bool noted = false;
};

} // namespace VideoCommon::MT
