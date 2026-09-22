#include "meridian/snapshot.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace meridian;
using Clock = std::chrono::steady_clock;
double ns(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::nano>(end - begin).count();
}
int main(int argc, char** argv) {
    try {
        const auto repetitions = argc > 1 ? std::stoi(argv[1]) : 3;
        if (repetitions < 1 || repetitions > 20 || argc > 2) throw std::invalid_argument("usage: exchange_service_profile [1..20 repetitions]");
        const std::vector<AccountConfig> accounts{{1, std::string(32, '1'), 1000000000000ULL, 0, 1000, 1000000000000ULL, 1000000}};
        std::cout << "# Full ExchangeState: risk, reservations, matching, quote and events; no journal/network.\n"
                     "# One occupied price level. Command construction is timed; object construction and final verification are excluded.\n"
                     "depth,repetition,prefill_ns_per_request,steady_ns_per_request,snapshot_ns_per_order,copied_orders,hash\n";
        for (const std::size_t depth : {1000, 10000, 100000}) {
            for (int repetition = 1; repetition <= repetitions; ++repetition) {
                ExchangeState state(depth + 1, accounts);
                std::vector<OrderId> active(depth);
                std::uint64_t sequence = 0;
                const auto prefill_start = Clock::now();
                for (std::size_t i = 0; i < depth; ++i) {
                    active[i] = i + 1;
                    ++sequence;
                    const auto result = state.apply({1, sequence, {Kind::New, active[i], Side::Buy, 100, 1}}, sequence);
                    if (result.code != Code::Accepted) throw std::runtime_error("prefill rejected");
                }
                const auto prefill_end = Clock::now();
                constexpr std::size_t cycles = 10000;
                const auto steady_start = Clock::now();
                for (std::size_t i = 0; i < cycles; ++i) {
                    auto& id = active[i % depth];
                    ++sequence;
                    if (state.apply({1, sequence, Command::cancel(id)}, sequence).code != Code::Cancelled)
                        throw std::runtime_error("cancel rejected");
                    id = depth + i + 1;
                    ++sequence;
                    if (state.apply({1, sequence, {Kind::New, id, Side::Buy, 100, 1}}, sequence).code != Code::Accepted)
                        throw std::runtime_error("replacement rejected");
                }
                const auto steady_end = Clock::now();
                BookSnapshots snapshots;
                std::uint64_t offset = 0, version = 0;
                const auto snapshot_start = Clock::now();
                do {
                    const auto page = snapshots.page(state, offset, version, 256);
                    if (page.changed || page.orders.empty()) throw std::runtime_error("snapshot unexpectedly expired");
                    version = page.version;
                    offset += page.orders.size();
                } while (offset < depth);
                const auto snapshot_end = Clock::now();
                state.verify();
                if (state.quote().bid_quantity != depth || snapshots.copied_orders() != depth)
                    throw std::runtime_error("depth/aggregate/copy count mismatch");
                std::cout << depth << ',' << repetition << ',' << ns(prefill_start, prefill_end) / depth << ','
                          << ns(steady_start, steady_end) / (cycles * 2) << ',' << ns(snapshot_start, snapshot_end) / depth << ','
                          << snapshots.copied_orders() << ',' << state.state_hash() << '\n';
            }
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
