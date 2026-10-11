#ifndef CLOGX_SOCKET_ASYNC_INTERNAL_H
#define CLOGX_SOCKET_ASYNC_INTERNAL_H

#include "clog_port.h"
#include "socket_async.h"

#ifdef CLOG_USE_TLS
#include <openssl/ssl.h>
#endif

/* ── Internal writer state ── */

struct socket_writer_t {
    socket_writer_config_t config;     /**< Copy of configuration. */
    socket_ring_buffer_t  *ring;       /**< Ring buffer (owned). */
    clog_thread_t          thread;     /**< Writer thread handle. */
    clog_socket_t          sockfd;     /**< Current socket fd. */
    int                    connected;  /**< 1 if socket is connected. */
    uint32_t               backoff_ms; /**< Current backoff delay. */
#ifdef CLOG_USE_TLS
    SSL_CTX *ssl_ctx; /**< TLS context (if enabled). */
    SSL     *ssl;     /**< TLS session (if enabled). */
#endif
};

uint32_t backoff_next(uint32_t current_ms, uint32_t max_ms);
void     backoff_sleep(uint32_t ms);
int      socket_set_nonblocking(clog_socket_t sockfd);
int      socket_connect_nonblocking(clog_socket_t sockfd,
                                    const char   *host,
                                    int           port,
                                    uint32_t      timeout_ms);
int      socket_send_nonblocking(clog_socket_t sockfd, const char *buf, size_t len);
#ifdef CLOG_USE_TLS
int tls_send_nonblocking(SSL *ssl, const char *buf, size_t len);
#endif

#endif /* CLOGX_SOCKET_ASYNC_INTERNAL_H */
