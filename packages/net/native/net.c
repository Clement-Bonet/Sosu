/* net: the sockets of the system (Winsock on Windows, BSD sockets elsewhere) behind small
   functions of plain C types, for the declarations of src/ip.sosu, src/tcp.sosu and src/udp.sosu.

   An address is 18 bytes: its family (4 or 6), a byte 0, then 16 bytes of IP (an IPv4 address
   in the first 4). A socket is an int64_t, -1 when there is none. A function that fails sets
   `*error` to one of the NET_ codes below, which src/ip.sosu turns into `NetError`. */
/* Before any #include: glibc reads it at the first one. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* -std=c99 hides getaddrinfo, inet_ntop... */
#endif
#include <stdint.h>
#include <string.h>

enum {
    NET_OK = 0,
    NET_NOT_FOUND = 1,     /* the name is not known */
    NET_REFUSED = 2,       /* nothing listens there */
    NET_TIMED_OUT = 3,
    NET_UNREACHABLE = 4,   /* no route to the host, or the network is down */
    NET_ADDRESS_IN_USE = 5,
    NET_CLOSED = 6,        /* the other side closed or reset the connection */
    NET_INVALID = 7,       /* an address or an argument the system refuses */
    NET_DENIED = 8,        /* a permission is missing (a port below 1024, a firewall) */
    NET_OTHER = 9,
};

#ifdef _WIN32
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0601
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 /* inet_ntop, with MinGW too */
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

typedef SOCKET Socket;
typedef int Length; /* of a buffer, for recv and send */
#define BAD_SOCKET INVALID_SOCKET

/* WSAStartup counts its calls: one per process is enough, never undone. */
static volatile LONG started = 0;
static void start(void) {
    if (InterlockedCompareExchange(&started, 1, 0) == 0) {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
}

static int last_error(void) {
    switch (WSAGetLastError()) {
    case WSAECONNREFUSED: return NET_REFUSED;
    case WSAETIMEDOUT: case WSAEWOULDBLOCK: return NET_TIMED_OUT;
    case WSAENETUNREACH: case WSAEHOSTUNREACH: case WSAENETDOWN: return NET_UNREACHABLE;
    case WSAEADDRINUSE: return NET_ADDRESS_IN_USE;
    case WSAECONNRESET: case WSAECONNABORTED: case WSAESHUTDOWN: case WSAENOTCONN: case WSAENETRESET: return NET_CLOSED;
    case WSAEADDRNOTAVAIL: case WSAEINVAL: case WSAEAFNOSUPPORT: case WSAENOTSOCK: case WSAEMSGSIZE: return NET_INVALID;
    case WSAEACCES: return NET_DENIED;
    default: return NET_OTHER;
    }
}

static void close_socket(Socket s) { closesocket(s); }

static int set_blocking(Socket s, int blocking) {
    u_long mode = blocking ? 0 : 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}

static int in_progress(void) { return WSAGetLastError() == WSAEWOULDBLOCK; }

#else
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef int Socket;
typedef size_t Length;
#define BAD_SOCKET (-1)

static void start(void) { }

static int error_of(int e) {
    switch (e) {
    case ECONNREFUSED: return NET_REFUSED;
    case ETIMEDOUT: case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
        return NET_TIMED_OUT;
    case ENETUNREACH: case EHOSTUNREACH: case ENETDOWN: return NET_UNREACHABLE;
    case EADDRINUSE: return NET_ADDRESS_IN_USE;
    case ECONNRESET: case ECONNABORTED: case EPIPE: case ENOTCONN: case ESHUTDOWN: return NET_CLOSED;
    case EADDRNOTAVAIL: case EINVAL: case EAFNOSUPPORT: case ENOTSOCK: case EMSGSIZE: return NET_INVALID;
    case EACCES: case EPERM: return NET_DENIED;
    default: return NET_OTHER;
    }
}

static int last_error(void) { return error_of(errno); }

static void close_socket(Socket s) { close(s); }

static int set_blocking(Socket s, int blocking) {
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return 0;
    flags = blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK;
    return fcntl(s, F_SETFL, flags) == 0;
}

static int in_progress(void) { return errno == EINPROGRESS; }
#endif

/* Writing to a socket the other side closed: an error, never the signal SIGPIPE. */
#if defined(MSG_NOSIGNAL)
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

static void no_sigpipe(Socket s) {
#if defined(SO_NOSIGPIPE)
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, (const char *)&on, sizeof on);
#else
    (void)s;
#endif
}

