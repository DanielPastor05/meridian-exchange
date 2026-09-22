#include "meridian/service.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace meridian;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template <class F> void throws(F action, const char* message) {
    bool failed = false; try { action(); } catch (const std::exception&) { failed = true; }
    check(failed, message);
}
std::vector<AccountConfig> configs() {
    return {{1, std::string(32, '1'), 100000, 100, 100, 100000, 1000},
            {2, std::string(32, '2'), 100000, 100, 100, 100000, 1000},
            {3, std::string(32, '3'), 100000, 100, 100, 100000, 1000}};
}
Request order(AccountId account, std::uint64_t sequence, OrderId id, Side side, Price price, Quantity qty) {
    return {account, sequence, {Kind::New, id, side, price, qty}};
}
Outcome run(ExchangeState& state, Request request) {
    const auto cached = state.retry(request);
    if (cached) return *cached;
    const auto result = state.apply(request, state.sequence() + 1);
    state.verify();
    return result;
}

void accounting() {
    ExchangeState state(32, configs());
    auto result = run(state, order(1, 1, 10, Side::Sell, 100, 10));
    check(result.balance.reserved_inventory == 10 && result.balance.open_notional == 1000, "sell reservation");
    result = run(state, order(2, 1, 20, Side::Buy, 105, 4));
    check(result.filled == 4 && result.balance.cash == 99600 && result.balance.inventory == 104, "buyer pays maker price");
    auto seller = state.balance(1);
    check(seller.cash == 100400 && seller.inventory == 96 && seller.reserved_inventory == 6 && seller.open_notional == 600,
          "partial maker releases reservation exactly");
    result = run(state, {2, 2, Command::cancel(10)});
    check(result.code == Code::NotOwner && state.snapshot().size() == 1, "ownership check");
    result = run(state, {1, 2, Command::cancel(10)});
    check(result.code == Code::Cancelled && result.balance.reserved_inventory == 0, "cancel releases balance");
    run(state, order(2, 3, 21, Side::Buy, 90, 20));
    check(state.balance(2).reserved_cash == 1800 && state.balance(2).buy_quantity == 20, "buy reservation");
    result = run(state, order(3, 1, 30, Side::Sell, 80, 25));
    check(result.filled == 20 && result.remaining == 5 && result.balance.cash == 101800 &&
          result.balance.reserved_inventory == 5, "sell taker with remainder");
    check(state.balance(2).reserved_cash == 0 && state.balance(2).inventory == 124, "resting buy settles");
    result = run(state, order(3, 2, 31, Side::Buy, 80, 1));
    check(result.code == Code::SelfTrade && state.snapshot().size() == 1, "self trade rejected before any execution");
    run(state, order(3, 3, 32, Side::Sell, 120, 2));
    result = run(state, {3, 4, {Kind::Kill, 0, Side::Buy, 0, 0}});
    check(result.code == Code::Killed && result.cancelled == 2 && result.balance.halted &&
          result.balance.reserved_inventory == 0, "kill cancels and releases all own orders");
    check(run(state, order(3, 5, 33, Side::Buy, 80, 1)).code == Code::Halted, "halt blocks new orders");
    check(run(state, {3, 6, {Kind::Resume, 0, Side::Buy, 0, 0}}).code == Code::Resumed, "resume");
    check(run(state, order(3, 7, 33, Side::Buy, 80, 1)).code == Code::Accepted, "resumed account trades");
}

void risk_limits() {
    auto config = configs();
    config[0].cash = 100;
    config[0].inventory = 2;
    config[0].max_position = 3;
    config[0].max_open_notional = 1000;
    ExchangeState state(16, config);
    check(run(state, order(1, 1, 1, Side::Buy, 101, 1)).code == Code::Funds, "insufficient cash");
    check(run(state, order(1, 2, 2, Side::Sell, 1, 3)).code == Code::Inventory, "no unbacked short sales");
    check(run(state, order(1, 3, 3, Side::Buy, 1, 2)).code == Code::Position, "position limit includes pending buys");
    check(run(state, order(1, 4, 4, Side::Buy, 1, 101)).code == Code::OrderLimit, "max quantity");
    check(run(state, order(1, 5, 5, Side::Sell, 1001, 1)).code == Code::Exposure, "notional limit");
    check(run(state, order(1, 6, 6, Side::Sell, std::numeric_limits<Price>::max(), 2)).code == Code::Overflow, "notional overflow");
    check(run(state, order(1, 7, 7, Side::Buy, 80, 1)).code == Code::Accepted, "reserve cash");
    check(run(state, order(1, 8, 8, Side::Buy, 21, 1)).code == Code::Funds, "reserved cash unavailable");
    check(run(state, order(1, 9, 9, Side::Buy, 1, 1)).code == Code::Position, "pending position limit");
    run(state, order(1, 10, 10, Side::Sell, 100, 2));
    check(run(state, order(1, 11, 11, Side::Sell, 100, 1)).code == Code::Inventory, "reserved inventory unavailable");
    config[1].id = 1;
    throws([&] { ExchangeState duplicate(16, config); }, "duplicate account accepted");
    config = configs(); config[0].cash = std::numeric_limits<std::uint64_t>::max();
    throws([&] { ExchangeState overflow(16, config); }, "aggregate initial cash overflow accepted");
}

