// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Batch 7 T2: persistent disk cache of ASTC textures transcoded to BC on the GPU
// (EDEN_NX2_ASTC_DISKCACHE). One file per entry, <shader dir>/<title id>/astc_bc/<key>.bc:
//   64-byte header (magic, file version, encoder version, key, payload size, payload checksum,
//   header checksum) + the BC payload exactly as EncodeTranscoded lays it out (all levels,
//   level after level, each level all layers).
// Thread model: lookups/reads run on the caller's thread and only consult the in-memory index
// (no directory scans there); the directory scan, all file writes, LRU touches, deletes and
// evictions run on one writer thread fed by a bounded queue (jobs are dropped when it is full).
// The index is shared under a mutex.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/common_types.h"
#include "common/logging.h"

namespace VideoCommon {

struct AstcDiskCacheKey {
    u64 lo{};
    u64 hi{};
    bool operator==(const AstcDiskCacheKey&) const = default;
};

struct AstcDiskCacheKeyHash {
    size_t operator()(const AstcDiskCacheKey& key) const noexcept {
        return static_cast<size_t>(key.lo ^ (key.hi * 0x9E3779B97F4A7C15ULL));
    }
};

/// Everything besides the guest bytes that changes the transcoded result.
struct AstcDiskCacheParams {
    u32 format;
    u32 type;
    u32 width;
    u32 height;
    u32 depth;
    u32 levels;
    u32 layers;
    u32 block_width;
    u32 block_height;
    u32 block_depth;
    u32 tile_width_spacing;
    u32 layer_stride;
    u32 num_samples;
    u32 astc_opaque;
    u32 bc_mode; ///< 0 = BC1, 1 = BC3, 2 = BC7
    u32 encoder_version;
    u64 guest_size;
};

/// Copies `size` bytes from possibly write-combined (uncached) memory into cached memory, with
/// streaming loads where available (plain loads from WC memory are uncached and very slow).
#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("sse4.1")))
#endif
inline void AstcDiskCacheStreamCopySse41(u8* dst, const u8* src, size_t size) {
    size_t i = 0;
    const size_t misalign = reinterpret_cast<uintptr_t>(src) & 15;
    if (misalign != 0) {
        const size_t head = (std::min)(size, 16 - misalign);
        std::memcpy(dst, src, head);
        i = head;
    }
    for (; i + 64 <= size; i += 64) {
        __m128i* s = reinterpret_cast<__m128i*>(const_cast<u8*>(src + i));
        const __m128i a = _mm_stream_load_si128(s);
        const __m128i b = _mm_stream_load_si128(s + 1);
        const __m128i c = _mm_stream_load_si128(s + 2);
        const __m128i d = _mm_stream_load_si128(s + 3);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), a);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 16), b);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 32), c);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i + 48), d);
    }
    if (i < size) {
        std::memcpy(dst + i, src + i, size - i);
    }
}
#endif

inline void AstcDiskCacheCopyFromUncached(u8* dst, const u8* src, size_t size) {
#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__) || defined(__clang__)
    static const bool sse41 = __builtin_cpu_supports("sse4.1");
#else
    static constexpr bool sse41 = true;
#endif
    if (sse41) {
        AstcDiskCacheStreamCopySse41(dst, src, size);
        return;
    }
#endif
    std::memcpy(dst, src, size);
}

class AstcDiskCache {
public:
    /// Bump when the BC encoder's output changes (bc_encoder.comp, incl. the BC7 path): old
    /// entries then simply stop matching and age out through the LRU.
    static constexpr u32 kEncoderVersion = 2;
    static constexpr u32 kFileVersion = 1;
    static constexpr u32 kMagic = 0x43424341; // "ACBC"
    static constexpr size_t kHeaderSize = 64;
    static constexpr const char* kExtension = ".bc";

    struct Header {
        u32 magic;
        u32 file_version;
        u32 encoder_version;
        u32 reserved;
        u64 key_lo;
        u64 key_hi;
        u64 payload_size;
        u64 payload_checksum;
        u64 header_checksum; ///< CityHash64 of the bytes before this field
        u64 pad;
    };
    static_assert(sizeof(Header) == kHeaderSize);

