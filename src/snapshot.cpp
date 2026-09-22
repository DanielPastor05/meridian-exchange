#include "meridian/snapshot.hpp"
#include <algorithm>
#include <stdexcept>

namespace meridian {
BookSnapshots::BookSnapshots(std::size_t byte_budget, Clock::duration ttl)
    : budget_(byte_budget), ttl_(ttl) {
    if (budget_ < sizeof(Order) || ttl <= Clock::duration::zero())
        throw std::invalid_argument("invalid snapshot budget or expiry");
}
void BookSnapshots::erase(std::map<std::uint64_t, Snapshot>::iterator entry) {
    bytes_ -= entry->second.orders.capacity() * sizeof(Order);
    snapshots_.erase(entry);
}
BookPage BookSnapshots::page(const ExchangeState& state, std::uint64_t offset,
                             std::uint64_t version, std::size_t limit, Clock::time_point now) {
    if (limit == 0 || limit > 256) throw std::invalid_argument("invalid book limit");
    for (auto it = snapshots_.begin(); it != snapshots_.end();) {
        if (it->second.expires <= now) { const auto expired = it++; erase(expired); }
        else ++it;
    }
    if (offset == 0 && version == 0) {
        version = state.sequence();
        if (!snapshots_.contains(version)) {
            const auto required = state.order_count() * sizeof(Order);
            if (required > budget_) throw std::runtime_error("book exceeds snapshot memory budget");
            while (!snapshots_.empty() && (bytes_ > budget_ - required || snapshots_.size() >= 32))
                erase(snapshots_.begin());
            auto orders = state.snapshot();
            const auto allocation = orders.capacity() * sizeof(Order);
            if (allocation > budget_) throw std::runtime_error("snapshot allocation exceeds memory budget");
            while (!snapshots_.empty() && bytes_ > budget_ - allocation) erase(snapshots_.begin());
            const auto count = orders.size();
            snapshots_.emplace(version, Snapshot{now + ttl_, std::move(orders)});
            bytes_ += allocation;
            copied_orders_ += count;
        }
    }
    const auto entry = snapshots_.find(version);
    if (entry == snapshots_.end()) return {true, state.sequence(), 0, offset, {}};
    const auto& orders = entry->second.orders;
    const auto start = static_cast<std::size_t>(std::min(offset, static_cast<std::uint64_t>(orders.size())));
    const auto count = std::min(limit, orders.size() - start);
    return {false, version, orders.size(), start,
            std::vector<Order>(orders.begin() + static_cast<std::ptrdiff_t>(start),
                               orders.begin() + static_cast<std::ptrdiff_t>(start + count))};
}
}
