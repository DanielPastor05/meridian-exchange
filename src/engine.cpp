#include "meridian/engine.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <unordered_set>

namespace meridian {

std::string_view name(Status status) {
    switch (status) {
    case Status::Accepted: return "accepted";
    case Status::Cancelled: return "cancelled";
    case Status::Invalid: return "invalid";
    case Status::DuplicateId: return "duplicate_id";
    case Status::UnknownId: return "unknown_id";
    case Status::Capacity: return "capacity";
    }
    return "invalid";
}

Engine::Engine(std::size_t capacity) : nodes_(capacity) {
    if (capacity == 0) throw std::invalid_argument("capacity must be positive");
    free_.reserve(capacity);
    orders_.reserve(capacity);
    for (std::size_t i = capacity; i != 0; --i) free_.push_back(i - 1);
}

void Engine::erase(Levels& levels, Levels::iterator level, std::size_t slot) {
    Node& node = nodes_[slot];
    if (node.previous == none) level->second.head = node.next;
    else nodes_[node.previous].next = node.next;
    if (node.next == none) level->second.tail = node.previous;
    else nodes_[node.next].previous = node.previous;
    orders_.erase(node.order.id);
    node.used = false;
    free_.push_back(slot);
    if (level->second.head == none) levels.erase(level);
}

void Engine::rest(const Command& command, Quantity quantity) {
    Levels& levels = command.side == Side::Buy ? bids_ : asks_;
    auto [level, inserted] = levels.try_emplace(command.price);
    (void)inserted;
    const auto slot = free_.back();
    free_.pop_back();
    Node& node = nodes_[slot];
    node = {{command.id, command.side, command.price, quantity}, level->second.tail, none, true};
    if (node.previous == none) level->second.head = slot;
    else nodes_[node.previous].next = slot;
    level->second.tail = slot;
    orders_.emplace(command.id, slot);
}

void Engine::apply(const Command& command, Result& result) {
    if (sequence_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("command sequence exhausted");
    result.sequence = ++sequence_;
    result.status = Status::Invalid;
    result.remaining = 0;
    result.trades.clear();
    if (!valid(command)) return;
    const auto existing = orders_.find(command.id);
    if (command.kind == Kind::Cancel) {
        if (existing == orders_.end()) { result.status = Status::UnknownId; return; }
        const auto slot = existing->second;
        const Order order = nodes_[slot].order;
        Levels& levels = order.side == Side::Buy ? bids_ : asks_;
        erase(levels, levels.find(order.price), slot);
        result.status = Status::Cancelled;
        return;
    }
    if (existing != orders_.end()) { result.status = Status::DuplicateId; return; }
    // Admission happens before matching, even if this order could free slots.
    // This keeps capacity rejection atomic and the contract easy to reproduce.
    if (free_.empty()) { result.status = Status::Capacity; return; }

    Quantity remaining = command.quantity;
    Levels& opposite = command.side == Side::Buy ? asks_ : bids_;
    while (remaining != 0 && !opposite.empty()) {
        auto level = command.side == Side::Buy ? opposite.begin() : std::prev(opposite.end());
        if (command.side == Side::Buy ? level->first > command.price : level->first < command.price)
            break;
        const auto slot = level->second.head;
        Order& maker = nodes_[slot].order;
        const Quantity traded = std::min(remaining, maker.quantity);
        // Allocate the result before changing this maker. Allocation failure is
        // fatal to the session; callers must restart from the command journal.
        result.trades.push_back({maker.id, command.id, maker.price, traded});
        remaining -= traded;
        maker.quantity -= traded;
        if (maker.quantity == 0) erase(opposite, level, slot);
    }
    if (remaining != 0) rest(command, remaining);
    result.status = Status::Accepted;
    result.remaining = remaining;
}

std::vector<Order> Engine::snapshot() const {
    std::vector<Order> out;
    out.reserve(orders_.size());
    const auto append = [&](const Level& level) {
        for (auto slot = level.head; slot != none; slot = nodes_[slot].next)
            out.push_back(nodes_[slot].order);
    };
    for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) append(it->second);
    for (const auto& [price, level] : asks_) { (void)price; append(level); }
    return out;
}

std::uint64_t Engine::state_hash() const {
    std::uint64_t hash = 14695981039346656037ULL;
    const auto mix = [&](std::uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            hash ^= value & 0xffU;
            hash *= 1099511628211ULL;
            value >>= 8;
        }
    };
    mix(sequence_);
    for (const auto& order : snapshot()) {
        mix(order.id); mix(static_cast<std::uint64_t>(order.side));
        mix(static_cast<std::uint64_t>(order.price)); mix(order.quantity);
    }
    return hash; // Diagnostic fingerprint, not a cryptographic integrity check.
}

void Engine::verify() const {
    const auto require = [](bool ok) { if (!ok) throw std::logic_error("book invariant failed"); };
    std::unordered_set<std::size_t> seen;
    const auto inspect = [&](const Levels& levels, Side side) {
        for (const auto& [price, level] : levels) {
            require(level.head != none && level.tail != none);
            auto previous = none;
            for (auto slot = level.head; slot != none; slot = nodes_[slot].next) {
                require(slot < nodes_.size() && seen.insert(slot).second);
                const auto& node = nodes_[slot];
                require(node.used && node.previous == previous);
                require(node.order.side == side && node.order.price == price && node.order.quantity > 0);
                const auto found = orders_.find(node.order.id);
                require(found != orders_.end() && found->second == slot);
                previous = slot;
            }
            require(previous == level.tail);
        }
    };
    inspect(bids_, Side::Buy);
    inspect(asks_, Side::Sell);
    require(seen.size() == orders_.size());
    for (const auto slot : free_) {
        require(slot < nodes_.size() && !nodes_[slot].used && seen.insert(slot).second);
    }
    require(seen.size() == nodes_.size());
    require(bids_.empty() || asks_.empty() || bids_.rbegin()->first < asks_.begin()->first);
}

} // namespace meridian