void retries_and_feed() {
    ExchangeState state(16, configs(), 3);
    const auto first = order(1, 1, 10, Side::Buy, 100, 2);
    const auto original = run(state, first);
    run(state, order(2, 1, 20, Side::Sell, 100, 1));
    const auto before = state.state_hash();
    check(run(state, first) == original && before == state.state_hash(), "duplicate response must be identical despite later fills");
    auto changed = first; changed.command.quantity = 7;
    check(run(state, changed).code == Code::Conflict, "same sequence with different payload");
    changed.sequence = 3;
    check(run(state, changed).code == Code::Gap, "out-of-order future request");
    run(state, {1, 2, Command::cancel(10)});
    check(run(state, first).code == Code::Stale && state.sequence() == 3, "old retries must never execute again");
    check(state.feed(0, 256).gap, "expired event cursor must signal a gap");
    const auto snapshot = state.feed(0, 256);
    check(snapshot.quote == state.quote(), "gap response includes recovery quote at latest sequence");
    run(state, order(3, 1, 30, Side::Sell, 110, 3));
    const auto updates = state.feed(snapshot.latest, 256);
    check(!updates.gap && updates.events.size() == 1 && updates.events[0].quote.ask == 110, "snapshot then live continuation");
    check(state.authenticate(1, std::string(32,'1')) && !state.authenticate(1, std::string(32,'2')),
          "account authentication");
}

void randomized_accounting() {
    // An independent ledger integrates cash/inventory from changes in matched
    // orders, while reservations are recomputed from the entire resting book.
    ExchangeState state(128, configs());
    std::map<OrderId, AccountId> owner;
    std::map<AccountId, std::uint64_t> cash{{1,100000},{2,100000},{3,100000}}, inventory{{1,100},{2,100},{3,100}};
    std::mt19937_64 rng(20260922);
    OrderId next_id = 1;
    for (int step = 0; step < 10000; ++step) {
        const AccountId account = 1 + rng() % 3;
        const auto seq = state.last_request(account) + 1;
        Request request;
        if (rng() % 4 == 0 && !state.snapshot().empty()) {
            const auto book = state.snapshot();
            const auto id = book[static_cast<std::size_t>(rng() % book.size())].id;
            request = {account, seq, Command::cancel(id)};
        } else request = order(account, seq, next_id++, rng()%2 == 0 ? Side::Buy : Side::Sell,
                               95 + static_cast<Price>(rng()%11), 1 + rng()%15);
        const auto before = state.snapshot();
        const auto event_start = state.feed(std::numeric_limits<std::uint64_t>::max(), 1).latest;
        const auto result = run(state, request);
        if (result.code == Code::Accepted) owner[request.command.id] = account;
        const auto events = state.feed(event_start, 256);
        check(!events.gap, "test event batch unexpectedly expired");
        Quantity actual_fills = 0;
        for (const auto& event : events.events) if (event.type == 1) {
            const auto& t = event.trade;
            const auto maker = std::find_if(before.begin(), before.end(), [&](auto o) { return o.id == t.maker; });
            check(maker != before.end(), "maker did not exist before execution");
            const auto buyer = maker->side == Side::Buy ? owner.at(t.maker) : account;
            const auto seller = maker->side == Side::Sell ? owner.at(t.maker) : account;
            const auto cost = static_cast<std::uint64_t>(t.price) * t.quantity;
            cash[buyer] -= cost; cash[seller] += cost;
            inventory[buyer] += t.quantity; inventory[seller] -= t.quantity;
            actual_fills += t.quantity;
        }
        check(actual_fills == result.filled, "fill summary differs from market-data events");
        for (AccountId id = 1; id <= 3; ++id) {
            const auto b = state.balance(id);
            check(cash[id] == b.cash && inventory[id] == b.inventory, "independent cash/inventory ledger diverged");
        }
        if (step % 17 == 0) {
            const auto hash = state.state_hash();
            check(run(state, request) == result && hash == state.state_hash(), "random retry changed state");
        }
    }
}

void durable_retries() {
    const auto path = std::filesystem::current_path() /
        ("accounts-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".journal");
    const auto config = configs();
    const auto request = order(1, 1, 42, Side::Buy, 100, 8);
    Outcome original;
    std::uint64_t hash = 0;
    {
        DurableExchange exchange(path, 64, config, Durability::Sync);
        original = exchange.execute(request);
        exchange.execute(order(2, 1, 43, Side::Sell, 100, 2));
        hash = exchange.state().state_hash();
    }
    {
        DurableExchange exchange(path, 64, config, Durability::Sync);
        check(exchange.state().state_hash() == hash, "account replay differs");
        check(exchange.execute(request) == original, "last outcome not recovered exactly");
        check(exchange.state().sequence() == 2, "retry added another journal entry");
        exchange.execute({1, 2, {Kind::Kill, 0, Side::Buy, 0, 0}});
    }
    {
        auto rotated = config; rotated[0].token = std::string(32, 'a');
        DurableExchange exchange(path, 64, rotated, Durability::Sync);
        check(exchange.state().balance(1).halted && exchange.state().balance(1).reserved_cash == 0, "kill did not survive restart");
        check(exchange.state().authenticate(1, std::string(32,'a')), "token rotation should preserve accounting replay");
    }
    auto changed = config; changed[0].cash += 1;
    throws([&] { DurableExchange exchange(path, 64, changed, Durability::Sync); }, "changed accounting configuration accepted");
    std::filesystem::remove(path);
}
} // namespace
int main() {
    try {
        accounting(); std::cout << "PASS settlement, reservations, ownership, self-trade and kill\n";
        risk_limits(); std::cout << "PASS risk and integer boundaries\n";
        retries_and_feed(); std::cout << "PASS retry protocol and market-data recovery\n";
        randomized_accounting(); std::cout << "PASS 10000 accounting commands with independent ledger\n";
        durable_retries(); std::cout << "PASS persistent outcomes, kill and configuration protection\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