    AstcDiskCache(std::filesystem::path dir_, u64 cap_bytes_, u64 queue_cap_bytes_)
        : dir{std::move(dir_)}, cap_bytes{cap_bytes_}, queue_cap_bytes{queue_cap_bytes_} {
        writer = std::thread([this] { WriterLoop(); });
    }

    ~AstcDiskCache() {
        {
            std::scoped_lock lock{queue_mutex};
            stopping = true;
        }
        queue_cv.notify_all();
        if (writer.joinable()) {
            writer.join();
        }
    }

    AstcDiskCache(const AstcDiskCache&) = delete;
    AstcDiskCache& operator=(const AstcDiskCache&) = delete;

    [[nodiscard]] const std::filesystem::path& Directory() const noexcept {
        return dir;
    }

    [[nodiscard]] static AstcDiskCacheKey MakeKey(std::span<const u8> guest,
                                                  const AstcDiskCacheParams& params) {
        const u128 seed = Common::CityHash128(reinterpret_cast<const char*>(&params),
                                              sizeof(params));
        const u128 hash = Common::CityHash128WithSeed(reinterpret_cast<const char*>(guest.data()),
                                                      guest.size(), seed);
        return AstcDiskCacheKey{hash[0], hash[1]};
    }

    /// Payload size of `key` when the index has it (and marks it most recently used).
    [[nodiscard]] std::optional<u64> Lookup(const AstcDiskCacheKey& key) {
        bool touch = false;
        u64 size = 0;
        {
            std::scoped_lock lock{index_mutex};
            const auto it = index.find(key);
            if (it == index.end()) {
                return std::nullopt;
            }
            lru.splice(lru.end(), lru, it->second.lru_it);
            size = it->second.size;
            const auto now = std::chrono::steady_clock::now();
            if (now - it->second.last_touch > std::chrono::minutes{10}) {
                it->second.last_touch = now;
                touch = true;
            }
        }
        if (touch) {
            Enqueue(Job{JobKind::Touch, key, {}});
        }
        return size;
    }

    /// Reads and validates the payload of `key` into `out` (resized to `size`). A bad entry is
    /// dropped from the index and its file deleted (on the writer thread).
    [[nodiscard]] bool Read(const AstcDiskCacheKey& key, u64 size, std::vector<u8>& out) {
        Header header{};
        bool ok = false;
        {
            std::ifstream file(EntryPath(key), std::ios::binary);
            if (file && file.read(reinterpret_cast<char*>(&header), sizeof(header)) &&
                ValidHeader(header, key, size)) {
                out.resize(size);
                ok = static_cast<bool>(
                         file.read(reinterpret_cast<char*>(out.data()),
                                   static_cast<std::streamsize>(size))) &&
                     Common::CityHash64(reinterpret_cast<const char*>(out.data()), size) ==
                         header.payload_checksum;
            }
        }
        if (!ok) {
            corrupt.fetch_add(1, std::memory_order_relaxed);
            EraseFromIndex(key);
            Enqueue(Job{JobKind::Remove, key, {}});
        }
        return ok;
    }

    /// Reserves `key` for a write; false if it is already cached or being written.
    [[nodiscard]] bool BeginInsert(const AstcDiskCacheKey& key) {
        std::scoped_lock lock{index_mutex};
        if (index.contains(key)) {
            return false;
        }
        return in_flight.insert(key).second;
    }

    void CancelInsert(const AstcDiskCacheKey& key) {
        std::scoped_lock lock{index_mutex};
        in_flight.erase(key);
    }

