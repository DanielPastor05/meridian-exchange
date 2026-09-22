#pragma once

#include "meridian/engine.hpp"

#include <cstdio>
#include <filesystem>
#include <functional>

namespace meridian {

enum class Durability { Sync, Buffered };
struct JournalInfo {
    std::size_t capacity{};
    std::uint64_t records{};
    std::size_t incomplete_tail_bytes{};
};
using ReplayCallback = std::function<void(std::uint64_t, const Command&)>;
// Read-only. A partial final record is reported; a complete corrupt record fails.
JournalInfo scan_journal(const std::filesystem::path& path, const ReplayCallback& consume = {});

class Journal {
public:
    // Holds an exclusive writer lock until destruction. On opening, validates
    // the entire journal and removes only an incomplete final record.
    Journal(const std::filesystem::path& path, std::size_t capacity, Durability durability);
    ~Journal();
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;
    void append(const Command& command);
    [[nodiscard]] std::uint64_t sequence() const { return sequence_; }
    [[nodiscard]] JournalInfo replay(const ReplayCallback& consume);

private:
    std::FILE* file_{};
    std::uint64_t sequence_{};
    Durability durability_;
    bool failed_{};
    void flush();
};

} // namespace meridian
