#include "meridian/service.hpp"
#include "meridian/text.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace meridian {
namespace {
constexpr auto bound = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
std::uint64_t integer(const std::string& text) {
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) throw std::runtime_error("invalid account integer");
    return value;
}
void checked_add(std::uint64_t& value, std::uint64_t amount) {
    if (amount > bound - value) throw std::runtime_error("aggregate account configuration exceeds int64 range");
    value += amount;
}
bool notional(Price price, Quantity quantity, std::uint64_t& out) {
    if (price <= 0 || quantity == 0 || quantity > bound / static_cast<std::uint64_t>(price)) return false;
    out = static_cast<std::uint64_t>(price) * quantity;
    return true;
}
Code engine_code(Status status) {
    switch (status) {
    case Status::Accepted: return Code::Accepted;
    case Status::Cancelled: return Code::Cancelled;
    case Status::DuplicateId: return Code::DuplicateOrder;
    case Status::UnknownId: return Code::UnknownOrder;
    case Status::Capacity: return Code::Capacity;
    default: return Code::Invalid;
    }
}
} // namespace

std::string_view name(Code code) {
    constexpr std::string_view names[]{"accepted", "cancelled", "invalid", "duplicate_order", "unknown_order",
        "capacity", "stale", "gap", "conflict", "unknown_account", "not_owner", "halted", "order_limit",
        "funds", "inventory", "exposure", "position", "overflow", "self_trade", "killed", "resumed"};
    const auto index = static_cast<std::size_t>(code);
    return index < std::size(names) ? names[index] : "invalid";
}

std::vector<AccountConfig> read_accounts(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot open account configuration");
    std::vector<AccountConfig> out;
    std::string line;
    bool exceeded = false;
    while (read_bounded_line(file, line, exceeded)) {
        if (exceeded) throw std::runtime_error("account line exceeds 4096 bytes");
        std::istringstream input(line.substr(0, line.find('#')));
        std::vector<std::string> words;
        for (std::string word; input >> word;) words.push_back(word);
        if (words.empty()) continue;
        if (words.size() != 7 || out.size() >= 128) throw std::runtime_error("expected seven account fields, at most 128 accounts");
        out.push_back({integer(words[0]), words[1], integer(words[2]), integer(words[3]),
                       integer(words[4]), integer(words[5]), integer(words[6])});
    }
    if (file.bad()) throw std::runtime_error("account configuration read failed");
    return out;
}

std::uint64_t configuration_hash(const std::vector<AccountConfig>& accounts) {
    auto sorted = accounts;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto& a : sorted) {
        // Tokens may be rotated without changing the accounting replay contract.
        for (auto value : {a.id, a.cash, a.inventory, a.max_order_quantity, a.max_open_notional, a.max_position}) {
            for (int i = 0; i < 8; ++i) { hash ^= value & 255; hash *= 1099511628211ULL; value >>= 8; }
        }
    }
    return hash == 0 ? 1 : hash;
}

ExchangeState::ExchangeState(std::size_t capacity, const std::vector<AccountConfig>& configs,
                             std::size_t event_capacity, FaultHook fault)
    : engine_(capacity), fault_(std::move(fault)), event_capacity_(event_capacity) {
    if (configs.empty() || configs.size() > 128 || event_capacity == 0) throw std::invalid_argument("invalid account or event capacity");
    std::uint64_t positions = 0;
    for (const auto& config : configs) {
        if (config.id == 0 || config.token.size() != 32 ||
            !std::all_of(config.token.begin(), config.token.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
            }) || config.max_order_quantity == 0 || config.max_order_quantity > bound ||
            config.max_open_notional == 0 || config.max_open_notional > bound || config.inventory > config.max_position)
            throw std::invalid_argument("invalid account limits or token; token must be 32 lowercase hex characters");
        checked_add(total_cash_, config.cash);
        checked_add(total_inventory_, config.inventory);
        checked_add(positions, config.max_position);
        Account account;
        account.config = config;
        account.balance.cash = config.cash;
        account.balance.inventory = config.inventory;
        if (!accounts_.emplace(config.id, std::move(account)).second) throw std::invalid_argument("duplicate account");
    }
    orders_.reserve(capacity);
}

