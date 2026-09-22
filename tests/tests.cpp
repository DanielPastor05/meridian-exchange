#include "meridian/engine.hpp"
#include "meridian/journal.hpp"
#include "meridian/text.hpp"
#include <sstream>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>

using namespace meridian;

namespace {
void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
template <class F> void throws(F&& action, const std::string& message) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    check(failed, message);
}
Command add(OrderId id, Side side, Price price, Quantity quantity) {
    return {Kind::New, id, side, price, quantity};
}
Result apply(Engine& engine, const Command& command) {
    Result result;
    engine.apply(command, result);
    engine.verify();
    return result;
}

// Deliberately slow, independent representation: arrival-ordered vector and a
// full scan for each best maker. No price levels, node links or ID index.
class Reference {
public:
    explicit Reference(std::size_t capacity) : capacity_(capacity) {}
    Result apply(const Command& c) {
        Result out;
        out.sequence = ++sequence_;
        if (c.id == 0) return out;
        if (c.kind == Kind::Cancel) {
            if (c.side != Side::Buy || c.price != 0 || c.quantity != 0) return out;
            const auto found = std::find_if(orders_.begin(), orders_.end(), [&](auto o) { return o.id == c.id; });
            if (found == orders_.end()) out.status = Status::UnknownId;
            else { orders_.erase(found); out.status = Status::Cancelled; }
            return out;
        }
        if (c.kind != Kind::New || (c.side != Side::Buy && c.side != Side::Sell) ||
            c.price <= 0 || c.quantity == 0) return out;
        for (const auto& o : orders_) if (o.id == c.id) { out.status = Status::DuplicateId; return out; }
        bool can_cross = false;
        for (const auto& order : orders_) if (order.side != c.side &&
            (c.side == Side::Buy ? order.price <= c.price : order.price >= c.price)) can_cross = true;
        if (orders_.size() == capacity_ && !can_cross) { out.status = Status::Capacity; return out; }
        out.status = Status::Accepted;
        Quantity remaining = c.quantity;
        while (remaining != 0) {
            auto best = orders_.end();
            for (auto it = orders_.begin(); it != orders_.end(); ++it) {
                if (it->side == c.side) continue;
                if (c.side == Side::Buy ? it->price > c.price : it->price < c.price) continue;
                if (best == orders_.end() ||
                    (c.side == Side::Buy ? it->price < best->price : it->price > best->price)) best = it;
            }
            if (best == orders_.end()) break;
            const auto quantity = std::min(remaining, best->quantity);
            out.trades.push_back({best->id, c.id, best->price, quantity});
            best->quantity -= quantity;
            remaining -= quantity;
            if (best->quantity == 0) orders_.erase(best);
        }
        if (remaining > 0) orders_.push_back({c.id, c.side, c.price, remaining});
        out.remaining = remaining;
        return out;
    }
    Quote quote() const {
        Quote out;
        for (const auto& order : orders_) {
            auto& price = order.side == Side::Buy ? out.bid : out.ask;
            auto& total = order.side == Side::Buy ? out.bid_quantity : out.ask_quantity;
            if (price == 0 || (order.side == Side::Buy ? order.price > price : order.price < price)) {
                price = order.price;
                total = order.quantity;
            } else if (price == order.price) total += order.quantity;
        }
        return out;
    }
    std::vector<Order> snapshot() const {
        auto copy = orders_;
        std::stable_sort(copy.begin(), copy.end(), [](const Order& a, const Order& b) {
            if (a.side != b.side) return a.side == Side::Buy;
            return a.side == Side::Buy ? a.price > b.price : a.price < b.price;
        });
        return copy;
    }
private:
    std::vector<Order> orders_;
    std::size_t capacity_;
    std::uint64_t sequence_{};
};

void price_time_priority() {
    Engine engine(16);
    apply(engine, add(1, Side::Sell, 102, 8));
    apply(engine, add(2, Side::Sell, 101, 5));
    apply(engine, add(3, Side::Sell, 101, 7));
    const auto result = apply(engine, add(4, Side::Buy, 103, 14));
    check(result.trades == std::vector<Trade>{{2, 4, 101, 5}, {3, 4, 101, 7}, {1, 4, 102, 2}},
          "buy must choose price, then arrival, and trade at maker price");
    check(engine.snapshot() == std::vector<Order>{{1, Side::Sell, 102, 6}}, "partial fill must retain maker");
    apply(engine, Command::cancel(1));
    apply(engine, add(5, Side::Buy, 99, 4));
    apply(engine, add(6, Side::Buy, 100, 3));
    apply(engine, add(7, Side::Buy, 100, 3));
    const auto sell = apply(engine, add(8, Side::Sell, 99, 8));
    check(sell.trades == std::vector<Trade>{{6, 8, 100, 3}, {7, 8, 100, 3}, {5, 8, 99, 2}},
          "sell must choose highest bid then FIFO");
}

