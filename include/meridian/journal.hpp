#pragma once

#include "meridian/engine.hpp"
#include <cstdio>
#include <filesystem>
#include <functional>

namespace meridian {
enum class Durability { Sync, Buffered };
using FaultHook = std::function<void(std::string_view)>;
struct JournalInfo {
    std::size_t capacity{};
    std::uint64_t records{};
    std::size_t incomplete_tail_bytes{};
    std::uint64_t configuration{};
};
using ReplayCallback = std::function<void(std::uint64_t, const Command&)>;
using RequestCallback = std::function<void(std::uint64_t, const Request&)>;
JournalInfo scan_journal(const std::filesystem::path& path, const ReplayCallback& consume = {});

class Journal {
public:
    Journal(const std::filesystem::path& path, std::size_t capacity, Durability durability,
            std::uint64_t configuration = 0, FaultHook fault = {});
    ~Journal();
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;
    void append(const Command& command) { append(Request{0, 0, command}); }
    void append(const Request& request);
    [[nodiscard]] std::uint64_t sequence() const { return sequence_; }
    [[nodiscard]] JournalInfo replay(const ReplayCallback& consume);
    [[nodiscard]] JournalInfo replay_requests(const RequestCallback& consume);
private:
    std::FILE* file_{};
    std::uint64_t sequence_{};
    std::uint64_t configuration_{};
    Durability durability_;
    bool failed_{};
    FaultHook fault_;
    void flush();
};
} // namespace meridian
