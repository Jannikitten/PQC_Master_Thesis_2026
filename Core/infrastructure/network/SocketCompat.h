#ifndef SAFIRA_SOCKET_COMPAT_H
#define SAFIRA_SOCKET_COMPAT_H

// Cross-platform socket primitives. POSIX on macOS/Linux, WinSock2 on Windows.

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <basetsd.h>

    using ssize_t = SSIZE_T;
#else
    #include <arpa/inet.h>
    #include <fcntl.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
    #include <cerrno>
#endif

namespace Safira::net {

inline void EnsureWinsockInitialized() noexcept {
#ifdef _WIN32
    static const int s_init = []() noexcept {
        WSADATA d{};
        return ::WSAStartup(MAKEWORD(2, 2), &d);
    }();
    (void)s_init;
#endif
}

inline int CloseSocket(int fd) noexcept {
    if (fd < 0) return 0;
#ifdef _WIN32
    return ::closesocket(static_cast<SOCKET>(fd));
#else
    return ::close(fd);
#endif
}

inline int SetNonBlocking(int fd) noexcept {
#ifdef _WIN32
    u_long mode = 1;
    return ::ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &mode);
#else
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1) flags = 0;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

inline int LastSocketError() noexcept {
#ifdef _WIN32
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

inline bool IsWouldBlock(int err) noexcept {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK || err == WSAEINTR;
#else
    return err == EWOULDBLOCK || err == EAGAIN || err == EINTR;
#endif
}

inline bool IsConnReset(int err) noexcept {
#ifdef _WIN32
    return err == WSAECONNRESET || err == WSAENOTCONN || err == WSAESHUTDOWN
        || err == WSAECONNREFUSED || err == WSAECONNABORTED
        || err == WSAEHOSTUNREACH || err == WSAENETUNREACH;
#else
    return err == ECONNRESET || err == ENOTCONN || err == EPIPE
        || err == ECONNREFUSED || err == EHOSTUNREACH || err == ENETUNREACH;
#endif
}

inline bool IsBadSocket(int err) noexcept {
#ifdef _WIN32
    return err == WSAENOTSOCK || err == WSAEBADF || err == WSAEINVAL;
#else
    return err == EBADF || err == ENOTSOCK || err == EINVAL;
#endif
}

} // namespace Safira::net

#endif // SAFIRA_SOCKET_COMPAT_H