bool ExchangeState::authenticate(AccountId id, std::string_view token) const {
    const auto found = accounts_.find(id);
    if (found == accounts_.end() || token.size() != 32) return false;
    unsigned difference = 0;
    for (std::size_t i = 0; i < 32; ++i)
        difference |= static_cast<unsigned char>(token[i]) ^ static_cast<unsigned char>(found->second.config.token[i]);
    return difference == 0;
}

Balance ExchangeState::balance(AccountId id) const { return accounts_.at(id).balance; }
std::uint64_t ExchangeState::last_request(AccountId id) const { return accounts_.at(id).last; }
Outcome ExchangeState::reject(const Request& request, Code code) const {
    Outcome outcome;
    outcome.global_sequence = sequence_;
    outcome.request_sequence = request.sequence;
    outcome.code = code;
    const auto found = accounts_.find(request.account);
    if (found != accounts_.end()) outcome.balance = found->second.balance;
    outcome.quote = engine_.quote();
    outcome.event_sequence = event_sequence_;
    return outcome;
}

std::optional<Outcome> ExchangeState::retry(const Request& request) const {
    const auto found = accounts_.find(request.account);
    if (found == accounts_.end()) return reject(request, Code::UnknownAccount);
    const auto& account = found->second;
    if (request.sequence == 0 || request.sequence < account.last) return reject(request, Code::Stale);
    if (request.sequence == account.last)
        return request == account.request ? account.outcome : reject(request, Code::Conflict);
    if (account.last == std::numeric_limits<std::uint64_t>::max() || request.sequence != account.last + 1)
        return reject(request, Code::Gap);
    return std::nullopt;
}

Code ExchangeState::validate_new(const Request& request) const {
    const auto& c = request.command;
    const auto& a = accounts_.at(request.account);
    const auto& b = a.balance;
    if (!valid(c)) return Code::Invalid;
    if (orders_.contains(c.id)) return Code::DuplicateOrder;
    if (b.halted) return Code::Halted;
    if (c.quantity > a.config.max_order_quantity) return Code::OrderLimit;
    std::uint64_t cost{};
    if (!notional(c.price, c.quantity, cost)) return Code::Overflow;
    if (cost > a.config.max_open_notional - b.open_notional) return Code::Exposure;
    if (c.side == Side::Buy) {
        if (cost > b.cash - b.reserved_cash) return Code::Funds;
        if (c.quantity > a.config.max_position - b.inventory - b.buy_quantity) return Code::Position;
    } else if (c.quantity > b.inventory - b.reserved_inventory) return Code::Inventory;
    if (engine_.would_match(c, [&](auto id) { return orders_.at(id).owner == request.account; })) return Code::SelfTrade;
    return Code::Accepted;
}

void ExchangeState::release(Account& account, const Order& order, Quantity quantity) {
    const auto cost = static_cast<std::uint64_t>(order.price) * quantity;
    account.balance.open_notional -= cost;
    if (order.side == Side::Buy) {
        account.balance.reserved_cash -= cost;
        account.balance.buy_quantity -= quantity;
    } else account.balance.reserved_inventory -= quantity;
}

void ExchangeState::cancel(OrderId id) {
    const auto owned = orders_.at(id);
    auto& account = accounts_.at(owned.owner);
    release(account, owned.order, owned.order.quantity);
    Result result;
    engine_.apply(Command::cancel(id), result);
    if (result.status != Status::Cancelled) throw std::logic_error("account/book cancel mismatch");
    account.orders.erase(id);
    orders_.erase(id);
}

