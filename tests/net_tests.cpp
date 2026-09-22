#include "meridian/net.hpp"
#include <cerrno>
#include <iostream>
#include <stdexcept>

int main() {
    using meridian::net::AcceptError;
    const auto check = [](int error, AcceptError expected) {
        if (meridian::net::classify_accept_error(error) != expected)
            throw std::runtime_error("incorrect accept error policy");
    };
    try {
#ifdef _WIN32
        for (auto error : {WSAEWOULDBLOCK, WSAEINTR, WSAECONNABORTED, WSAECONNRESET, WSAENETDOWN, WSAENETUNREACH, WSAEHOSTUNREACH}) check(error, AcceptError::Retry);
        for (auto error : {WSAEMFILE, WSAENOBUFS}) check(error, AcceptError::ResourcePressure);
        check(WSAENOTSOCK, AcceptError::Fatal);
#else
        for (auto error : {EAGAIN, EINTR, ECONNABORTED, ENETDOWN, EPROTO, ENOPROTOOPT, EHOSTDOWN, ENONET, EHOSTUNREACH, EOPNOTSUPP, ENETUNREACH}) check(error, AcceptError::Retry);
        for (auto error : {EMFILE, ENFILE, ENOBUFS, ENOMEM}) check(error, AcceptError::ResourcePressure);
        check(EBADF, AcceptError::Fatal);
#endif
        std::cout << "PASS transient, resource and fatal accept errors\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