void cancellation_and_slot_reuse() {
    Engine engine(8);
    for (OrderId id = 1; id <= 4; ++id) apply(engine, add(id, Side::Sell, 100, 1));
    apply(engine, Command::cancel(2)); // middle
    apply(engine, Command::cancel(1)); // head
    apply(engine, Command::cancel(4)); // tail
    apply(engine, add(5, Side::Sell, 100, 1));
    const auto result = apply(engine, add(6, Side::Buy, 100, 3));
    check(result.trades == std::vector<Trade>{{3, 6, 100, 1}, {5, 6, 100, 1}},
          "recycled slots must not jump the queue");
    check(result.remaining == 1, "unfilled taker quantity must rest");
    check(apply(engine, Command::cancel(6)).status == Status::Cancelled, "resting taker must be cancellable");
    check(engine.size() == 0, "last cancellation must remove its level");
    check(apply(engine, Command::cancel(6)).status == Status::UnknownId, "cancel cannot succeed twice");
}

void validation_and_capacity() {
    Engine engine(2);
    const std::vector<Command> invalid{
        add(0, Side::Buy, 1, 1), add(1, Side::Buy, 0, 1), add(1, Side::Buy, -1, 1),
        add(1, Side::Buy, 1, 0), add(1, static_cast<Side>(7), 1, 1),
        {static_cast<Kind>(7), 1, Side::Buy, 1, 1}, {Kind::Cancel, 1, Side::Sell, 0, 0}
    };
    for (const auto& c : invalid) check(apply(engine, c).status == Status::Invalid, "invalid order accepted");
    apply(engine, add(1, Side::Buy, 100, 2));
    apply(engine, add(2, Side::Sell, 101, 2));
    const auto before = engine.snapshot();
    check(apply(engine, add(1, Side::Sell, 100, 8)).status == Status::DuplicateId, "duplicate ID traded");
    check(apply(engine, add(3, Side::Buy, 99, 8)).status == Status::Capacity, "full noncrossing book must reject");
    check(engine.snapshot() == before, "rejected order changed book");
    apply(engine, Command::cancel(1));
    check(apply(engine, add(1, Side::Buy, 100, 1)).status == Status::Accepted, "inactive IDs may be reused");
    throws([] { Engine invalid_engine(0); }, "zero capacity accepted");
    Engine full(1);
    apply(full, add(10, Side::Sell, 100, 5));
    const auto partial = apply(full, add(11, Side::Buy, 100, 2));
    check(partial.status == Status::Accepted && full.find(10)->quantity == 3,
          "full book must accept a crossing taker that leaves its maker alive");
    const auto remainder = apply(full, add(12, Side::Buy, 100, 7));
    check(remainder.status == Status::Accepted && full.find(12)->quantity == 4,
          "crossing on a full book must reuse the consumed maker slot");
}

void aggregate_boundaries() {
    Engine engine(4);
    constexpr auto maximum = std::numeric_limits<Quantity>::max();
    apply(engine, add(1, Side::Buy, 100, maximum));
    apply(engine, add(2, Side::Buy, 100, 2));
    throws([&] { (void)engine.quote(); }, "overflowed quote must not wrap");
    apply(engine, add(3, Side::Sell, 100, 3));
    check(engine.quote().bid_quantity == maximum - 1, "partial fill did not reduce wide aggregate");
    apply(engine, Command::cancel(1));
    check(engine.quote().bid_quantity == 2, "cancel did not release level aggregate");
    apply(engine, Command::cancel(2));
    check(engine.quote() == Quote{}, "empty level retained aggregate");
}

