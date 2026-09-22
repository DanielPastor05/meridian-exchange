#pragma once
#include "meridian/service.hpp"
#include <span>

namespace meridian::wire {
constexpr std::size_t max_payload = 65536;
constexpr std::size_t header_size = 16;
enum Type : std::uint16_t { Hello=1, Submit=2, Account=3, Events=4, Book=5, Ping=6, Metrics=7, Error=65535 };
struct Header { std::uint16_t type; std::uint32_t length; };
struct Frame { std::uint16_t type{}; std::vector<unsigned char> payload; };
struct Writer {
    std::vector<unsigned char> bytes;
    void u64(std::uint64_t value);
    void balance(const Balance& value);
    void quote(const Quote& value);
};
class Reader {
public:
    explicit Reader(std::span<const unsigned char> bytes) : bytes_(bytes) {}
    std::uint64_t u64();
    std::string text(std::size_t length);
    void finish() const;
private:
    std::span<const unsigned char> bytes_;
    std::size_t position_{};
};
Header decode_header(std::span<const unsigned char> bytes);
std::vector<unsigned char> encode(const Frame& frame);
Request decode_request(std::span<const unsigned char> bytes, AccountId account);
Frame outcome(const Outcome& result);
Frame error(std::uint64_t code);
Frame account(AccountId id, const ExchangeState& state, std::uint16_t type);
Frame feed(const Feed& events);
} // namespace meridian::wire
