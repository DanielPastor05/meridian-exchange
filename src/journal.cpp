#include "meridian/journal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <memory>
#include <stdexcept>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace meridian {
namespace {
constexpr std::size_t header_size = 24;
constexpr std::size_t record_size = 44;
constexpr std::array<unsigned char, 8> magic{'M', 'R', 'D', 'N', 'J', 'N', 'L', '1'};
using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;

void put(unsigned char* out, std::uint64_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) { out[i] = static_cast<unsigned char>(value); value >>= 8; }
}
std::uint64_t get(const unsigned char* in, std::size_t size) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) value |= std::uint64_t{in[i]} << (8 * i);
    return value;
}
std::uint32_t crc32(const unsigned char* bytes, std::size_t size) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}
void seek(std::FILE* file, std::uint64_t position) {
    if (position > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::runtime_error("journal is too large");
#ifdef _WIN32
    const int status = _fseeki64(file, static_cast<__int64>(position), SEEK_SET);
#else
    const int status = fseeko(file, static_cast<off_t>(position), SEEK_SET);
#endif
    if (status != 0) throw std::runtime_error("cannot seek journal");
}
void truncate(std::FILE* file, std::uint64_t size) {
#ifdef _WIN32
    const int status = _chsize_s(_fileno(file), size);
#else
    const int status = ftruncate(fileno(file), static_cast<off_t>(size));
#endif
    if (status != 0) throw std::runtime_error("cannot truncate incomplete journal tail");
}
void write(std::FILE* file, const unsigned char* bytes, std::size_t size) {
    if (std::fwrite(bytes, 1, size, file) != size) throw std::runtime_error("journal write failed");
}

void sync_parent(const std::filesystem::path& path) {
#ifndef _WIN32
    // fsync(file) alone does not make a newly created directory entry durable.
    const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    const int directory = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) throw std::runtime_error("cannot open journal parent directory");
    const int status = fsync(directory);
    close(directory);
    if (status != 0) throw std::runtime_error("journal directory synchronization failed");
#else
    (void)path; // Windows uses _commit for the file; filesystem metadata semantics apply.
#endif
}

JournalInfo scan(std::FILE* file, const ReplayCallback& consume) {
    seek(file, 0);
    std::array<unsigned char, header_size> header{};
    if (std::fread(header.data(), 1, header.size(), file) != header.size())
        throw std::runtime_error("incomplete journal header");
    if (!std::equal(magic.begin(), magic.end(), header.begin()) || get(header.data() + 8, 4) != 1 ||
        get(header.data() + 20, 4) != crc32(header.data(), 20))
        throw std::runtime_error("invalid journal header or checksum");
    const auto capacity = get(header.data() + 12, 8);
    if (capacity == 0 || capacity > 1000000) throw std::runtime_error("unsupported journal capacity");
    JournalInfo info{static_cast<std::size_t>(capacity), 0, 0};
    std::array<unsigned char, record_size> record{};
    for (;;) {
        const auto read = std::fread(record.data(), 1, record.size(), file);
        if (std::ferror(file)) throw std::runtime_error("journal read failed");
        if (read != record.size()) { info.incomplete_tail_bytes = read; break; }
        if (get(record.data() + 40, 4) != crc32(record.data(), 40))
            throw std::runtime_error("journal checksum mismatch at record " + std::to_string(info.records + 1));
        if (get(record.data(), 8) != info.records + 1)
            throw std::runtime_error("journal sequence gap");
        if ((record[8] != 1 && record[8] != 2) || (record[9] != 1 && record[9] != 2) ||
            !std::all_of(record.begin() + 10, record.begin() + 16, [](auto b) { return b == 0; }))
            throw std::runtime_error("invalid journal record encoding");
        Command command{static_cast<Kind>(record[8]), get(record.data() + 16, 8),
                        static_cast<Side>(record[9]), std::bit_cast<Price>(get(record.data() + 24, 8)),
                        get(record.data() + 32, 8)};
        ++info.records;
        if (consume) consume(info.records, command);
    }
    return info;
}