void bounded_input() {
    std::istringstream input(std::string(100000, 'x') + "\nBOOK\n" + std::string(4096, 'a'));
    std::string line;
    bool exceeded = false;
    check(read_bounded_line(input, line, exceeded) && exceeded && line.size() == 4096, "oversized input grew the buffer");
    check(read_bounded_line(input, line, exceeded) && !exceeded && line == "BOOK", "oversized line swallowed next command");
    check(read_bounded_line(input, line, exceeded) && !exceeded && line.size() == 4096, "exact bound at EOF rejected");
    check(!read_bounded_line(input, line, exceeded), "EOF generated another line");
}

void integer_boundaries() {
    Engine engine(4);
    constexpr auto max_quantity = std::numeric_limits<Quantity>::max();
    constexpr auto max_price = std::numeric_limits<Price>::max();
    apply(engine, add(1, Side::Sell, max_price, max_quantity));
    const auto result = apply(engine, add(2, Side::Buy, max_price, max_quantity));
    check(result.trades == std::vector<Trade>{{1, 2, max_price, max_quantity}}, "integer boundaries overflowed");
    check(engine.size() == 0, "max-quantity fill left an order");
    apply(engine, add(std::numeric_limits<OrderId>::max(), Side::Buy, 1, 1));
    check(apply(engine, Command::cancel(std::numeric_limits<OrderId>::max())).status == Status::Cancelled,
          "maximum ID cannot be cancelled");
}

Command random_command(std::mt19937_64& rng, OrderId& next_id) {
    const auto choice = rng() % 100;
    if (choice < 25) return Command::cancel(1 + rng() % (next_id + 1));
    const auto id = choice < 35 ? 1 + rng() % (next_id + 1) : next_id++;
    auto command = add(id, rng() % 2 == 0 ? Side::Buy : Side::Sell,
                       95 + static_cast<Price>(rng() % 11), 1 + rng() % 30);
    if (choice == 97) command.quantity = 0;
    if (choice == 98) command.price = -1;
    if (choice == 99) command.id = 0;
    return command;
}

void differential() {
    for (std::uint64_t seed = 0; seed < 20; ++seed) {
        const std::size_t capacity = seed % 2 == 0 ? 16 : 128;
        Engine engine(capacity);
        Reference reference(capacity);
        std::mt19937_64 rng(seed);
        OrderId next_id = 1;
        Result actual;
        for (int step = 0; step < 4000; ++step) {
            const auto command = random_command(rng, next_id);
            engine.apply(command, actual);
            const auto expected = reference.apply(command);
            const std::string context = "seed=" + std::to_string(seed) + " step=" + std::to_string(step);
            check(actual == expected, "result differs: " + context);
            check(engine.snapshot() == reference.snapshot(), "book differs: " + context);
            check(engine.quote() == reference.quote(), "aggregated quote differs: " + context);
            if (actual.status == Status::Accepted) {
                Quantity total = actual.remaining;
                for (const auto& trade : actual.trades) total += trade.quantity;
                check(total == command.quantity, "incoming quantity not conserved: " + context);
            }
            if (step % 32 == 0) engine.verify();
        }
        engine.verify();
    }
}

struct TestDirectory {
    std::filesystem::path path = std::filesystem::current_path() /
        ("meridian-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TestDirectory() { check(std::filesystem::create_directory(path), "cannot create unique test directory"); }
    ~TestDirectory() {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(path, error))
            std::filesystem::remove(entry.path(), error);
        std::filesystem::remove(path, error);
    }
};

std::vector<char> bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void save(const std::filesystem::path& path, const std::vector<char>& data) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    check(static_cast<bool>(output), "test fixture write failed");
}

void journal_roundtrip() {
    TestDirectory directory;
    const auto path = directory.path / "roundtrip.journal";
    Engine live(128), replayed(128);
    std::vector<Result> expected;
    {
        Journal journal(path, 128, Durability::Sync);
        std::mt19937_64 rng(123);
        OrderId next_id = 1;
        for (int i = 0; i < 200; ++i) {
            const auto command = random_command(rng, next_id);
            journal.append(command);
            expected.push_back(apply(live, command));
        }
        check(journal.sequence() == live.sequence(), "journal sequence differs");
        throws([&] { Journal second_writer(path, 128, Durability::Buffered); }, "second writer was allowed");
    }
    const auto info = scan_journal(path, [&](auto sequence, const auto& command) {
        check(apply(replayed, command) == expected.at(static_cast<std::size_t>(sequence - 1)),
              "replay changed an execution or rejection");
    });
    check(info.records == expected.size() && info.incomplete_tail_bytes == 0, "wrong journal record count");
    check(live.snapshot() == replayed.snapshot() && live.state_hash() == replayed.state_hash(), "replay differs");
    const auto original = bytes(path);
    throws([&] { Journal wrong_capacity(path, 64, Durability::Sync); }, "capacity mismatch accepted");
    check(bytes(path) == original, "capacity error modified journal");
    {
        Journal journal(path, 128, Durability::Buffered);
        check(journal.sequence() == 200, "reopen lost sequence");
        journal.append(Command::cancel(123456));
    }
    check(scan_journal(path).records == 201, "append after reopen lost a record");
}

