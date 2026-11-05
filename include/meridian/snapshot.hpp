#pragma once
#include "meridian/service.hpp"
#include <chrono>
#include <map>

namespace meridian {
struct BookPage {
    bool changed{};
    std::uint64_t version{}, total{}, offset{};
    std::vector<Order> orders;
};

// Called by the state owner. A retained version is immutable across mutations.
// The global byte/version budget bounds all subscribers together, not per socket.
class BookSnapshots {
public:
    using Clock = std::chrono::steady_clock;
    explicit BookSnapshots(std::size_t byte_budget = 64 * 1024 * 1024,
                           Clock::duration ttl = std::chrono::seconds(5));
    BookPage page(const ExchangeState& state, std::uint64_t offset, std::uint64_t version,
                  std::size_t limit, Clock::time_point now = Clock::now());
    [[nodiscard]] std::size_t retained_bytes() const { return bytes_; }
    [[nodiscard]] std::uint64_t copied_orders() const { return copied_orders_; }
private:
    struct Snapshot { Clock::time_point expires; std::vector<Order> orders; };
    std::map<std::uint64_t, Snapshot> snapshots_;
    std::size_t budget_, bytes_{};
    Clock::duration ttl_;
    std::uint64_t copied_orders_{};
    void erase(std::map<std::uint64_t, Snapshot>::iterator entry);
};
}
