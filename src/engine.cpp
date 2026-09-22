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
    level->second.remove(node.order.quantity);
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
    level->second.add(quantity);
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
    Levels& opposite = command.side == Side::Buy ? asks_ : bids_;
    const bool crosses = !opposite.empty() && (command.side == Side::Buy
        ? opposite.begin()->first <= command.price : opposite.rbegin()->first >= command.price);
    // A crossing order either finishes on its first maker or consumes that
    // maker and frees a slot. Therefore any surviving remainder has a slot.
    if (free_.empty() && !crosses) { result.status = Status::Capacity; return; }

    Quantity remaining = command.quantity;
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
        level->second.remove(traded);
        if (maker.quantity == 0) erase(opposite, level, slot);
    }
    if (remaining != 0) rest(command, remaining);
    result.status = Status::Accepted;
    result.remaining = remaining;
}

std::optional<Order> Engine::find(OrderId id) const {
    const auto found = orders_.find(id);
    if (found == orders_.end()) return std::nullopt;
    return nodes_[found->second].order;
}

Quote Engine::quote() const {
    Quote out;
    const auto total = [](const Level& level) {
        if (level.quantity_high != 0) throw std::overflow_error("aggregate quote quantity overflow");
        return level.quantity;
    };
    if (!bids_.empty()) { out.bid = bids_.rbegin()->first; out.bid_quantity = total(bids_.rbegin()->second); }
    if (!asks_.empty()) { out.ask = asks_.begin()->first; out.ask_quantity = total(asks_.begin()->second); }
    return out;
}

bool Engine::would_match(const Command& command, const std::function<bool(OrderId)>& predicate) const {
    Quantity remaining = command.quantity;
    const auto inspect = [&](const auto& levels) {
        for (const auto& [price, level] : levels) {
            if (command.side == Side::Buy ? price > command.price : price < command.price) break;
            for (auto slot = level.head; slot != none; slot = nodes_[slot].next) {
                if (predicate(nodes_[slot].order.id)) return true;
                const auto amount = std::min(remaining, nodes_[slot].order.quantity);
                remaining -= amount;
                if (remaining == 0) return false;
            }
        }
        return false;
    };
    if (command.side == Side::Buy) return inspect(asks_);
    // Walk bids in descending price without constructing a temporary book.
    for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
        if (it->first < command.price) break;
        for (auto slot = it->second.head; slot != none; slot = nodes_[slot].next) {
            if (predicate(nodes_[slot].order.id)) return true;
            remaining -= std::min(remaining, nodes_[slot].order.quantity);
            if (remaining == 0) return false;
        }
    }
    return false;
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
            Quantity total = 0, high = 0;
            for (auto slot = level.head; slot != none; slot = nodes_[slot].next) {
                require(slot < nodes_.size() && seen.insert(slot).second);
                const auto& node = nodes_[slot];
                require(node.used && node.previous == previous);
                require(node.order.side == side && node.order.price == price && node.order.quantity > 0);
                const auto found = orders_.find(node.order.id);
                require(found != orders_.end() && found->second == slot);
                const auto amount = node.order.quantity;
                if (amount > std::numeric_limits<Quantity>::max() - total) ++high;
                total += amount;
                previous = slot;
            }
            require(previous == level.tail);
            require(total == level.quantity && high == level.quantity_high);
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