void replay_exception() {
    TestDirectory directory;
    const auto file = directory.path / "replay-exception.journal";
    Journal journal(file, 8, Durability::Sync);
    journal.append(add(1, Side::Buy, 99, 2));
    journal.append(add(2, Side::Sell, 101, 3));
    const auto original = bytes(file);
    throws([&] { (void)journal.replay([](auto, const auto&) { throw std::runtime_error("callback failed"); }); },
           "replay swallowed callback failure");
    throws([&] { journal.append(Command::cancel(1)); }, "failed replay allowed append");
    throws([&] { (void)journal.replay({}); }, "failed replay allowed reuse");
    check(bytes(file) == original, "failed replay modified journal");
}

void torn_tail_and_corruption() {
    TestDirectory directory;
    const auto path = directory.path / "source.journal";
    {
        Journal journal(path, 8, Durability::Buffered);
        journal.append(add(1, Side::Buy, 99, 2));
        journal.append(add(2, Side::Sell, 101, 3));
    }
    const auto complete = bytes(path);
    check(complete.size() == 32 + 2 * 60, "journal format size changed");
    const auto fixture = directory.path / "damaged.journal";
    for (std::size_t tail = 1; tail < 60; ++tail) {
        auto cut = complete;
        cut.resize(32 + 60 + tail);
        save(fixture, cut);
        const auto info = scan_journal(fixture);
        check(info.records == 1 && info.incomplete_tail_bytes == tail, "torn tail not reported");
        check(bytes(fixture) == cut, "read-only replay changed file");
        {
            Journal recovered(fixture, 8, Durability::Sync);
            check(recovered.sequence() == 1, "recovery retained incomplete record");
            recovered.append(add(3, Side::Sell, 105, 7));
        }
        std::vector<OrderId> ids;
        scan_journal(fixture, [&](auto, const auto& command) { ids.push_back(command.id); });
        check(ids == std::vector<OrderId>{1, 3}, "tail repair did not permit safe append");
    }
    for (std::size_t offset = 0; offset < complete.size(); ++offset) {
        auto corrupted = complete;
        corrupted[offset] ^= 0x40;
        save(fixture, corrupted);
        throws([&] { scan_journal(fixture); }, "corrupt byte accepted at " + std::to_string(offset));
        throws([&] { Journal writer(fixture, 8, Durability::Buffered); }, "corrupt journal opened for writing");
        check(bytes(fixture) == corrupted, "complete corruption was silently repaired");
    }
    for (std::size_t size = 1; size < 32; ++size) {
        save(fixture, std::vector<char>(complete.begin(), complete.begin() + static_cast<std::ptrdiff_t>(size)));
        throws([&] { scan_journal(fixture); }, "incomplete header accepted");
    }
}
} // namespace

int main() {
    const std::pair<const char*, void (*)()> tests[]{
        {"price-time priority and maker execution price", price_time_priority},
        {"cancellation, partial fills and slot reuse", cancellation_and_slot_reuse},
        {"validation, duplicate IDs and atomic capacity rejection", validation_and_capacity},
        {"integer boundaries", integer_boundaries},
        {"bounded input consumes oversized lines without retaining them", bounded_input},
        {"cached aggregate overflow and recovery", aggregate_boundaries},
        {"failed replay forbids further writes", replay_exception},
        {"80,000 differential commands, 20 reproducible seeds", differential},
        {"journal replay, durability modes and writer exclusion", journal_roundtrip},
        {"every partial tail and every corrupted byte", torn_tail_and_corruption}
    };
    for (const auto& [label, test] : tests) {
        try { test(); std::cout << "PASS " << label << '\n'; }
        catch (const std::exception& error) {
            std::cerr << "FAIL " << label << ": " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}
