#include "meridian/protocol.hpp"
#include <bit>
#include <stdexcept>

namespace meridian::wire {
namespace {
void put(std::vector<unsigned char>& out, std::uint64_t value, int width) {
    for (int i = width - 1; i >= 0; --i) out.push_back(static_cast<unsigned char>(value >> (i * 8)));
}
std::uint64_t get(std::span<const unsigned char> data) {
    std::uint64_t value = 0;
    for (auto byte : data) value = (value << 8) | byte;
    return value;
}
} // namespace
void Writer::u64(std::uint64_t value) { put(bytes, value, 8); }
void Writer::balance(const Balance& b) {
    for (auto value : {b.cash,b.inventory,b.reserved_cash,b.reserved_inventory,b.buy_quantity,b.open_notional,
                      static_cast<std::uint64_t>(b.halted)}) u64(value);
}
void Writer::quote(const Quote& q) {
    u64(static_cast<std::uint64_t>(q.bid)); u64(static_cast<std::uint64_t>(q.ask));
    u64(q.bid_quantity); u64(q.ask_quantity);
}
std::uint64_t Reader::u64() {
    if (bytes_.size() - position_ < 8) throw std::runtime_error("truncated protocol integer");
    const auto value = get(bytes_.subspan(position_,8)); position_ += 8; return value;
}
std::string Reader::text(std::size_t size) {
    if (size > bytes_.size() - position_) throw std::runtime_error("truncated protocol text");
    const std::string text(reinterpret_cast<const char*>(bytes_.data()+position_),size);
    position_ += size; return text;
}
void Reader::finish() const { if (position_ != bytes_.size()) throw std::runtime_error("unexpected trailing payload"); }
Header decode_header(std::span<const unsigned char> bytes) {
    if (bytes.size() != header_size || bytes[0]!='M' || bytes[1]!='D' || bytes[2]!='X' || bytes[3]!='1' ||
        get(bytes.subspan(4,2)) != 1 || get(bytes.subspan(12,4)) != 0)
        throw std::runtime_error("invalid protocol header");
    const auto length = get(bytes.subspan(8,4));
    if (length > max_payload) throw std::runtime_error("protocol frame exceeds bound");
    return {static_cast<std::uint16_t>(get(bytes.subspan(6,2))), static_cast<std::uint32_t>(length)};
}
std::vector<unsigned char> encode(const Frame& frame) {
    if (frame.payload.size() > max_payload) throw std::runtime_error("response exceeds frame bound");
    std::vector<unsigned char> out{'M','D','X','1'};
    out.reserve(header_size + frame.payload.size());
    put(out,1,2); put(out,frame.type,2); put(out,frame.payload.size(),4); put(out,0,4);
    out.insert(out.end(),frame.payload.begin(),frame.payload.end());
    return out;
}
Request decode_request(std::span<const unsigned char> bytes, AccountId account_id) {
    Reader input(bytes);
    const auto sequence = input.u64(), kind = input.u64(), id = input.u64(), side = input.u64();
    const auto price = std::bit_cast<Price>(input.u64());
    const auto quantity = input.u64(); input.finish();
    if (kind < 1 || kind > 4 || side < 1 || side > 2) throw std::runtime_error("unknown order kind or side");
    return {account_id, sequence, {static_cast<Kind>(kind), id, static_cast<Side>(side), price, quantity}};
}
Frame outcome(const Outcome& result) {
    Writer out;
    for (auto value : {result.global_sequence,result.request_sequence,static_cast<std::uint64_t>(result.code),
                      result.remaining,result.filled,result.executions,result.cancelled,result.event_sequence}) out.u64(value);
    out.balance(result.balance); out.quote(result.quote);
    return {static_cast<std::uint16_t>(Submit | 0x8000), std::move(out.bytes)};
}
Frame error(std::uint64_t code) { Writer out; out.u64(code); return {Error, std::move(out.bytes)}; }
Frame account(AccountId id, const ExchangeState& state, std::uint16_t type) {
    Writer out;
    out.u64(id); out.u64(state.last_request(id)); out.u64(state.sequence());
    out.u64(state.event_sequence());
    out.balance(state.balance(id)); out.quote(state.quote());
    return {static_cast<std::uint16_t>(type | 0x8000), std::move(out.bytes)};
}
Frame feed(const Feed& events) {
    Writer out;
    out.u64(events.gap); out.u64(events.latest); out.quote(events.quote); out.u64(events.events.size());
    for (const auto& e : events.events) {
        out.u64(e.sequence); out.u64(e.global_sequence); out.u64(e.type);
        out.u64(e.trade.maker); out.u64(e.trade.taker); out.u64(static_cast<std::uint64_t>(e.trade.price)); out.u64(e.trade.quantity);
        out.quote(e.quote);
    }
    return {static_cast<std::uint16_t>(Events | 0x8000), std::move(out.bytes)};
}
} // namespace meridian::wire
