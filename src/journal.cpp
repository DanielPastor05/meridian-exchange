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
constexpr std::size_t header_size = 32;
constexpr std::size_t record_size = 60;
constexpr std::array<unsigned char, 8> magic{'M', 'R', 'D', 'N', 'J', 'N', 'L', '2'};
struct FileCloser { void operator()(std::FILE* file) const noexcept { std::fclose(file); } };
using File = std::unique_ptr<std::FILE, FileCloser>;

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
void write(JournalIO& io, std::FILE* file, const unsigned char* bytes, std::size_t size) {
    if (io.write(file, bytes, size) != size) throw std::runtime_error("journal write failed");
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

JournalInfo scan(std::FILE* file, const RequestCallback& consume) {
    seek(file, 0);
    std::array<unsigned char, header_size> header{};
    if (std::fread(header.data(), 1, header.size(), file) != header.size())
        throw std::runtime_error("incomplete journal header");
    if (!std::equal(magic.begin(), magic.end(), header.begin()) || get(header.data() + 8, 4) != 2 ||
        get(header.data() + 28, 4) != crc32(header.data(), 28))
        throw std::runtime_error("invalid journal header or checksum");
    const auto capacity = get(header.data() + 12, 8);
    if (capacity == 0 || capacity > 1000000) throw std::runtime_error("unsupported journal capacity");
    JournalInfo info{static_cast<std::size_t>(capacity), 0, 0, get(header.data() + 20, 8)};
    std::array<unsigned char, record_size> record{};
    for (;;) {
        const auto read = std::fread(record.data(), 1, record.size(), file);
        if (std::ferror(file)) throw std::runtime_error("journal read failed");
        if (read != record.size()) { info.incomplete_tail_bytes = read; break; }
        if (get(record.data() + 56, 4) != crc32(record.data(), 56))
            throw std::runtime_error("journal checksum mismatch at record " + std::to_string(info.records + 1));
        if (get(record.data(), 8) != info.records + 1)
            throw std::runtime_error("journal sequence gap");
        if ((record[24] < 1 || record[24] > 4) || (record[25] != 1 && record[25] != 2) ||
            !std::all_of(record.begin() + 26, record.begin() + 32, [](auto b) { return b == 0; }))
            throw std::runtime_error("invalid journal record encoding");
        Command command{static_cast<Kind>(record[24]), get(record.data() + 32, 8),
                        static_cast<Side>(record[25]), std::bit_cast<Price>(get(record.data() + 40, 8)),
                        get(record.data() + 48, 8)};
        ++info.records;
        if (consume) consume(info.records, Request{get(record.data() + 8, 8), get(record.data() + 16, 8), command});
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
    const int descriptor = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
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

std::size_t JournalIO::write(std::FILE* file, const unsigned char* bytes, std::size_t size) {
    return std::fwrite(bytes, 1, size, file);
}
int JournalIO::flush(std::FILE* file) { return std::fflush(file); }
int JournalIO::sync(std::FILE* file) {
#ifdef _WIN32
    return _commit(_fileno(file));
#else
    return fsync(fileno(file));
#endif
}

JournalInfo scan_journal(const std::filesystem::path& path, const ReplayCallback& consume) {
    std::FILE* raw = nullptr;
#ifdef _WIN32
    if (_wfopen_s(&raw, path.c_str(), L"rb") != 0) throw std::runtime_error("cannot read journal");
#else
    raw = std::fopen(path.c_str(), "rb");
    if (!raw) throw std::runtime_error("cannot read journal");
#endif
    File file(raw);
    return scan(file.get(), [&](auto sequence, const Request& request) {
        if (consume) {
            if (request.account != 0) throw std::runtime_error("account journal requires server replay");
            consume(sequence, request.command);
        }
    });
}

Journal::Journal(const std::filesystem::path& path, std::size_t capacity, Durability durability,
                  std::uint64_t configuration, FaultHook fault, std::shared_ptr<JournalIO> io)
    : configuration_(configuration), durability_(durability), fault_(std::move(fault)),
      io_(io ? std::move(io) : std::make_shared<JournalIO>()) {
    if (capacity == 0 || capacity > 1000000) throw std::invalid_argument("capacity must be 1..1000000");
    File file(open_writer(path));
    if (std::filesystem::file_size(path) == 0) {
        std::array<unsigned char, header_size> header{};
        std::copy(magic.begin(), magic.end(), header.begin());
        put(header.data() + 8, 2, 4);
        put(header.data() + 12, capacity, 8);
        put(header.data() + 20, configuration, 8);
        put(header.data() + 28, crc32(header.data(), 28), 4);
        write(*io_, file.get(), header.data(), header.size());
        if (io_->flush(file.get()) != 0) throw std::runtime_error("journal header flush failed");
    }
    const auto info = scan(file.get(), {});
    if (info.capacity != capacity) throw std::runtime_error("journal capacity differs from engine capacity");
    if (info.configuration != configuration) throw std::runtime_error("journal account configuration changed");
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
    if (io_->flush(file_) != 0) throw std::runtime_error("journal flush failed");
    if (durability_ == Durability::Sync && io_->sync(file_) != 0)
        throw std::runtime_error("journal disk synchronization failed");
}

JournalInfo Journal::replay(const ReplayCallback& consume) {
    return replay_requests([&](auto sequence, const Request& request) {
        if (request.account != 0) throw std::runtime_error("account journal requires server replay");
        if (consume) consume(sequence, request.command);
    });
}

JournalInfo Journal::replay_requests(const RequestCallback& consume) {
    if (failed_) throw std::runtime_error("journal session failed; reopen before use");
    try {
        const auto info = scan(file_, consume);
        seek(file_, header_size + sequence_ * record_size);
        return info;
    } catch (...) {
        // A callback can fail between records, leaving an unsafe append position.
        failed_ = true;
        throw;
    }
}

void Journal::append(const Request& request) {
    const auto& command = request.command;
    if ((configuration_ == 0) != (request.account == 0 && request.sequence == 0))
        throw std::invalid_argument("journal mode mismatch");
    if (failed_) throw std::runtime_error("journal session failed; reopen before use");
    if ((command.kind < Kind::New || command.kind > Kind::Resume) ||
        (command.side != Side::Buy && command.side != Side::Sell))
        throw std::invalid_argument("command cannot be encoded");
    if (sequence_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("journal sequence exhausted");
    std::array<unsigned char, record_size> record{};
    put(record.data(), sequence_ + 1, 8);
    record[24] = static_cast<unsigned char>(command.kind);
    record[25] = static_cast<unsigned char>(command.side);
    put(record.data() + 8, request.account, 8);
    put(record.data() + 16, request.sequence, 8);
    put(record.data() + 32, command.id, 8);
    put(record.data() + 40, std::bit_cast<std::uint64_t>(command.price), 8);
    put(record.data() + 48, command.quantity, 8);
    put(record.data() + 56, crc32(record.data(), 56), 4);
    try {
        if (fault_) {
            fault_("before_append");
            write(*io_, file_, record.data(), record.size() / 2);
            if (io_->flush(file_) != 0) throw std::runtime_error("fault-injection flush failed");
            fault_("mid_append");
            write(*io_, file_, record.data() + record.size() / 2, record.size() - record.size() / 2);
            fault_("after_write");
        } else write(*io_, file_, record.data(), record.size());
        flush();
        if (fault_) fault_("after_sync");
    }
    catch (...) { failed_ = true; throw; }
    ++sequence_;
}

} // namespace meridian
