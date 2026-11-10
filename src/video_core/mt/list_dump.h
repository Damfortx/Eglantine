// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_set>

#ifndef _WIN32
#include <unistd.h>
#endif

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/fs/path_util.h"
#include "common/logging.h"

/// EDEN_NX2_LIST_DUMP=<N> (diagnostic, off): records command lists fetched from GPU-written pages
/// (DmaPusher::ReadGpuWrittenCommands and its flush fallback) with their context and decoded
/// method stream, to tell what the GPU-generated lists contain (analysis:
/// eglantine-buildenv/tools/listdump.py). After EDEN_NX2_LIST_DUMP_DELAY seconds of process
/// runtime (default 40) the first N fetches are recorded, then one in EDEN_NX2_LIST_DUMP_EVERY
/// (default 64). Output: <log dir>/gpu_lists/listdump-<pid>-<time>.txt, text, one line per
/// record (format in the file's header). Read once; nothing is done without the variable.
namespace VideoCommon::MT {

/// Process start (dynamic initialisation), the runtime the dump's delay counts from.
inline const std::chrono::steady_clock::time_point g_list_dump_process_start =
    std::chrono::steady_clock::now();

class ListDump {
public:
    /// Writer state ReadGpuWrittenCommands saw for a sampled fetch.
    struct Fetch {
        bool sampled{};
        bool ok{};
        bool new_page{};
        bool waited{};
        bool skipped{};
        bool shortened{};
        u8 page_count{};
        u8 writer_kind{6};
        std::array<u8, 4> results{};
        std::array<u8, 4> state_before{};
        u64 page_bound{};
        u64 writers_target{};
        u64 consumed_before{};
        u64 settled_before{};
        u64 wait_ns{};
    };

    /// GPFIFO entry context, filled by DmaPusher::Step.
    struct Entry {
        u64 address{};
        u64 header_raw{};
        u32 path{}; ///< 1 read by ReadGpuWrittenCommands, 2 flush fallback
        s32 channel{-1};
        u32 subindex{};
        u32 entries{};
        u64 prev_address{};
        u32 prev_size{};
        u64 next_address{};
        u32 next_size{};
        u32 resume_offset{};
        u64 frame{};
        std::array<u8, 8> engines{}; ///< Tegra::Engines::EngineTypes per subchannel, 0xff unbound
        // Method state the entry starts in (a method whose data continues from the last entry).
        u32 carry_method{};
        u32 carry_subchannel{};
        u32 carry_count{};
        char carry_mode{}; ///< 0 none, 'I' incrementing, 'N' non-incrementing, 'O' increase once
    };

    [[nodiscard]] static bool Enabled() {
        return Limit() != 0;
    }

    /// Called once per GPU-written list fetch: whether this one is recorded.
    bool Sample() {
        fetch = {};
        if (!Enabled()) {
            return false;
        }
        static const u64 delay_s = ReadU64("EDEN_NX2_LIST_DUMP_DELAY", 40);
        if (!started) {
            if (std::chrono::steady_clock::now() - g_list_dump_process_start <
                std::chrono::seconds{delay_s}) {
                return false;
            }
            started = true;
        }
        static const u64 every = std::max<u64>(1, ReadU64("EDEN_NX2_LIST_DUMP_EVERY", 64));
        const u64 index = fetches++;
        fetch.sampled = index < Limit() || (index - Limit()) % every == 0;
        return fetch.sampled;
    }

    Fetch fetch;