void ExchangeState::emit(Event event) {
    if (event_sequence_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("event sequence exhausted");
    event.sequence = ++event_sequence_;
    event.global_sequence = sequence_;
    events_.push_back(event);
    if (events_.size() > event_capacity_) events_.pop_front();
}

Outcome ExchangeState::apply(const Request& request, std::uint64_t global_sequence) {
    if (retry(request)) throw std::logic_error("only a fresh next request may enter the state machine");
    if (global_sequence != sequence_ + 1) throw std::logic_error("global request sequence gap");
    sequence_ = global_sequence;
    auto& account = accounts_.at(request.account);
    const auto& command = request.command;
    Outcome outcome;
    outcome.global_sequence = sequence_;
    outcome.request_sequence = request.sequence;
    outcome.code = Code::Invalid;
    if (command.kind == Kind::New) {
        outcome.code = validate_new(request);
        if (outcome.code == Code::Accepted) {
            Result result;
            engine_.apply(command, result);
            if (fault_) fault_("after_match");
            outcome.code = engine_code(result.status);
            outcome.remaining = result.remaining;
            outcome.executions = result.trades.size();
            for (const auto& trade : result.trades) {
                auto& maker = orders_.at(trade.maker);
                auto& maker_account = accounts_.at(maker.owner);
                release(maker_account, maker.order, trade.quantity);
                auto& buyer = command.side == Side::Buy ? account : maker_account;
                auto& seller = command.side == Side::Sell ? account : maker_account;
                const auto cost = static_cast<std::uint64_t>(trade.price) * trade.quantity;
                buyer.balance.cash -= cost;
                buyer.balance.inventory += trade.quantity;
                seller.balance.cash += cost;
                seller.balance.inventory -= trade.quantity;
                maker.order.quantity -= trade.quantity;
                if (maker.order.quantity == 0) { maker_account.orders.erase(trade.maker); orders_.erase(trade.maker); }
                outcome.filled += trade.quantity;
                emit({0, 0, 1, trade, {}});
                if (fault_) fault_("during_settlement");
            }
            if (result.status == Status::Accepted && result.remaining > 0) {
                const Order order{command.id, command.side, command.price, result.remaining};
                orders_.emplace(command.id, OwnedOrder{request.account, order});
                account.orders.insert(command.id);
                const auto cost = static_cast<std::uint64_t>(command.price) * result.remaining;
                account.balance.open_notional += cost;
                if (command.side == Side::Buy) { account.balance.reserved_cash += cost; account.balance.buy_quantity += result.remaining; }
                else account.balance.reserved_inventory += result.remaining;
            }
        }
    } else if (command.kind == Kind::Cancel) {
        if (!valid(command)) outcome.code = Code::Invalid;
        else if (!orders_.contains(command.id)) outcome.code = Code::UnknownOrder;
        else if (orders_.at(command.id).owner != request.account) outcome.code = Code::NotOwner;
        else { cancel(command.id); outcome.code = Code::Cancelled; outcome.cancelled = 1; }
    } else if ((command.kind == Kind::Kill || command.kind == Kind::Resume) && command.id == 0 &&
               command.side == Side::Buy && command.price == 0 && command.quantity == 0) {
        account.balance.halted = command.kind == Kind::Kill;
        outcome.code = account.balance.halted ? Code::Killed : Code::Resumed;
        if (account.balance.halted) {
            while (!account.orders.empty()) { cancel(*account.orders.begin()); ++outcome.cancelled; }
        }
    }
    const auto final_quote = engine_.quote();
    emit({0, 0, 2, {}, final_quote});
    outcome.balance = account.balance;
    outcome.quote = final_quote;
    outcome.event_sequence = event_sequence_;
    if (fault_) fault_("before_result_cache");
    account.last = request.sequence;
    account.request = request;
    account.outcome = outcome;
    return outcome;
}

Feed ExchangeState::feed(std::uint64_t after, std::size_t limit) const {
    if (limit == 0 || limit > 256) throw std::invalid_argument("feed limit must be 1..256");
    Feed out{false, event_sequence_, engine_.quote(), {}};
    if (after > event_sequence_ || (!events_.empty() && after < events_.front().sequence - 1)) {
        out.gap = true;
        return out; // quote + latest are an atomic top-of-book recovery snapshot.
    }
    if (!events_.empty() && after < event_sequence_) {
        const auto offset = after < events_.front().sequence ? 0 :
            static_cast<std::size_t>(after - events_.front().sequence + 1);
        const auto count = std::min(limit, events_.size() - offset);
        out.events.reserve(count);
        for (std::size_t i = 0; i < count; ++i) out.events.push_back(events_[offset + i]);
    }
    return out;
}

std::uint64_t ExchangeState::state_hash() const {
    auto hash = engine_.state_hash();
    const auto mix = [&](std::uint64_t value) {
        for (int i = 0; i < 8; ++i) { hash ^= value & 255; hash *= 1099511628211ULL; value >>= 8; }
    };
    const auto balance = [&](const Balance& b) {
        for (auto v : {b.cash, b.inventory, b.reserved_cash, b.reserved_inventory, b.buy_quantity, b.open_notional,
                       static_cast<std::uint64_t>(b.halted)}) mix(v);
    };
    const auto quote = [&](const Quote& q) { mix(q.bid); mix(q.ask); mix(q.bid_quantity); mix(q.ask_quantity); };
    mix(sequence_); mix(event_sequence_); mix(event_capacity_); mix(total_cash_); mix(total_inventory_);
    mix(accounts_.size());
    for (const auto& [id, account] : accounts_) {
        mix(id); mix(account.last); balance(account.balance);
        const auto& config = account.config;
        for (auto v : {config.cash, config.inventory, config.max_order_quantity, config.max_open_notional, config.max_position}) mix(v);
        const auto& request = account.request;
        mix(request.account); mix(request.sequence); mix(static_cast<std::uint64_t>(request.command.kind));
        mix(request.command.id); mix(static_cast<std::uint64_t>(request.command.side)); mix(request.command.price); mix(request.command.quantity);
        const auto& outcome = account.outcome;
        for (auto v : {outcome.global_sequence, outcome.request_sequence, static_cast<std::uint64_t>(outcome.code),
                       outcome.remaining, outcome.filled, outcome.executions, outcome.cancelled, outcome.event_sequence}) mix(v);
        balance(outcome.balance); quote(outcome.quote);
        mix(account.orders.size());
        for (auto order : account.orders) mix(order);
    }
    for (const auto& order : engine_.snapshot()) { mix(order.id); mix(orders_.at(order.id).owner); }
    mix(events_.size());
    for (const auto& event : events_) {
        mix(event.sequence); mix(event.global_sequence); mix(event.type);
        mix(event.trade.maker); mix(event.trade.taker); mix(event.trade.price); mix(event.trade.quantity); quote(event.quote);
    }
    return hash; // Diagnostic only: collisions remain possible; tests also compare explicit state.
}

void ExchangeState::verify() const {
    engine_.verify();
    std::map<AccountId, Balance> expected;
    std::uint64_t cash = 0, inventory = 0;
    const auto book = engine_.snapshot();
    if (book.size() != orders_.size()) throw std::logic_error("ownership index count mismatch");
    for (const auto& order : book) {
        const auto& owned = orders_.at(order.id);
        if (owned.order != order || !accounts_.at(owned.owner).orders.contains(order.id)) throw std::logic_error("ownership mismatch");
        auto& b = expected[owned.owner];
        const auto cost = static_cast<std::uint64_t>(order.price) * order.quantity;
        b.open_notional += cost;
        if (order.side == Side::Buy) { b.reserved_cash += cost; b.buy_quantity += order.quantity; }
        else b.reserved_inventory += order.quantity;
    }
    for (const auto& [id, account] : accounts_) {
        const auto& b = account.balance;
        const auto& e = expected[id];
        if (b.reserved_cash != e.reserved_cash || b.reserved_inventory != e.reserved_inventory ||
            b.buy_quantity != e.buy_quantity || b.open_notional != e.open_notional ||
            b.cash < b.reserved_cash || b.inventory < b.reserved_inventory ||
            b.inventory + b.buy_quantity > account.config.max_position || b.open_notional > account.config.max_open_notional ||
            (b.halted && !account.orders.empty())) throw std::logic_error("account reservation invariant failed");
        for (auto order : account.orders) if (!orders_.contains(order) || orders_.at(order).owner != id)
            throw std::logic_error("account order index mismatch");
        checked_add(cash, b.cash); checked_add(inventory, b.inventory);
    }
    if (cash != total_cash_ || inventory != total_inventory_) throw std::logic_error("cash or inventory not conserved");
}

DurableExchange::DurableExchange(const std::filesystem::path& path, std::size_t capacity,
                                 const std::vector<AccountConfig>& accounts, Durability durability, FaultHook fault,
                                 std::shared_ptr<JournalIO> io)
    : state_(capacity, accounts, 8192, fault),
      journal_(path, capacity, durability, configuration_hash(accounts), fault, std::move(io)), fault_(std::move(fault)) {
    (void)journal_.replay_requests([&](auto sequence, const Request& request) { state_.apply(request, sequence); });
    state_.verify();
}

Outcome DurableExchange::execute(const Request& request) {
    if (failed_) throw std::runtime_error("exchange failed; restart and replay before continuing");
    if (const auto cached = state_.retry(request)) return *cached;
    try {
        journal_.append(request);
        auto outcome = state_.apply(request, journal_.sequence());
        if (fault_) fault_("after_apply");
        return outcome;
    } catch (...) { failed_ = true; throw; }
}
} // namespace meridian