/* The 18 bytes of an address, from a sockaddr; its port in `*port`. */
static void address_of(const struct sockaddr *sa, uint8_t *out, uint16_t *port) {
    memset(out, 0, 18);
    if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
        out[0] = 6;
        memcpy(out + 2, &in6->sin6_addr, 16);
        if (port) *port = ntohs(in6->sin6_port);
    } else {
        const struct sockaddr_in *in4 = (const struct sockaddr_in *)sa;
        out[0] = 4;
        memcpy(out + 2, &in4->sin_addr, 4);
        if (port) *port = ntohs(in4->sin_port);
    }
}

/* A sockaddr from the 18 bytes of an address and a port; its size. */
static socklen_t sockaddr_of(const uint8_t *address, uint16_t port, struct sockaddr_storage *out) {
    memset(out, 0, sizeof *out);
    if (address[0] == 6) {
        struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)out;
        in6->sin6_family = AF_INET6;
        in6->sin6_port = htons(port);
        memcpy(&in6->sin6_addr, address + 2, 16);
        return (socklen_t)sizeof *in6;
    }
    struct sockaddr_in *in4 = (struct sockaddr_in *)out;
    in4->sin_family = AF_INET;
    in4->sin_port = htons(port);
    memcpy(&in4->sin_addr, address + 2, 4);
    return (socklen_t)sizeof *in4;
}

/* The addresses of `host` (a NUL-ended name, or an IP address written out), at most `max`, in
   `out` (18 bytes each): IPv4 first, as a client tries them in order. Gives how many, or -1 with
   `*error` set. */
int32_t sosu_net_resolve(const uint8_t *host, uint8_t *out, int32_t max, int32_t *error) {
    start();
    struct addrinfo hints, *list = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo((const char *)host, NULL, &hints, &list);
    if (rc != 0) {
#ifdef EAI_SYSTEM
        *error = rc == EAI_SYSTEM ? last_error() : rc == EAI_AGAIN ? NET_UNREACHABLE : NET_NOT_FOUND;
#else
        *error = rc == EAI_AGAIN ? NET_UNREACHABLE : NET_NOT_FOUND;
#endif
        return -1;
    }
    int32_t count = 0;
    for (int family = 4; family <= 6; family += 2) {
        for (struct addrinfo *a = list; a && count < max; a = a->ai_next) {
            if ((family == 4 && a->ai_family != AF_INET) || (family == 6 && a->ai_family != AF_INET6)) continue;
            uint8_t candidate[18];
            address_of(a->ai_addr, candidate, NULL);
            int seen = 0; /* getaddrinfo may give an address once per protocol */
            for (int32_t i = 0; i < count && !seen; i++) seen = memcmp(out + 18 * i, candidate, 18) == 0;
            if (!seen) memcpy(out + 18 * count++, candidate, 18);
        }
    }
    freeaddrinfo(list);
    *error = count ? NET_OK : NET_NOT_FOUND;
    return count ? count : -1;
}

/* An address written out ("192.168.0.1", "::1") in `out` (64 bytes, NUL-ended); its length. */
int32_t sosu_net_address_text(const uint8_t *address, uint8_t *out) {
    start();
    char text[64] = { 0 };
    if (!inet_ntop(address[0] == 6 ? AF_INET6 : AF_INET, (const void *)(address + 2), text, sizeof text)) text[0] = '\0';
    size_t n = strlen(text);
    memcpy(out, text, n + 1);
    return (int32_t)n;
}

/* A connection to `address`:`port`, waiting `timeout_ms` at most (0: as long as the system
   does), or -1 with `*error` set. */