    /// Writes one list record (if this fetch was sampled). `macro` (u32 index) -> {u32 start,
    /// std::span<const u32> code} looks up 3D macros called in the list (code empty: unknown).
    template <typename MacroLookup>
    void Record(const Entry& entry, std::span<const u32> words, MacroLookup&& macro) {
        if (!fetch.sampled) {
            return;
        }
        fetch.sampled = false;
        std::FILE* const out = File();
        if (out == nullptr || bytes_written > (u64{1} << 30)) {
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - g_list_dump_process_start)
                            .count();
        std::string line = fmt::format(
            "L {} t={} f={} ch={} a={:x} n={} path={} ok={} sub={}/{} hdr={:x} prev={:x}:{} "
            "next={:x}:{} resume={} np={} wait={} skip={} short={} wk={} wbound={} wtgt={} "
            "cons={} settled={} waitus={} res=",
            records, ms, entry.frame, entry.channel, entry.address, words.size(), entry.path,
            fetch.ok ? 1 : 0, entry.subindex, entry.entries, entry.header_raw, entry.prev_address,
            entry.prev_size, entry.next_address, entry.next_size, entry.resume_offset,
            fetch.new_page ? 1 : 0, fetch.waited ? 1 : 0, fetch.skipped ? 1 : 0,
            fetch.shortened ? 1 : 0, fetch.writer_kind, fetch.page_bound, fetch.writers_target,
            fetch.consumed_before, fetch.settled_before, fetch.wait_ns / 1000);
        for (u8 i = 0; i < fetch.page_count; ++i) {
            line += fmt::format("{}{}", i ? "," : "", fetch.results[i]);
        }
        line += " ps=";
        for (u8 i = 0; i < fetch.page_count; ++i) {
            line += fmt::format("{}{}", i ? "," : "", fetch.state_before[i]);
        }
        line += " sc=";
        for (const u8 engine : entry.engines) {
            line += engine == 0xff ? '-' : static_cast<char>('0' + engine);
        }
        if (entry.carry_mode != 0) {
            line += fmt::format(" carry={}{}:{:x}+{}", entry.carry_subchannel, entry.carry_mode,
                                entry.carry_method, entry.carry_count);
        }
        line += " |";
        Decode(line, entry, words, macro);
        line += '\n';
        std::fwrite(line.data(), 1, line.size(), out);
        std::fflush(out);
        bytes_written += line.size();
        ++records;
    }

private:
    static u64 ReadU64(const char* name, u64 fallback) {
        const char* value = std::getenv(name);
        return value != nullptr && value[0] != '\0' ? std::strtoull(value, nullptr, 0) : fallback;
    }
    static long Pid() {
#ifdef _WIN32
        return 0;
#else
        return static_cast<long>(::getpid());
#endif
    }
    static u64 Limit() {
        static const u64 limit = ReadU64("EDEN_NX2_LIST_DUMP", 0);
        return limit;
    }

    std::FILE* File() {
        if (opened) {
            return file;
        }
        opened = true;
        std::error_code ec;
        const auto dir = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "gpu_lists";
        std::filesystem::create_directories(dir, ec);
        const auto path =
            dir / fmt::format("listdump-{}-{}.txt", Pid(),
                              static_cast<long long>(std::time(nullptr)));
        file = std::fopen(path.string().c_str(), "w");
        if (file == nullptr) {
            LOG_ERROR(HW_GPU, "EDEN_NX2_LIST_DUMP: can't open {}", path.string());
            return nullptr;
        }
        LOG_INFO(HW_GPU, "EDEN_NX2_LIST_DUMP: writing GPU-written command lists to {}",
                 path.string());
        const std::string header =
            "# eglantine listdump v1\n"
            "# L <seq> t=<ms since start> f=<presents> ch=<channel> a=<gpu va> n=<words> "
            "path=<1 copy/guest read, 2 flush fallback> ok=<read ok> sub=<gpfifo index>/<entries> "
            "hdr=<gpfifo entry raw> prev/next=<neighbour entry va>:<words> resume=<word offset> "
            "np=<first list on page> wait=<waited> skip=<L25> short=<L24> "
            "wk=<last writer kind: 0 timer 1 compute 2 3d 3 2d 4 dma 5 i2m 6 none> "
            "wbound=<pages' last writer position> wtgt=<writers target> cons/settled=<split "
            "positions before> waitus=<wait time> res=<per page 0 synced 1 copied 2 unknown> "
            "ps=<per page state before 0 none 1 synced 2 copy current 3 copy stale 4 "
            "draw-written> sc=<engine per subchannel 0-7, '-' unbound> [carry=<sc><mode>:<method>+"
            "<left>] | <commands>\n"
            "# command: <sc><mode>:<method hex>=<values hex,...> mode i incrementing, n "
            "non-incrementing, o increase once, l inline (value in header), I/N/O carried from "
            "the previous entry; '+<k>' after the values: k words continue in the next entry; "
            "?<word>: unknown submission mode (skipped)\n"
            "# M <macro index> start=<code start> len=<words> hash=<fnv64> code=<words hex,...>: "
            "3D macro first called in a recorded list (again when its code changes)\n";
        std::fwrite(header.data(), 1, header.size(), file);
        return file;
    }

