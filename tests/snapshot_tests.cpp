#include "meridian/snapshot.hpp"
#include <iostream>
#include <stdexcept>

using namespace meridian;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int main() {
    try {
        const std::vector<AccountConfig> accounts{{1, std::string(32, '1'), 1000000000, 1000, 1000, 1000000000, 1000000}};
        ExchangeState state(1024, accounts);
        for (std::uint64_t i = 1; i <= 1000; ++i) state.apply({1, i, {Kind::New, i, Side::Buy, 100, 1}}, i);
        BookSnapshots cache(2000 * sizeof(Order), std::chrono::seconds(5));
        const auto now = BookSnapshots::Clock::now();
        auto first = cache.page(state, 0, 0, 1, now);
        check(!first.changed && first.total == 1000 && first.orders.front().id == 1, "first snapshot");
        const auto version = first.version;
        state.apply({1, 1001, Command::cancel(1)}, 1001);
        for (std::uint64_t offset = 1; offset < 1000; ++offset) {
            const auto page = cache.page(state, offset, version, 1, now);
            check(!page.changed && page.total == 1000 && page.orders.size() == 1 && page.orders[0].id == offset + 1,
                  "mutation changed retained snapshot");
        }
        check(cache.copied_orders() == 1000, "pagination copied the full book again");
        const auto updated = cache.page(state, 0, 0, 256, now);
        check(updated.version == 1001 && updated.total == 999 && updated.orders[0].id == 2, "new snapshot is not current");
        check(cache.copied_orders() == 1999 && cache.retained_bytes() <= 2000 * sizeof(Order), "snapshot memory bound");
        check(cache.page(state, 0, version, 1, now + std::chrono::seconds(5)).changed, "expired snapshot survived");
        BookSnapshots small(1000 * sizeof(Order));
        const auto old = small.page(state, 0, 0, 1, now);
        state.apply({1, 1002, Command::cancel(2)}, 1002);
        (void)small.page(state, 0, 0, 1, now);
        check(small.page(state, 1, old.version, 1, now).changed, "budget eviction did not expire cursor");
        check(small.retained_bytes() <= 1000 * sizeof(Order), "eviction exceeded memory bound");
        const auto beyond = small.page(state, UINT64_MAX, state.sequence(), 256, now);
        check(beyond.orders.empty() && beyond.offset == beyond.total, "offset overflow");
        std::cout << "PASS immutable pagination, copy count, TTL, eviction and bounds\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
