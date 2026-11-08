// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// EDEN_NX2_WAIT_STATS (set to anything): measures how much wall time the application's guest
// threads spend blocked in the kernel, by what they wait on, and how long HLE service handlers
// take per command; logs the totals every 10 s. Off, each instrumented SVC pays one load of
// g_enabled. Header-only so no build files change.
//
// Accumulation is per host thread (a shard with its own std::mutex, contended only while the
// report swaps it out). Guest threads are fibers that can resume on another host thread, so every
// thread_local access sits in a YUZU_NO_INLINE function that never switches fibers.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "core/arm/debug.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_readable_event.h"
#include "core/hle/kernel/k_synchronization_object.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/service/kernel_helpers.h"

namespace Kernel::WaitStats {

inline const bool g_enabled = std::getenv("EDEN_NX2_WAIT_STATS") != nullptr;

enum class Category : u8 {
    Sync,    ///< svcWaitSynchronization, by object(s)
    CondVar, ///< svcWaitProcessWideKeyAtomic, by cv key
    Lock,    ///< svcArbitrateLock, by mutex address
    Arbiter, ///< svcWaitForAddress, by address
    Sleep,   ///< svcSleepThread(ns > 0), by requested ns
    Yield,   ///< svcSleepThread(0/-1/-2)
    Ipc,     ///< svcSendSyncRequest*, by the HLE command that served it
    Receive, ///< svcReplyAndReceive* (guest-side servers)
    Hle,     ///< host time inside HLE handlers (service thread), by command; overlaps Ipc
    Count,
};
inline constexpr std::array<const char*, static_cast<size_t>(Category::Count)> CategoryNames{
    "sync", "condvar", "lock", "arbiter", "sleep", "yield", "ipc", "recv", "hle"};

using Clock = std::chrono::steady_clock;

namespace Detail {

struct Key {
    Category category;
    u64 tid;
    u64 id;
    bool operator==(const Key&) const = default;
};
struct KeyHash {
    size_t operator()(const Key& k) const noexcept {
        u64 h = k.id * 0x9E3779B97F4A7C15ULL;
        h ^= (k.tid + 0x632BE59BD9B4E019ULL + (h << 6) + (h >> 2));
        h ^= static_cast<u64>(k.category) * 0xC2B2AE3D27D4EB4FULL;
        return static_cast<size_t>(h);
    }
};
struct Entry {
    s64 ns{};
    u64 count{};
    u64 timeouts{};
    s64 max_ns{};
    std::string label;
    std::string thread; ///< guest thread description
};
using Map = std::unordered_map<Key, Entry, KeyHash>;

struct Shard {
    std::mutex mutex;
    Map map;
};

/// What the HLE handler that serves a guest thread's request recorded, so the guest side can
/// label its ipc wait. Pointers are to static strings (service and handler names).
struct IpcLabel {
    const char* service{};
    const char* function{};
    u32 command{};
    u32 sub{};
    bool has_sub{};
};

struct Global {
    std::mutex mutex; ///< shard registry and reporting
    std::vector<std::shared_ptr<Shard>> shards;
    std::atomic<s64> last_log{Clock::now().time_since_epoch().count()};
    std::mutex ipc_mutex;
    std::unordered_map<const void*, IpcLabel> ipc_labels; ///< by requesting KThread*
};

inline Global& Get() {
    // Leaked on purpose: guest and service threads may record during static destruction.
    static Global* const global = new Global;
    return *global;
}

YUZU_NO_INLINE inline Shard& LocalShard() {
    thread_local std::shared_ptr<Shard> shard = [] {
        auto created = std::make_shared<Shard>();
        Global& global = Get();
        std::scoped_lock lock{global.mutex};
        global.shards.push_back(created);
        return created;
    }();
    return *shard;
}

/// The handler's sub label (nvdrv ioctl number), set on the service thread.
struct SubLabel {
    u32 value{};
    bool set{};
};
YUZU_NO_INLINE inline SubLabel& LocalSubLabel() {
    thread_local SubLabel sub;
    return sub;
}

inline std::string DescribeThread(const KThread* thread) {
    if (thread == nullptr) {
        return "?";
    }
    std::string name;
    if (thread->GetOwnerProcess() != nullptr) {
        if (auto guest_name = Core::GetThreadName(thread)) {
            name = std::move(*guest_name);
        }
    }
    return fmt::format("{}{}{}#{} p{}", name.empty() ? "" : "'", name, name.empty() ? "" : "' ",
                       thread->GetThreadId(), thread->GetPriority());
}

inline double PerSecond(double value, double seconds) {
    return seconds > 0 ? value / seconds : 0.0;
}

inline void Report(s64 previous, s64 now) {
    Global& global = Get();
    std::scoped_lock lock{global.mutex};
    Map merged;
    for (const auto& shard : global.shards) {
        Map taken;
        {
            std::scoped_lock shard_lock{shard->mutex};
            taken.swap(shard->map);
        }
        for (auto& [key, entry] : taken) {
            auto [it, inserted] = merged.try_emplace(key, std::move(entry));
            if (!inserted) {
                it->second.ns += entry.ns;
                it->second.count += entry.count;
                it->second.timeouts += entry.timeouts;
                it->second.max_ns = std::max(it->second.max_ns, entry.max_ns);
            }
        }
    }
    const double seconds =
        std::chrono::duration<double>(Clock::duration{now - previous}).count();
    const auto ms_s = [&](s64 ns) { return PerSecond(static_cast<double>(ns) / 1e6, seconds); };
    const auto n_s = [&](u64 n) { return PerSecond(static_cast<double>(n), seconds); };

    constexpr size_t NumCategories = static_cast<size_t>(Category::Count);
    std::array<s64, NumCategories> cat_ns{};
    std::array<u64, NumCategories> cat_count{};
    // (category, label) -> totals and the thread that waited most on it.
    struct LabelTotals {
        s64 ns{};
        u64 count{};
        u64 timeouts{};
        s64 max_ns{};
        std::map<std::string, s64> threads;
    };
    std::map<std::pair<size_t, std::string>, LabelTotals> by_label;
    struct ThreadTotals {
        s64 ns{};
        std::string name;
        std::vector<std::pair<s64, std::string>> parts;
    };
    std::map<u64, ThreadTotals> by_thread;
    for (const auto& [key, entry] : merged) {
        const size_t cat = static_cast<size_t>(key.category);
        cat_ns[cat] += entry.ns;
        cat_count[cat] += entry.count;
        LabelTotals& label = by_label[{cat, entry.label}];
        label.ns += entry.ns;
        label.count += entry.count;
        label.timeouts += entry.timeouts;
        label.max_ns = std::max(label.max_ns, entry.max_ns);
        label.threads[entry.thread] += entry.ns;
        if (key.category != Category::Hle) {
            ThreadTotals& thread = by_thread[key.tid];
            thread.ns += entry.ns;
            thread.name = entry.thread;
            thread.parts.emplace_back(entry.ns, fmt::format("{} {}", CategoryNames[cat], entry.label));
        }
    }

    std::string header;
    for (size_t cat = 0; cat < NumCategories; ++cat) {
        header += fmt::format("{}{} {:.1f}/{:.0f}", cat == 0 ? "" : " | ", CategoryNames[cat],
                              ms_s(cat_ns[cat]), n_s(cat_count[cat]));
    }
    LOG_INFO(Kernel, "WaitStats {:.1f}s, app guest threads, blocked ms/s / waits/s: {}", seconds,
             header);

    for (size_t cat = 0; cat < NumCategories; ++cat) {
        if (cat_count[cat] == 0) {
            continue;
        }
        std::vector<std::pair<s64, std::string>> lines;
        for (const auto& [id, totals] : by_label) {
            if (id.first != cat) {
                continue;
            }
            const auto top = std::ranges::max_element(
                totals.threads, [](const auto& a, const auto& b) { return a.second < b.second; });
            lines.emplace_back(
                totals.ns,
                fmt::format("{} {:.1f}ms/s {:.0f}/s avg {:.0f}us max {:.1f}ms{} [{} {:.0f}%{}]",
                            id.second, ms_s(totals.ns), n_s(totals.count),
                            static_cast<double>(totals.ns) / 1e3 /
                                static_cast<double>(std::max<u64>(totals.count, 1)),
                            static_cast<double>(totals.max_ns) / 1e6,
                            totals.timeouts ? fmt::format(" timeouts {:.0f}/s", n_s(totals.timeouts))
                                            : std::string{},
                            top->first,
                            totals.ns ? 100.0 * static_cast<double>(top->second) /
                                            static_cast<double>(totals.ns)
                                      : 0.0,
                            totals.threads.size() > 1
                                ? fmt::format(" +{} thr", totals.threads.size() - 1)
                                : std::string{}));
        }
        std::ranges::sort(lines, [](const auto& a, const auto& b) { return a.first > b.first; });
        const size_t limit = cat == static_cast<size_t>(Category::Hle) ? 10 : 6;
        if (cat == static_cast<size_t>(Category::Hle)) {
            for (size_t i = 0; i < std::min(lines.size(), limit); ++i) {
                LOG_INFO(Kernel, "WaitStats  hle #{}: {}", i + 1, lines[i].second);
            }
            continue;
        }
        std::string text;
        for (size_t i = 0; i < std::min(lines.size(), limit); ++i) {
            text += fmt::format("{}{}", i == 0 ? "" : " ; ", lines[i].second);
        }
        LOG_INFO(Kernel, "WaitStats  {} ({} kinds): {}", CategoryNames[cat], lines.size(), text);
    }

    std::vector<std::pair<u64, ThreadTotals*>> threads;
    for (auto& [tid, totals] : by_thread) {
        threads.emplace_back(tid, &totals);
    }
    std::ranges::sort(threads, [](const auto& a, const auto& b) { return a.second->ns > b.second->ns; });
    for (size_t i = 0; i < std::min<size_t>(threads.size(), 16); ++i) {
        ThreadTotals& totals = *threads[i].second;
        std::ranges::sort(totals.parts, [](const auto& a, const auto& b) { return a.first > b.first; });
        std::string text;
        for (size_t p = 0; p < std::min<size_t>(totals.parts.size(), 3); ++p) {
            text += fmt::format(" | {} {:.1f}", totals.parts[p].second, ms_s(totals.parts[p].first));
        }
        LOG_INFO(Kernel, "WaitStats  thread {}: blocked {:.1f}ms/s{}", totals.name,
                 ms_s(totals.ns), text);
    }
}

template <typename MakeLabel>
YUZU_NO_INLINE void Record(Category category, const KThread* thread, u64 id, Clock::duration wait,
                           bool timed_out, MakeLabel&& make_label) {
    const Key key{category, thread != nullptr ? thread->GetThreadId() : 0, id};
    const s64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(wait).count();
    Shard& shard = LocalShard();
    {
        std::unique_lock lock{shard.mutex};
        auto it = shard.map.find(key);
        if (it == shard.map.end()) {
            // Resolve names outside the shard lock (they read guest memory / take other locks).
            lock.unlock();
            Entry entry;
            entry.label = make_label();
            entry.thread = DescribeThread(thread);
            lock.lock();
            it = shard.map.try_emplace(key, std::move(entry)).first;
        }
        Entry& entry = it->second;
        entry.ns += ns;
        ++entry.count;
        entry.timeouts += timed_out ? 1 : 0;
        entry.max_ns = std::max(entry.max_ns, ns);
    }
    Global& global = Get();
    const s64 now = Clock::now().time_since_epoch().count();
    s64 last = global.last_log.load(std::memory_order_relaxed);
    if (Clock::duration{now - last} >= std::chrono::seconds{10} &&
        global.last_log.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        Report(last, now);
    }
}

} // namespace Detail

/// Whether the current guest thread belongs to the application (and stats are on).
inline bool Active(KernelCore& kernel) {
    if (!g_enabled) {
        return false;
    }
    const KProcess* process = GetCurrentProcessPointer(kernel);
    return process != nullptr && process->IsApplication();
}

inline std::string DescribeObject(const KSynchronizationObject* object) {
    if (object == nullptr) {
        return "null";
    }
    if (const auto* event = object->DynamicCast<const KReadableEvent*>()) {
        std::string name = Service::KernelHelpers::DebugEventName(event);
        if (name != "?") {
            return "ev:" + name;
        }
        const KEvent* parent = event->GetParent();
        const KProcess* owner = parent != nullptr ? parent->GetOwner() : nullptr;
        return fmt::format("ev:unnamed@{}({:#x})", owner != nullptr ? owner->GetName() : "?",
                           reinterpret_cast<uintptr_t>(event) & 0xFFFFFF);
    }
    if (const auto* thread = object->DynamicCast<const KThread*>()) {
        return fmt::format("thread-exit#{}", thread->GetThreadId());
    }
    if (const auto* process = object->DynamicCast<const KProcess*>()) {
        return fmt::format("process:{}", process->GetName());
    }
    return object->GetTypeObj().GetName();
}

/// svcWaitSynchronization: the label lists every object waited on.
inline void RecordSync(KernelCore& kernel, KSynchronizationObject* const* objects, s32 count,
                       Clock::duration wait, bool timed_out) {
    u64 id = 0x51;
    for (s32 i = 0; i < count; ++i) {
        id = (id ^ reinterpret_cast<uintptr_t>(objects[i])) * 0x100000001B3ULL;
    }
    Detail::Record(Category::Sync, GetCurrentThreadPointer(kernel), id, wait, timed_out, [&] {
        if (count == 0) {
            return std::string{"(no handles)"};
        }
        std::string label;
        for (s32 i = 0; i < count; ++i) {
            label += (i == 0 ? "" : "|") + DescribeObject(objects[i]);
        }
        return label;
    });
}

/// Address-keyed waits (condvar, lock, arbiter, sleep, yield, recv).
inline void RecordAddress(KernelCore& kernel, Category category, u64 address, Clock::duration wait,
                          bool timed_out, const char* what) {
    Detail::Record(category, GetCurrentThreadPointer(kernel), address, wait, timed_out,
                   [&] {
                       if (category == Category::Sleep || category == Category::Yield ||
                           category == Category::Receive) {
                           return fmt::format("{}{}", what, static_cast<s64>(address));
                       }
                       return fmt::format("{}{:#x}", what, address);
                   });
}

/// svcSendSyncRequest: labels the wait with what the HLE handler recorded for this thread.
inline void RecordIpc(KernelCore& kernel, Clock::duration wait) {
    KThread* thread = GetCurrentThreadPointer(kernel);
    Detail::IpcLabel ipc{};
    {
        Detail::Global& global = Detail::Get();
        std::scoped_lock lock{global.ipc_mutex};
        if (const auto it = global.ipc_labels.find(thread); it != global.ipc_labels.end()) {
            ipc = it->second;
            global.ipc_labels.erase(it);
        }
    }
    const u64 id = (reinterpret_cast<uintptr_t>(ipc.service) * 0x9E3779B97F4A7C15ULL) ^
                   (static_cast<u64>(ipc.command) << 40) ^ ipc.sub ^ (ipc.has_sub ? 1ULL << 63 : 0);
    Detail::Record(Category::Ipc, thread, id, wait, false, [&] {
        if (ipc.service == nullptr) {
            return std::string{"(non-HLE or unknown session)"};
        }
        return fmt::format("{}::{} {}{}", ipc.service, ipc.command,
                           ipc.function ? ipc.function : "?",
                           ipc.has_sub ? fmt::format(" ioctl {:#010x}", ipc.sub) : std::string{});
    });
}

/// Service thread, before the handler: clears the sub label.
inline void HleBegin() {
    Detail::LocalSubLabel() = {};
}

/// Service thread, inside a handler: a finer label (nvdrv ioctl number).
inline void SetSubLabel(u32 value) {
    if (!g_enabled) {
        return;
    }
    Detail::LocalSubLabel() = {value, true};
}

/// Service thread, after the handler: records the handler time and labels the requester's wait.
inline void HleEnd(KThread* requester, const char* service, u32 command, const char* function,
                   Clock::duration time) {
    if (requester == nullptr || requester->GetOwnerProcess() == nullptr ||
        !requester->GetOwnerProcess()->IsApplication()) {
        return;
    }
    const Detail::SubLabel sub = Detail::LocalSubLabel();
    const Detail::IpcLabel ipc{service, function, command, sub.value, sub.set};
    {
        Detail::Global& global = Detail::Get();
        std::scoped_lock lock{global.ipc_mutex};
        global.ipc_labels[requester] = ipc;
    }
    const u64 id = (reinterpret_cast<uintptr_t>(service) * 0x9E3779B97F4A7C15ULL) ^
                   (static_cast<u64>(command) << 40) ^ sub.value ^ (sub.set ? 1ULL << 63 : 0);
    Detail::Record(Category::Hle, requester, id, time, false, [&] {
        return fmt::format("{}::{} {}{}", service, command, function ? function : "?",
                           sub.set ? fmt::format(" ioctl {:#010x}", sub.value) : std::string{});
    });
}

} // namespace Kernel::WaitStats