    /// Hands a payload reserved with BeginInsert to the writer thread (dropped when the queue
    /// is full).
    void Submit(const AstcDiskCacheKey& key, std::vector<u8>&& payload) {
        if (!Enqueue(Job{JobKind::Write, key, std::move(payload)})) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            CancelInsert(key);
        }
    }

    void NoteHit(u64 bytes, u64 read_ns) {
        hits.fetch_add(1, std::memory_order_relaxed);
        hit_bytes.fetch_add(bytes, std::memory_order_relaxed);
        hit_read_ns.fetch_add(read_ns, std::memory_order_relaxed);
    }
    void NoteMiss() {
        misses.fetch_add(1, std::memory_order_relaxed);
    }
    void NoteSkippedReadback() {
        skipped.fetch_add(1, std::memory_order_relaxed);
    }
    void NoteHash(u64 bytes, u64 ns) {
        hashed_bytes.fetch_add(bytes, std::memory_order_relaxed);
        hash_ns.fetch_add(ns, std::memory_order_relaxed);
    }

private:
    enum class JobKind { Write, Touch, Remove };
    struct Job {
        JobKind kind;
        AstcDiskCacheKey key;
        std::vector<u8> payload;
    };
    struct Entry {
        u64 size;
        std::list<AstcDiskCacheKey>::iterator lru_it;
        std::chrono::steady_clock::time_point last_touch;
    };

    [[nodiscard]] std::filesystem::path EntryPath(const AstcDiskCacheKey& key) const {
        return dir / fmt::format("{:016x}{:016x}{}", key.hi, key.lo, kExtension);
    }

    [[nodiscard]] static u64 HeaderChecksum(const Header& header) {
        return Common::CityHash64(reinterpret_cast<const char*>(&header),
                                  offsetof(Header, header_checksum));
    }

    [[nodiscard]] static bool ValidHeader(const Header& header, const AstcDiskCacheKey& key,
                                          u64 size) {
        return header.magic == kMagic && header.file_version == kFileVersion &&
               header.encoder_version == kEncoderVersion && header.key_lo == key.lo &&
               header.key_hi == key.hi && header.payload_size == size &&
               header.header_checksum == HeaderChecksum(header);
    }

    [[nodiscard]] static std::optional<AstcDiskCacheKey> ParseName(
        const std::filesystem::path& path) {
        if (path.extension() != kExtension) {
            return std::nullopt;
        }
        const std::string stem = path.stem().string();
        if (stem.size() != 32 || !std::ranges::all_of(stem, [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            })) {
            return std::nullopt;
        }
        AstcDiskCacheKey key;
        key.hi = std::stoull(stem.substr(0, 16), nullptr, 16);
        key.lo = std::stoull(stem.substr(16, 16), nullptr, 16);
        return key;
    }

    bool Enqueue(Job&& job) {
        const u64 bytes = job.payload.size();
        {
            std::scoped_lock lock{queue_mutex};
            if (stopping || queue.size() >= 4096 || queued_bytes + bytes > queue_cap_bytes) {
                return false;
            }
            queued_bytes += bytes;
            queue.push_back(std::move(job));
        }
        queue_cv.notify_one();
        return true;
    }

    void EraseFromIndex(const AstcDiskCacheKey& key) {
        std::scoped_lock lock{index_mutex};
        const auto it = index.find(key);
        if (it != index.end()) {
            index_bytes -= it->second.size;
            lru.erase(it->second.lru_it);
            index.erase(it);
        }
    }

    void RemoveFile(const std::filesystem::path& path) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    /// Writer thread: drops oldest entries until the index fits the cap.
    void EvictOverCap() {
        std::vector<AstcDiskCacheKey> victims;
        {
            std::scoped_lock lock{index_mutex};
            while (index_bytes > cap_bytes && lru.size() > 1) {
                const AstcDiskCacheKey victim = lru.front();
                lru.pop_front();
                const auto it = index.find(victim);
                index_bytes -= it->second.size;
                index.erase(it);
                victims.push_back(victim);
            }
        }
        for (const AstcDiskCacheKey& victim : victims) {
            RemoveFile(EntryPath(victim));
        }
        evicted.fetch_add(victims.size(), std::memory_order_relaxed);
    }

    void Scan() {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        struct Found {
            AstcDiskCacheKey key;
            u64 size;
            std::filesystem::file_time_type time;
        };
        std::vector<Found> found;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end;
             it.increment(ec)) {
            std::error_code entry_ec;
            if (!it->is_regular_file(entry_ec)) {
                continue;
            }
            const std::filesystem::path& path = it->path();
            const auto key = ParseName(path);
            const u64 file_size = it->file_size(entry_ec);
            if (!key || entry_ec || file_size < kHeaderSize) {
                // Unfinished writes (.tmp) and anything unexpected in our own directory.
                RemoveFile(path);
                continue;
            }
            found.push_back({*key, file_size - kHeaderSize, it->last_write_time(entry_ec)});
        }
        std::ranges::sort(found, {}, &Found::time);
        {
            std::scoped_lock lock{index_mutex};
            for (const Found& entry : found) {
                if (index.contains(entry.key)) {
                    continue;
                }
                // Never touched in this run: the first hit refreshes the file's mtime.
                lru.push_back(entry.key);
                index.emplace(entry.key, Entry{entry.size, std::prev(lru.end()), {}});
                index_bytes += entry.size;
            }
        }
        EvictOverCap();
        u64 entries;
        u64 bytes;
        {
            std::scoped_lock lock{index_mutex};
            entries = index.size();
            bytes = index_bytes;
        }
        LOG_INFO(Render_Vulkan, "ASTC disk cache: {} (cap {} MiB), {} entries / {} MiB",
                 dir.string(), cap_bytes >> 20, entries, bytes >> 20);
    }

    void Write(const AstcDiskCacheKey& key, const std::vector<u8>& payload) {
        Header header{};
        header.magic = kMagic;
        header.file_version = kFileVersion;
        header.encoder_version = kEncoderVersion;
        header.key_lo = key.lo;
        header.key_hi = key.hi;
        header.payload_size = payload.size();
        header.payload_checksum =
            Common::CityHash64(reinterpret_cast<const char*>(payload.data()), payload.size());
        header.header_checksum = HeaderChecksum(header);

        const std::filesystem::path path = EntryPath(key);
        std::filesystem::path tmp = path;
        tmp += ".tmp";
        bool ok = false;
        {
            std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
            ok = file && file.write(reinterpret_cast<const char*>(&header), sizeof(header)) &&
                 file.write(reinterpret_cast<const char*>(payload.data()),
                            static_cast<std::streamsize>(payload.size()));
            if (ok) {
                file.flush();
                ok = static_cast<bool>(file);
            }
        }
        std::error_code ec;
        if (ok) {
            std::filesystem::rename(tmp, path, ec);
            ok = !ec;
        }
        if (!ok) {
            RemoveFile(tmp);
            std::scoped_lock lock{index_mutex};
            in_flight.erase(key);
            write_errors.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        {
            std::scoped_lock lock{index_mutex};
            in_flight.erase(key);
            if (!index.contains(key)) {
                lru.push_back(key);
                index.emplace(key, Entry{payload.size(), std::prev(lru.end()),
                                         std::chrono::steady_clock::now()});
                index_bytes += payload.size();
            }
        }
        written.fetch_add(1, std::memory_order_relaxed);
        written_bytes.fetch_add(payload.size(), std::memory_order_relaxed);
        EvictOverCap();
    }

    void Run(Job& job) {
        switch (job.kind) {
        case JobKind::Write:
            Write(job.key, job.payload);
            break;
        case JobKind::Touch: {
            std::error_code ec;
            std::filesystem::last_write_time(EntryPath(job.key),
                                             std::filesystem::file_time_type::clock::now(), ec);
            break;
        }
        case JobKind::Remove:
            RemoveFile(EntryPath(job.key));
            break;
        }
    }

    void LogStats() {
        const u64 n_hits = hits.exchange(0, std::memory_order_relaxed);
        const u64 n_hit_bytes = hit_bytes.exchange(0, std::memory_order_relaxed);
        const u64 n_read_ns = hit_read_ns.exchange(0, std::memory_order_relaxed);
        const u64 n_misses = misses.exchange(0, std::memory_order_relaxed);
        const u64 n_written = written.exchange(0, std::memory_order_relaxed);
        const u64 n_written_bytes = written_bytes.exchange(0, std::memory_order_relaxed);
        const u64 n_skipped = skipped.exchange(0, std::memory_order_relaxed);
        const u64 n_evicted = evicted.exchange(0, std::memory_order_relaxed);
        const u64 n_dropped = dropped.exchange(0, std::memory_order_relaxed);
        const u64 n_corrupt = corrupt.exchange(0, std::memory_order_relaxed);
        const u64 n_errors = write_errors.exchange(0, std::memory_order_relaxed);
        const u64 n_hashed = hashed_bytes.exchange(0, std::memory_order_relaxed);
        const u64 n_hash_ns = hash_ns.exchange(0, std::memory_order_relaxed);
        if (n_hits + n_misses + n_written + n_skipped + n_evicted + n_dropped + n_corrupt +
                n_errors ==
            0) {
            return;
        }
        u64 entries;
        u64 bytes;
        {
            std::scoped_lock lock{index_mutex};
            entries = index.size();
            bytes = index_bytes;
        }
        LOG_INFO(Render_Vulkan,
                 "ASTC disk cache in 10 s: {} hits ({} MiB, {} ms read), {} misses, {} written "
                 "({} MiB), {} skipped readbacks, {} evicted, index {} entries / {} MiB; "
                 "hash {} ms over {} MiB, {} dropped writes, {} corrupt, {} write errors",
                 n_hits, n_hit_bytes >> 20, n_read_ns / 1'000'000, n_misses, n_written,
                 n_written_bytes >> 20, n_skipped, n_evicted, entries, bytes >> 20,
                 n_hash_ns / 1'000'000, n_hashed >> 20, n_dropped, n_corrupt, n_errors);
    }

    void WriterLoop() {
        Scan();
        auto next_log = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        while (true) {
            Job job;
            {
                std::unique_lock lock{queue_mutex};
                queue_cv.wait_until(lock, next_log, [this] { return stopping || !queue.empty(); });
                if (queue.empty()) {
                    if (stopping) {
                        break;
                    }
                    lock.unlock();
                    if (std::chrono::steady_clock::now() >= next_log) {
                        LogStats();
                        next_log = std::chrono::steady_clock::now() + std::chrono::seconds{10};
                    }
                    continue;
                }
                job = std::move(queue.front());
                queue.pop_front();
                queued_bytes -= job.payload.size();
            }
            // On shutdown the queue is drained (bounded by queue_cap_bytes) so finished
            // transcodes still reach the disk.
            Run(job);
            if (std::chrono::steady_clock::now() >= next_log) {
                LogStats();
                next_log = std::chrono::steady_clock::now() + std::chrono::seconds{10};
            }
        }
        LogStats();
    }

    const std::filesystem::path dir;
    const u64 cap_bytes;
    const u64 queue_cap_bytes;

    std::mutex index_mutex;
    std::list<AstcDiskCacheKey> lru; ///< front = least recently used
    std::unordered_map<AstcDiskCacheKey, Entry, AstcDiskCacheKeyHash> index;
    std::unordered_set<AstcDiskCacheKey, AstcDiskCacheKeyHash> in_flight;
    u64 index_bytes = 0;

    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<Job> queue;
    u64 queued_bytes = 0;
    bool stopping = false;

    std::atomic<u64> hits{};
    std::atomic<u64> hit_bytes{};
    std::atomic<u64> hit_read_ns{};
    std::atomic<u64> misses{};
    std::atomic<u64> written{};
    std::atomic<u64> written_bytes{};
    std::atomic<u64> skipped{};
    std::atomic<u64> evicted{};
    std::atomic<u64> dropped{};
    std::atomic<u64> corrupt{};
    std::atomic<u64> write_errors{};
    std::atomic<u64> hashed_bytes{};
    std::atomic<u64> hash_ns{};

    std::thread writer; ///< last: starts after every other member is constructed
};

} // namespace VideoCommon
