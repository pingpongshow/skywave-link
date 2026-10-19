// Bounded TCP connects for the email gateway clients (POP3, SMTP, Winlink
// Telnet). Internal to src/email.
//
// Every gateway job must finish in bounded time: the connect gets its own
// deadline (instead of the OS default of a minute or more) and the returned
// socket carries SO_RCVTIMEO/SO_SNDTIMEO, which also bound TLS handshakes
// and every SSL_read/SSL_write since OpenSSL runs over this blocking socket.
#pragma once

#include <cstdint>
#include <string>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <errno.h>
    #include <fcntl.h>
    #include <netdb.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <unistd.h>
#endif

namespace skylink::netio {

#ifdef _WIN32
using Socket = SOCKET;
inline constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
using Socket = int;
inline constexpr Socket kInvalidSocket = -1;
#endif

inline constexpr int kConnectTimeoutMs = 5000;
inline constexpr int kIoTimeoutMs = 15000;

inline void closeSocket(Socket& sock) {
    if (sock == kInvalidSocket) return;
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
    sock = kInvalidSocket;
}

inline bool setIoTimeouts(Socket sock, int timeout_ms) {
#ifdef _WIN32
    DWORD ms = static_cast<DWORD>(timeout_ms);
    return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms)) == 0 &&
           setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms)) == 0;
#else
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
           setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
#endif
}

inline bool setBlocking(Socket sock, bool blocking) {
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(sock, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK)) != -1;
#endif
}

// Connects one address with a deadline; the socket stays non-blocking.
inline bool connectAddress(Socket sock, const sockaddr* addr, socklen_t len, int timeout_ms) {
    if (!setBlocking(sock, false)) return false;
#ifdef _WIN32
    if (::connect(sock, addr, static_cast<int>(len)) == 0) return true;
    if (WSAGetLastError() != WSAEWOULDBLOCK) return false;
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sock, &wfds);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    if (select(0, nullptr, &wfds, nullptr, &tv) != 1) return false;
    int err = 0;
    int err_len = sizeof(err);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &err_len);
    return err == 0;
#else
    if (::connect(sock, addr, len) == 0) return true;
    if (errno != EINPROGRESS) return false;
    pollfd pfd{sock, POLLOUT, 0};
    int rc;
    do {
        rc = ::poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);
    if (rc != 1) return false;
    int err = 0;
    socklen_t err_len = sizeof(err);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &err_len);
    return err == 0;
#endif
}

// Resolves `host` and connects to the first reachable address within
// `connect_timeout_ms` per address. Returns a blocking socket with
// `io_timeout_ms` read/write timeouts, or kInvalidSocket with *error set.
inline Socket connectTcp(const std::string& host, uint16_t port,
                         int connect_timeout_ms, int io_timeout_ms,
                         std::string* error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const std::string port_text = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result) != 0) {
        if (error) *error = "DNS resolution failed for " + host;
        return kInvalidSocket;
    }

    Socket sock = kInvalidSocket;
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == kInvalidSocket) continue;
#ifdef SO_NOSIGPIPE
        const int no_sigpipe = 1;
        (void)setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
        if (connectAddress(sock, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen),
                           connect_timeout_ms) &&
            setBlocking(sock, true) && setIoTimeouts(sock, io_timeout_ms)) {
            break;
        }
        closeSocket(sock);
    }
    freeaddrinfo(result);

    if (sock == kInvalidSocket && error) {
        *error = "Failed to connect to " + host + ":" + port_text;
    }
    return sock;
}

} // namespace skylink::netio