    template <typename MacroLookup>
    void Decode(std::string& line, const Entry& entry, std::span<const u32> words,
                MacroLookup&& macro) {
        u32 method = 0;
        u32 subchannel = 0;
        u32 count = 0;
        bool non_incrementing = false;
        bool once = false;
        size_t index = 0;
        const auto take = [&](char mode) {
            const u32 start_method = method;
            const u32 sc = subchannel;
            line += fmt::format(" {}{}:{:x}=", sc, mode, start_method);
            bool first = true;
            while (count != 0 && index < words.size()) {
                if (!first) {
                    line += ',';
                }
                line += fmt::format("{:x}", words[index]);
                first = false;
                NoteMacro(entry, sc, method, macro);
                method += non_incrementing ? 0 : 1;
                non_incrementing |= once;
                --count;
                ++index;
            }
            if (count != 0) {
                line += fmt::format("+{}", count);
            }
        };
        if (entry.carry_mode != 0 && entry.resume_offset == 0) {
            method = entry.carry_method;
            subchannel = entry.carry_subchannel;
            count = entry.carry_count;
            non_incrementing = entry.carry_mode == 'N';
            once = entry.carry_mode == 'O';
            take(entry.carry_mode);
        }
        while (index < words.size()) {
            const u32 word = words[index++];
            const u32 mode = word >> 29;
            method = word & 0x1fff;
            subchannel = (word >> 13) & 7;
            count = (word >> 16) & 0x1fff;
            switch (mode) {
            case 1: // Increasing
                non_incrementing = once = false;
                take('i');
                break;
            case 3: // NonIncreasing
                non_incrementing = true;
                once = false;
                take('n');
                break;
            case 4: // Inline: the value is the count field
                line += fmt::format(" {}l:{:x}={:x}", subchannel, method, count);
                NoteMacro(entry, subchannel, method, macro);
                break;
            case 5: // IncreaseOnce
                non_incrementing = false;
                once = true;
                take('o');
                break;
            default:
                line += fmt::format(" ?{:x}", word);
                break;
            }
        }
    }

    /// A 3D macro call (method >= 0xE00, even: start): writes its code once per index and hash.
    template <typename MacroLookup>
    void NoteMacro(const Entry& entry, u32 subchannel, u32 method, MacroLookup& macro) {
        if (method < 0xE00 || (method & 1) != 0 || entry.engines[subchannel & 7] != 2) {
            return;
        }
        const u32 index = (method - 0xE00) >> 1;
        const auto [start, code] = macro(index);
        u64 hash = 0xcbf29ce484222325ULL;
        for (const u32 word : code) {
            hash = (hash ^ word) * 0x100000001b3ULL;
        }
        const u64 key = (hash << 8) ^ index ^ (u64{start} << 40);
        if (!macros_seen.insert(key).second) {
            return;
        }
        std::string text = fmt::format("M {} start={:x} len={} hash={:016x} code=", index, start,
                                       code.size(), hash);
        for (size_t i = 0; i < code.size(); ++i) {
            if (i != 0) {
                text += ',';
            }
            text += fmt::format("{:x}", code[i]);
        }
        text += '\n';
        if (std::FILE* const out = File()) {
            std::fwrite(text.data(), 1, text.size(), out);
            bytes_written += text.size();
        }
    }

    std::FILE* file{};
    bool opened{};
    bool started{};
    u64 fetches{};
    u64 records{};
    u64 bytes_written{};
    std::unordered_set<u64> macros_seen;
};

} // namespace VideoCommon::MT
