#include "clog_port.h"
#include "socket_async.h"
#include "socket_async_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef CLOG_USE_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

/* ── Exponential Backoff Helpers ── */

uint32_t backoff_next(uint32_t current_ms, uint32_t max_ms)
{
    uint32_t next = current_ms * 2;
    if (next > max_ms) {
        next = max_ms;
    }
    uint32_t jitter = next / 10;
    uint32_t seed   = (uint32_t)clog_get_now_ms();
    uint32_t delta  = (seed % (2 * jitter + 1));
    if (delta > jitter) {
        next += (delta - jitter);
    } else {
        next -= (jitter - delta);
    }
    if (next < 100) {
        next = 100;
    }
    return next;
}

void backoff_sleep(uint32_t ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000);
    nanosleep(&ts, NULL);
#endif
}

/* ── Non-blocking Socket Helpers ── */

int socket_set_nonblocking(clog_socket_t sockfd)
{
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(sockfd, FIONBIO, &mode);
#else
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
#endif
}

int socket_connect_nonblocking(clog_socket_t sockfd,
                               const char   *host,
                               int           port,
                               uint32_t      timeout_ms)
{
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &serv_addr.sin_addr) <= 0) {
        return -1;
    }

    int rc = connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
    if (rc == 0) {
        return 0; /* Connected immediately. */
    }

#if defined(_WIN32)
    if (WSAGetLastError() == WSAEWOULDBLOCK) {
#else
    if (errno == EINPROGRESS) {
#endif
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(sockfd, &writefds);

        if (timeout_ms == 0) {
            timeout_ms = 1000;
        }
        struct timeval tv;
        tv.tv_sec  = (long)(timeout_ms / 1000);
        tv.tv_usec = (long)((timeout_ms % 1000) * 1000);

        int sel = select((int)sockfd + 1, NULL, &writefds, NULL, &tv);
        if (sel > 0) {
            int       err    = 0;
            socklen_t errlen = sizeof(err);
            getsockopt(sockfd, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen);
            return (err == 0) ? 0 : -1;
        }
        return -1; /* Timeout or error. */
    }

    return -1;
}

int socket_send_nonblocking(clog_socket_t sockfd, const char *buf, size_t len)
{
    size_t total_sent = 0;

    while (total_sent < len) {
        long sent = send(sockfd,
                         buf + total_sent,
                         (clog_sock_size_t)(len - total_sent),
#if !defined(_WIN32)
                         MSG_NOSIGNAL
#else
                         0
#endif
        );
        if (sent < 0) {
#if defined(_WIN32)
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINTR) {
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
#endif
                return (int)total_sent > 0 ? (int)total_sent : -1;
            }
            return -1;
        }
        if (sent == 0) {
            break;
        }
        total_sent += (size_t)sent;
    }

    return (int)total_sent;
}

#ifdef CLOG_USE_TLS
int tls_send_nonblocking(SSL *ssl, const char *buf, size_t len)
{
    size_t total_sent = 0;

    while (total_sent < len) {
        int sent = SSL_write(ssl, buf + total_sent, (int)(len - total_sent));
        if (sent <= 0) {
            int err = SSL_get_error(ssl, sent);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                return (int)total_sent > 0 ? (int)total_sent : -1;
            }
            return -1;
        }
        total_sent += (size_t)sent;
    }

    return (int)total_sent;
}
#endif
