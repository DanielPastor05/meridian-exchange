#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <functional>
#include <map>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace meridian {

using OrderId = std::uint64_t;
using Price = std::int64_t; // Integer ticks. The instrument defines the tick size.
using Quantity = std::uint64_t;
enum class Side : std::uint8_t { Buy = 1, Sell = 2 };
enum class Kind : std::uint8_t { New = 1, Cancel = 2, Kill = 3, Resume = 4 };
enum class Status { Accepted, Cancelled, Invalid, DuplicateId, UnknownId, Capacity };
std::string_view name(Status status);

struct Command {
    Kind kind{Kind::New};
    OrderId id{};
    Side side{Side::Buy};
    Price price{};
    Quantity quantity{};
    static Command cancel(OrderId id) { return {Kind::Cancel, id, Side::Buy, 0, 0}; }
    bool operator==(const Command&) const = default;
};

struct Request {
    std::uint64_t account{};
    std::uint64_t sequence{}; // Per-account request sequence, distinct from log sequence.
    Command command{};
    bool operator==(const Request&) const = default;
};

struct Quote {
    Price bid{}, ask{};
    Quantity bid_quantity{}, ask_quantity{};
    bool operator==(const Quote&) const = default;
};

struct Trade {
    OrderId maker{};
    OrderId taker{};
    Price price{};
    Quantity quantity{};
    bool operator==(const Trade&) const = default;
};

struct Order {
    OrderId id{};
    Side side{Side::Buy};
    Price price{};
    Quantity quantity{};
    bool operator==(const Order&) const = default;
};

struct Result {
    std::uint64_t sequence{};
    Status status{Status::Invalid};
    Quantity remaining{};
    std::vector<Trade> trades;
    bool operator==(const Result&) const = default;
};

// A single writer owns the book. Results and snapshots must be published by
// the caller; querying concurrently with apply is not supported.
class Engine {
public:
    explicit Engine(std::size_t capacity = 65536);
    void apply(const Command& command, Result& result);
    [[nodiscard]] std::vector<Order> snapshot() const;
    [[nodiscard]] std::optional<Order> find(OrderId id) const;
    [[nodiscard]] Quote quote() const;
    [[nodiscard]] bool would_match(const Command& command, const std::function<bool(OrderId)>& predicate) const;
    [[nodiscard]] std::uint64_t state_hash() const;
    [[nodiscard]] std::uint64_t sequence() const { return sequence_; }
    [[nodiscard]] std::size_t size() const { return orders_.size(); }
    [[nodiscard]] std::size_t capacity() const { return nodes_.size(); }
    // Expensive structural checks for tests, never called in the hot path.
    void verify() const;

private:
    static constexpr std::size_t none = std::numeric_limits<std::size_t>::max();
    struct Node {
        Order order{};
        std::size_t previous{none};
        std::size_t next{none};
        bool used{};
    };
    struct Level { std::size_t head{none}; std::size_t tail{none}; };
    using Levels = std::map<Price, Level>;
    Levels bids_, asks_;
    std::vector<Node> nodes_;
    std::vector<std::size_t> free_;
    std::unordered_map<OrderId, std::size_t> orders_;
    std::uint64_t sequence_{};
    void erase(Levels& levels, Levels::iterator level, std::size_t slot);
    void rest(const Command& command, Quantity quantity);
};

inline bool valid(const Command& command) {
    if (command.id == 0) return false;
    if (command.kind == Kind::Cancel)
        return command.side == Side::Buy && command.price == 0 && command.quantity == 0;
    return command.kind == Kind::New &&
           (command.side == Side::Buy || command.side == Side::Sell) &&
           command.price > 0 && command.quantity > 0;
}

} // namespace meridian
