// EDEN_NX2_LOCK_STATS (implementation, header-only so no build files change).
#pragma once
#include <algorithm>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#endif

#include "common/logging.h"


namespace Common {
namespace LockStatsDetail {

struct WaitTotals {
    std::chrono::steady_clock::duration time{};
    std::uint64_t count{};
};

// Only touched by threads that registered or waited, with EDEN_NX2_LOCK_STATS set. A plain
// std::mutex: logging must never take a SpinRecursiveMutex.
struct LockStats {
    std::mutex mutex;
    std::unordered_map<std::thread::id, std::string> names;
    std::map<std::pair<std::string, std::string>, WaitTotals> waits; ///< (waiter, owner)
    std::chrono::steady_clock::time_point last_log = std::chrono::steady_clock::now();
};

inline LockStats& Stats() {
    // Leaked on purpose: threads may still lock during static destruction.
    static LockStats* const stats = new LockStats;
    return *stats;
}

inline std::string ThreadIdString(std::thread::id id) {
    std::ostringstream stream;
    stream << id;
    return "tid " + stream.str();
}

inline const std::string& CurrentThreadName() {
    thread_local const std::string name = [] {
#if defined(__linux__)
        char buffer[32]{};
        if (pthread_getname_np(pthread_self(), buffer, sizeof(buffer)) == 0 && buffer[0] != '\0') {
            return std::string{buffer};
        }
#endif
        return ThreadIdString(std::this_thread::get_id());
    }();
    return name;
}

} // namespace LockStatsDetail

inline void LockStatsRegisterThread() {
    using namespace LockStatsDetail;
    const std::string& name = CurrentThreadName();
    LockStats& stats = Stats();
    std::scoped_lock lock{stats.mutex};
    stats.names[std::this_thread::get_id()] = name;
}

inline void LockStatsRecord(std::thread::id owner, std::chrono::steady_clock::duration wait) {
    using namespace LockStatsDetail;
    const std::string& waiter = CurrentThreadName();
    LockStats& stats = Stats();
    std::vector<std::pair<std::pair<std::string, std::string>, WaitTotals>> report;
    {
        std::scoped_lock lock{stats.mutex};
        std::string owner_name = "<unknown>";
        if (owner != std::thread::id{}) {
            const auto it = stats.names.find(owner);
            owner_name = it != stats.names.end() ? it->second : ThreadIdString(owner);
        }
        WaitTotals& totals = stats.waits[{waiter, std::move(owner_name)}];
        totals.time += wait;
        ++totals.count;
        const auto now = std::chrono::steady_clock::now();
        if (now - stats.last_log < std::chrono::seconds{10}) {
            return;
        }
        stats.last_log = now;
        report.assign(stats.waits.begin(), stats.waits.end());
        stats.waits.clear();
    }
    std::ranges::sort(report, [](const auto& a, const auto& b) { return a.second.time > b.second.time; });
    for (const auto& [threads, totals] : report) {
        const double ms = std::chrono::duration<double, std::milli>(totals.time).count();
        LOG_INFO(Common, "Lock waits: {} waited {:.2f} ms on {} ({} times)", threads.first, ms,
                 threads.second, totals.count);
    }
}

} // namespace Common