std::FILE* open_writer(const std::filesystem::path& path) {
#ifdef _WIN32
    int descriptor = -1;
    if (_wsopen_s(&descriptor, path.c_str(), _O_RDWR | _O_CREAT | _O_BINARY,
                  _SH_DENYWR, _S_IREAD | _S_IWRITE) != 0)
        throw std::runtime_error("cannot open journal (check path and other writers)");
    auto* file = _fdopen(descriptor, "r+b");
    if (!file) { _close(descriptor); throw std::runtime_error("cannot open journal stream"); }
#else
    const int descriptor = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (descriptor < 0) throw std::runtime_error("cannot open journal");
    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        close(descriptor); throw std::runtime_error("journal already has a writer");
    }
    auto* file = fdopen(descriptor, "r+b");
    if (!file) { close(descriptor); throw std::runtime_error("cannot open journal stream"); }
#endif
    return file;
}

} // namespace

JournalInfo scan_journal(const std::filesystem::path& path, const ReplayCallback& consume) {
    std::FILE* raw = nullptr;
#ifdef _WIN32
    if (_wfopen_s(&raw, path.c_str(), L"rb") != 0) throw std::runtime_error("cannot read journal");
#else
    raw = std::fopen(path.c_str(), "rb");
    if (!raw) throw std::runtime_error("cannot read journal");
#endif
    File file(raw, std::fclose);
    return scan(file.get(), consume);
}

Journal::Journal(const std::filesystem::path& path, std::size_t capacity, Durability durability)
    : durability_(durability) {
    if (capacity == 0 || capacity > 1000000) throw std::invalid_argument("capacity must be 1..1000000");
    File file(open_writer(path), std::fclose);
    if (std::filesystem::file_size(path) == 0) {
        std::array<unsigned char, header_size> header{};
        std::copy(magic.begin(), magic.end(), header.begin());
        put(header.data() + 8, 1, 4);
        put(header.data() + 12, capacity, 8);
        put(header.data() + 20, crc32(header.data(), 20), 4);
        write(file.get(), header.data(), header.size());
        if (std::fflush(file.get()) != 0) throw std::runtime_error("journal header flush failed");
    }
    const auto info = scan(file.get(), {});
    if (info.capacity != capacity) throw std::runtime_error("journal capacity differs from engine capacity");
    sequence_ = info.records;
    const auto end = header_size + sequence_ * record_size;
    if (info.incomplete_tail_bytes != 0) truncate(file.get(), end);
    seek(file.get(), end);
    file_ = file.get();
    try {
        flush();
        if (durability_ == Durability::Sync) sync_parent(path);
    } catch (...) { file_ = nullptr; throw; }
    (void)file.release();
}

Journal::~Journal() { if (file_) std::fclose(file_); }

void Journal::flush() {
    if (std::fflush(file_) != 0) throw std::runtime_error("journal flush failed");
    if (durability_ == Durability::Sync) {
#ifdef _WIN32
        const int status = _commit(_fileno(file_));
#else
        const int status = fsync(fileno(file_));
#endif
        if (status != 0) throw std::runtime_error("journal disk synchronization failed");
    }
}

JournalInfo Journal::replay(const ReplayCallback& consume) {
    if (failed_) throw std::runtime_error("journal session failed; reopen before use");
    const auto info = scan(file_, consume);
    seek(file_, header_size + sequence_ * record_size);
    return info;
}

void Journal::append(const Command& command) {
    if (failed_) throw std::runtime_error("journal session failed; reopen before use");
    if ((command.kind != Kind::New && command.kind != Kind::Cancel) ||
        (command.side != Side::Buy && command.side != Side::Sell))
        throw std::invalid_argument("command cannot be encoded");
    if (sequence_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("journal sequence exhausted");
    std::array<unsigned char, record_size> record{};
    put(record.data(), sequence_ + 1, 8);
    record[8] = static_cast<unsigned char>(command.kind);
    record[9] = static_cast<unsigned char>(command.side);
    put(record.data() + 16, command.id, 8);
    put(record.data() + 24, std::bit_cast<std::uint64_t>(command.price), 8);
    put(record.data() + 32, command.quantity, 8);
    put(record.data() + 40, crc32(record.data(), 40), 4);
    try { write(file_, record.data(), record.size()); flush(); }
    catch (...) { failed_ = true; throw; }
    ++sequence_;
}

} // namespace meridian
