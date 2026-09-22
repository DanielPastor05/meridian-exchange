#pragma once
#include "meridian/engine.hpp"
#include "meridian/journal.hpp"
#include <deque>
#include <map>
#include <set>
#include <string>

namespace meridian {
using AccountId = std::uint64_t;
struct AccountConfig {
    AccountId id{};
    std::string token;
    std::uint64_t cash{}, inventory{}, max_order_quantity{}, max_open_notional{}, max_position{};
};
std::vector<AccountConfig> read_accounts(const std::filesystem::path& path);
std::uint64_t configuration_hash(const std::vector<AccountConfig>& accounts);

enum class Code : std::uint64_t {
    Accepted, Cancelled, Invalid, DuplicateOrder, UnknownOrder, Capacity,
    Stale, Gap, Conflict, UnknownAccount, NotOwner, Halted, OrderLimit,
    Funds, Inventory, Exposure, Position, Overflow, SelfTrade, Killed, Resumed
};
std::string_view name(Code code);
struct Balance {
    std::uint64_t cash{}, inventory{}, reserved_cash{}, reserved_inventory{}, buy_quantity{}, open_notional{};
    bool halted{};
    bool operator==(const Balance&) const = default;
};
struct Outcome {
    std::uint64_t global_sequence{}, request_sequence{};
    Code code{Code::Invalid};
    Quantity remaining{}, filled{};
    std::uint64_t executions{}, cancelled{}, event_sequence{};
    Balance balance{};
    Quote quote{};
    bool operator==(const Outcome&) const = default;
};
struct Event {
    std::uint64_t sequence{}, global_sequence{};
    // type 1 = trade, type 2 = authoritative top-of-book quote after a request.
    std::uint64_t type{};
    Trade trade{};
    Quote quote{};
    bool operator==(const Event&) const = default;
};
struct Feed {
    bool gap{};
    std::uint64_t latest{};
    Quote quote{};
    std::vector<Event> events;
};

// Single-writer state machine. The gateway serializes calls externally; no
// socket or slow consumer is held inside this class.
class ExchangeState {
public:
    ExchangeState(std::size_t capacity, const std::vector<AccountConfig>& accounts,
                  std::size_t event_capacity = 8192, FaultHook fault = {});
    [[nodiscard]] bool authenticate(AccountId account, std::string_view token) const;
    [[nodiscard]] std::optional<Outcome> retry(const Request& request) const;
    Outcome apply(const Request& request, std::uint64_t global_sequence);
    [[nodiscard]] Balance balance(AccountId account) const;
    [[nodiscard]] std::uint64_t last_request(AccountId account) const;
    [[nodiscard]] std::uint64_t sequence() const { return sequence_; }
    [[nodiscard]] std::uint64_t event_sequence() const { return event_sequence_; }
    [[nodiscard]] std::size_t order_count() const { return engine_.size(); }
    [[nodiscard]] Feed feed(std::uint64_t after, std::size_t limit) const;
    [[nodiscard]] Quote quote() const { return engine_.quote(); }
    [[nodiscard]] std::vector<Order> snapshot() const { return engine_.snapshot(); }
    [[nodiscard]] std::uint64_t state_hash() const;
    void verify() const;
private:
    struct Account {
        AccountConfig config;
        Balance balance;
        std::set<OrderId> orders;
        std::uint64_t last{};
        Request request{};
        Outcome outcome{};
    };
    struct OwnedOrder { AccountId owner; Order order; };
    Engine engine_;
    FaultHook fault_;
    std::map<AccountId, Account> accounts_;
    std::unordered_map<OrderId, OwnedOrder> orders_;
    std::deque<Event> events_;
    std::size_t event_capacity_;
    std::uint64_t sequence_{}, event_sequence_{}, total_cash_{}, total_inventory_{};
    Outcome reject(const Request& request, Code code) const;
    Code validate_new(const Request& request) const;
    void release(Account& account, const Order& order, Quantity quantity);
    void cancel(OrderId id);
    void emit(Event event);
};

class DurableExchange {
public:
    DurableExchange(const std::filesystem::path& path, std::size_t capacity,
                    const std::vector<AccountConfig>& accounts, Durability durability,
                    FaultHook fault = {}, std::shared_ptr<JournalIO> io = {});
    Outcome execute(const Request& request);
    [[nodiscard]] const ExchangeState& state() const { return state_; }
private:
    ExchangeState state_;
    Journal journal_;
    FaultHook fault_;
    bool failed_{};
};
} // namespace meridian
