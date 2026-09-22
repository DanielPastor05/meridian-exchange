#pragma once
#include "meridian/protocol.hpp"
#include <atomic>
#include <chrono>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace meridian::net {
#ifdef _WIN32
using Handle = SOCKET;
constexpr Handle invalid = INVALID_SOCKET;
#else
using Handle = int;
constexpr Handle invalid = -1;
#endif
class Runtime {
public:
    Runtime();
    ~Runtime();
};
class Socket {
public:
    explicit Socket(Handle handle = invalid) : handle_(handle) {}
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    [[nodiscard]] Handle get() const { return handle_; }
private:
    Handle handle_;
};
struct Listener { Socket socket; std::uint16_t port; };
Listener listen(std::string_view address, std::uint16_t port, int backlog);
enum class AcceptError { Retry, ResourcePressure, Fatal };
[[nodiscard]] AcceptError classify_accept_error(int error);
struct AcceptResult { Socket socket; AcceptError error{AcceptError::Retry}; int system_error{}; };
AcceptResult accept(Handle listener);
bool readable(Handle socket, int milliseconds);
bool receive(Handle socket, wire::Frame& frame, int timeout_ms, const std::atomic<bool>& stopping);
void send(Handle socket, const wire::Frame& frame, int timeout_ms, const std::atomic<bool>& stopping);
} // namespace meridian::net
