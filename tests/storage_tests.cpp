#include "meridian/service.hpp"
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace meridian;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void fails(F action) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    check(failed, "expected failure was acknowledged");
}
struct FaultIO : JournalIO {
    enum Mode { ShortWrite, NoSpace, Flush, Sync };
    Mode mode;
    bool armed{};
    explicit FaultIO(Mode value) : mode(value) {}
    std::size_t write(std::FILE* file, const unsigned char* data, std::size_t size) override {
        if (armed && mode == NoSpace) { errno = ENOSPC; return 0; }
        if (armed && mode == ShortWrite) { errno = EIO; return JournalIO::write(file, data, size / 2); }
        return JournalIO::write(file, data, size);
    }
    int flush(std::FILE* file) override {
        if (armed && mode == Flush) { errno = EIO; return EOF; }
        return JournalIO::flush(file);
    }
    int sync(std::FILE* file) override {
        if (armed && mode == Sync) { errno = EIO; return -1; }
        return JournalIO::sync(file);
    }
};
struct TempDirectory {
    std::filesystem::path path = std::filesystem::current_path() /
        ("storage-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDirectory() { std::filesystem::create_directory(path); }
    ~TempDirectory() {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(path, error)) std::filesystem::remove(entry.path(), error);
        std::filesystem::remove(path, error);
    }
};
int main() {
    try {
        TempDirectory directory;
        const std::vector<AccountConfig> accounts{{1, std::string(32, '1'), 10000, 100, 100, 10000, 1000},
                                                  {2, std::string(32, '2'), 10000, 100, 100, 10000, 1000}};
        const Request maker{1, 1, {Kind::New, 10, Side::Sell, 100, 10}};
        const Request taker{2, 1, {Kind::New, 20, Side::Buy, 105, 3}};
        for (auto mode : {FaultIO::ShortWrite, FaultIO::NoSpace, FaultIO::Flush, FaultIO::Sync}) {
            const auto file = directory.path / (std::to_string(mode) + ".journal");
            auto io = std::make_shared<FaultIO>(mode);
            {
                DurableExchange exchange(file, 32, accounts, Durability::Sync, {}, io);
                check(exchange.execute(maker).code == Code::Accepted, "prefix failed");
                io->armed = true;
                fails([&] { (void)exchange.execute(taker); });
                fails([&] { (void)exchange.execute(taker); });
            }
            {
                DurableExchange recovered(file, 32, accounts, Durability::Sync);
                const auto result = recovered.execute(taker);
                check(result.code == Code::Accepted && result.filled == 3, "uncertain request could not recover");
                check(recovered.execute(taker) == result && recovered.state().sequence() == 2, "retry executed twice");
                check(recovered.state().balance(1).cash == 10300 && recovered.state().balance(2).cash == 9700, "settlement was lost or doubled");
                check(recovered.state().balance(1).reserved_inventory == 7, "reservation diverged");
                recovered.state().verify();
            }
        }
        for (const auto stage : {"after_match", "during_settlement", "before_result_cache"}) {
            const auto file = directory.path / (std::string(stage) + ".journal");
            bool armed = false;
            {
                DurableExchange exchange(file, 32, accounts, Durability::Sync, [&](std::string_view point) {
                    if (armed && point == stage) throw std::bad_alloc();
                });
                (void)exchange.execute(maker);
                armed = true;
                fails([&] { (void)exchange.execute(taker); });
                fails([&] { (void)exchange.execute(taker); });
            }
            DurableExchange recovered(file, 32, accounts, Durability::Sync);
            const auto result = recovered.execute(taker);
            check(result.filled == 3 && recovered.state().sequence() == 2, "allocation-failure replay diverged");
            check(recovered.state().balance(1).cash == 10300 && recovered.state().balance(2).cash == 9700, "partial accounting survived replay");
            recovered.state().verify();
        }
        std::cout << "PASS short write, ENOSPC, flush/sync errors and allocation failures in partial transitions\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