int64_t sosu_net_tcp_connect(const uint8_t *address, uint16_t port, int32_t timeout_ms, int32_t *error) {
    start();
    struct sockaddr_storage sa;
    socklen_t size = sockaddr_of(address, port, &sa);
    Socket s = socket(sa.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (s == BAD_SOCKET) {
        *error = last_error();
        return -1;
    }
    no_sigpipe(s);
    if (timeout_ms > 0 && !set_blocking(s, 0)) {
        *error = last_error();
        close_socket(s);
        return -1;
    }
    if (connect(s, (struct sockaddr *)&sa, size) != 0) {
        if (timeout_ms <= 0 || !in_progress()) {
            *error = last_error();
            close_socket(s);
            return -1;
        }
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(s, &writable);
        FD_SET(s, &failed);
        struct timeval wait = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        int ready = select((int)s + 1, NULL, &writable, &failed, &wait);
        if (ready <= 0) {
            *error = ready == 0 ? NET_TIMED_OUT : last_error();
            close_socket(s);
            return -1;
        }
        int failure = 0;
        socklen_t length = (socklen_t)sizeof failure;
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&failure, &length);
        if (failure != 0) {
#ifdef _WIN32
            WSASetLastError(failure);
            *error = last_error();
#else
            *error = error_of(failure);
#endif
            close_socket(s);
            return -1;
        }
    }
    if (timeout_ms > 0) set_blocking(s, 1);
    int on = 1; /* requests and answers are sent at once, not gathered */
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
    *error = NET_OK;
    return (int64_t)s;
}

/* A socket that listens on `address`:`port` (port 0: one the system chooses), or -1. */
int64_t sosu_net_tcp_listen(const uint8_t *address, uint16_t port, int32_t backlog, int32_t *error) {
    start();
    struct sockaddr_storage sa;
    socklen_t size = sockaddr_of(address, port, &sa);
    Socket s = socket(sa.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (s == BAD_SOCKET) {
        *error = last_error();
        return -1;
    }
#ifndef _WIN32
    int on = 1; /* a server restarted at once takes its port again */
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&on, sizeof on);
#endif
    if (bind(s, (struct sockaddr *)&sa, size) != 0 || listen(s, backlog > 0 ? backlog : SOMAXCONN) != 0) {
        *error = last_error();
        close_socket(s);
        return -1;
    }
    *error = NET_OK;
    return (int64_t)s;
}

/* The next connection to a listening socket, or -1; who connected in `address` and `*port`. */
int64_t sosu_net_tcp_accept(int64_t listener, uint8_t *address, uint16_t *port, int32_t *error) {
    struct sockaddr_storage sa;
    socklen_t size = (socklen_t)sizeof sa;
    Socket s = accept((Socket)listener, (struct sockaddr *)&sa, &size);
    if (s == BAD_SOCKET) {
        *error = last_error();
        return -1;
    }
    no_sigpipe(s);
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
    address_of((struct sockaddr *)&sa, address, port);
    *error = NET_OK;
    return (int64_t)s;
}

/* The address and port a socket is bound to (its own side). */
int32_t sosu_net_local(int64_t s, uint8_t *address, uint16_t *port) {
    struct sockaddr_storage sa;
    socklen_t size = (socklen_t)sizeof sa;
    if (getsockname((Socket)s, (struct sockaddr *)&sa, &size) != 0) return last_error();
    address_of((struct sockaddr *)&sa, address, port);
    return NET_OK;
}

/* The longest a read or a write waits, in milliseconds (0: no limit). */
int32_t sosu_net_set_timeout(int64_t s, int32_t timeout_ms) {
#ifdef _WIN32
    DWORD wait = (DWORD)(timeout_ms > 0 ? timeout_ms : 0);
#else
    struct timeval wait = { timeout_ms > 0 ? timeout_ms / 1000 : 0, timeout_ms > 0 ? (timeout_ms % 1000) * 1000 : 0 };
#endif
    if (setsockopt((Socket)s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&wait, sizeof wait) != 0 ||
        setsockopt((Socket)s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&wait, sizeof wait) != 0) {
        return last_error();
    }
    return NET_OK;
}

