#include "meridian/net.hpp"
#include <array>
#include <cerrno>
#include <stdexcept>
#include <utility>
#ifndef _WIN32
#include <fcntl.h>
#include <netinet/tcp.h>
#endif

namespace meridian::net {
namespace {
void close(Handle socket) {
    if (socket == invalid) return;
#ifdef _WIN32
    closesocket(socket);
#else
    ::close(socket);
#endif
}
bool pending() {
#ifdef _WIN32
    const auto e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINTR;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}
void nonblocking(Handle socket) {
#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(socket,FIONBIO,&mode) != 0) throw std::runtime_error("cannot set nonblocking socket");
#else
    const int flags = fcntl(socket,F_GETFL,0);
    if (flags < 0 || fcntl(socket,F_SETFL,flags|O_NONBLOCK) != 0) throw std::runtime_error("cannot set nonblocking socket");
#endif
}
bool wait(Handle socket, bool reading, int ms) {
    #ifndef _WIN32
    if (socket < 0 || socket >= FD_SETSIZE) throw std::runtime_error("socket exceeds select descriptor limit");
    #endif
    fd_set sockets; FD_ZERO(&sockets); FD_SET(socket,&sockets);
    timeval timeout{ms/1000,(ms%1000)*1000};
#ifdef _WIN32
    const auto result = select(0,reading?&sockets:nullptr,reading?nullptr:&sockets,nullptr,&timeout);
#else
    const auto result = select(socket+1,reading?&sockets:nullptr,reading?nullptr:&sockets,nullptr,&timeout);
#endif
    if (result < 0 && !pending()) throw std::runtime_error("socket wait failed");
    return result > 0;
}
using Clock=std::chrono::steady_clock;
void deadline(Clock::time_point end, const std::atomic<bool>& stopping) {
    if (stopping.load()) throw std::runtime_error("server stopping");
    if (Clock::now() >= end) throw std::runtime_error("socket deadline exceeded");
}
bool read(Handle socket, std::span<unsigned char> bytes, Clock::time_point end, const std::atomic<bool>& stopping) {
    std::size_t position = 0;
    while (position < bytes.size()) {
        deadline(end,stopping);
        if (!wait(socket,true,50)) continue;
        const auto count = ::recv(socket,reinterpret_cast<char*>(bytes.data()+position),
                                  static_cast<int>(bytes.size()-position),0);
        if (count == 0) {
            if (position == 0) return false;
            throw std::runtime_error("connection ended inside a frame");
        }
        if (count < 0) { if (pending()) continue; throw std::runtime_error("socket read failed"); }
        position += static_cast<std::size_t>(count);
    }
    return true;
}
} // namespace

Runtime::Runtime() {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2,2),&data) != 0) throw std::runtime_error("Winsock startup failed");
#endif
}
Runtime::~Runtime() {
#ifdef _WIN32
    WSACleanup();
#endif
}
Socket::~Socket() { close(handle_); }
Socket::Socket(Socket&& other) noexcept : handle_(std::exchange(other.handle_,invalid)) {}
Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) { close(handle_); handle_ = std::exchange(other.handle_,invalid); }
    return *this;
}
Listener listen(std::string_view address, std::uint16_t port, int backlog) {
    Socket socket(::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));
    if (socket.get() == invalid) throw std::runtime_error("cannot create listening socket");
    int enabled = 1;
#ifdef _WIN32
    if (setsockopt(socket.get(),SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&enabled),sizeof(enabled)) != 0)
#else
    if (setsockopt(socket.get(),SOL_SOCKET,SO_REUSEADDR,&enabled,sizeof(enabled)) != 0)
#endif
        throw std::runtime_error("cannot configure listening socket");
    sockaddr_in endpoint{}; endpoint.sin_family = AF_INET; endpoint.sin_port = htons(port);
    if (inet_pton(AF_INET,std::string(address).c_str(),&endpoint.sin_addr) != 1) throw std::runtime_error("invalid IPv4 bind address");
    if (::bind(socket.get(),reinterpret_cast<sockaddr*>(&endpoint),sizeof(endpoint)) != 0 || ::listen(socket.get(),backlog) != 0)
        throw std::runtime_error("cannot bind/listen; check address and port");
#ifdef _WIN32
    int length = sizeof(endpoint);
#else
    socklen_t length = sizeof(endpoint);
#endif
    if (getsockname(socket.get(),reinterpret_cast<sockaddr*>(&endpoint),&length) != 0) throw std::runtime_error("cannot read bound port");
    nonblocking(socket.get());
    return {std::move(socket),ntohs(endpoint.sin_port)};
}
Socket accept(Handle listener) {
    Socket socket(::accept(listener,nullptr,nullptr));
    if (socket.get() == invalid) { if (pending()) return socket; throw std::runtime_error("accept failed"); }
    nonblocking(socket.get());
    int enabled = 1;
    if (setsockopt(socket.get(),IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&enabled),sizeof(enabled)) != 0)
        throw std::runtime_error("cannot disable Nagle");
    return socket;
}
bool readable(Handle socket, int milliseconds) { return wait(socket,true,milliseconds); }
bool receive(Handle socket, wire::Frame& frame, int timeout_ms, const std::atomic<bool>& stopping) {
    const auto end = Clock::now()+std::chrono::milliseconds(timeout_ms);
    std::array<unsigned char,wire::header_size> bytes{};
    if (!read(socket,bytes,end,stopping)) return false;
    const auto header = wire::decode_header(bytes);
    frame.type = header.type;
    frame.payload.resize(header.length);
    if (!frame.payload.empty() && !read(socket,frame.payload,end,stopping)) throw std::runtime_error("missing frame payload");
    return true;
}
void send(Handle socket, const wire::Frame& frame, int timeout_ms, const std::atomic<bool>& stopping) {
    const auto bytes = wire::encode(frame);
    const auto end = Clock::now()+std::chrono::milliseconds(timeout_ms);
    std::size_t position = 0;
    while (position < bytes.size()) {
        deadline(end,stopping);
        if (!wait(socket,false,50)) continue;
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const auto count = ::send(socket,reinterpret_cast<const char*>(bytes.data()+position),
                                  static_cast<int>(bytes.size()-position),flags);
        if (count < 0) { if (pending()) continue; throw std::runtime_error("socket write failed"); }
        if (count == 0) throw std::runtime_error("socket write made no progress");
        position += static_cast<std::size_t>(count);
    }
}
} // namespace meridian::net