/* Reads at most `size` bytes: how many (0: the other side closed), or -1 with `*error` set. */
int64_t sosu_net_recv(int64_t s, uint8_t *buffer, int64_t size, int32_t *error) {
    int n = (int)(size > 0x40000000 ? 0x40000000 : size);
    for (;;) {
        int got = (int)recv((Socket)s, (char *)buffer, (Length)n, 0);
        if (got >= 0) {
            *error = NET_OK;
            return got;
        }
#ifndef _WIN32
        if (errno == EINTR) continue;
#endif
        *error = last_error();
        return -1;
    }
}

/* Writes at most `size` bytes: how many, or -1 with `*error` set. */
int64_t sosu_net_send(int64_t s, const uint8_t *buffer, int64_t size, int32_t *error) {
    int n = (int)(size > 0x40000000 ? 0x40000000 : size);
    for (;;) {
        int sent = (int)send((Socket)s, (const char *)buffer, (Length)n, SEND_FLAGS);
        if (sent >= 0) {
            *error = NET_OK;
            return sent;
        }
#ifndef _WIN32
        if (errno == EINTR) continue;
#endif
        *error = last_error();
        return -1;
    }
}

/* No more writes: the other side reads the end of the stream. */
void sosu_net_shutdown(int64_t s) {
#ifdef _WIN32
    shutdown((Socket)s, SD_SEND);
#else
    shutdown((Socket)s, SHUT_WR);
#endif
}

void sosu_net_close(int64_t s) { close_socket((Socket)s); }

/* A UDP socket bound to `address`:`port` (port 0: one the system chooses), or -1. */
int64_t sosu_net_udp_open(const uint8_t *address, uint16_t port, int32_t *error) {
    start();
    struct sockaddr_storage sa;
    socklen_t size = sockaddr_of(address, port, &sa);
    Socket s = socket(sa.ss_family, SOCK_DGRAM, IPPROTO_UDP);
    if (s == BAD_SOCKET) {
        *error = last_error();
        return -1;
    }
    if (bind(s, (struct sockaddr *)&sa, size) != 0) {
        *error = last_error();
        close_socket(s);
        return -1;
    }
    *error = NET_OK;
    return (int64_t)s;
}

/* Sends one datagram to `address`:`port`: its size, or -1 with `*error` set. */
int64_t sosu_net_send_to(int64_t s, const uint8_t *address, uint16_t port, const uint8_t *buffer, int64_t size,
                         int32_t *error) {
    struct sockaddr_storage sa;
    socklen_t length = sockaddr_of(address, port, &sa);
    int sent = (int)sendto((Socket)s, (const char *)buffer, (Length)(size > 65535 ? 65535 : size), 0,
                           (struct sockaddr *)&sa, length);
    if (sent < 0) {
        *error = last_error();
        return -1;
    }
    *error = NET_OK;
    return sent;
}

/* Receives one datagram, cut to `size` bytes: its size, or -1; who sent it in `address`, `*port`. */
int64_t sosu_net_recv_from(int64_t s, uint8_t *buffer, int64_t size, uint8_t *address, uint16_t *port,
                           int32_t *error) {
    struct sockaddr_storage sa;
    socklen_t length = (socklen_t)sizeof sa;
    for (;;) {
        int got = (int)recvfrom((Socket)s, (char *)buffer, (Length)(size > 65535 ? 65535 : size), 0,
                                (struct sockaddr *)&sa, &length);
        if (got >= 0) {
            address_of((struct sockaddr *)&sa, address, port);
            *error = NET_OK;
            return got;
        }
#ifdef _WIN32
        if (WSAGetLastError() == WSAEMSGSIZE) { /* cut, as on the other systems */
            address_of((struct sockaddr *)&sa, address, port);
            *error = NET_OK;
            return size;
        }
#else
        if (errno == EINTR) continue;
#endif
        *error = last_error();
        return -1;
    }
}
