/**
 * @file socket_async.c
 * @brief Async non-blocking socket writer with ring buffer and exponential backoff.
 *
 * Implements the ring buffer for formatted log lines and a background writer
 * thread that drains the buffer over a TCP/TLS socket with non-blocking I/O.
 */

#include "socket_async.h"
#include "clog_port.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
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

/* ── Ring Buffer Implementation ── */

socket_ring_buffer_t *socket_ring_create(size_t capacity)
{
    if (capacity == 0) {
        return NULL;
    }

    socket_ring_buffer_t *ring = calloc(1, sizeof(socket_ring_buffer_t));
    if (!ring) {
        return NULL;
    }

    ring->slots = calloc(capacity, sizeof(socket_ring_slot_t));
    if (!ring->slots) {
        free(ring);
        return NULL;
    }

    ring->capacity = capacity;
    ring->head     = 0;
    ring->tail     = 0;
    ring->count    = 0;
    ring->dropped  = 0;
    ring->closed   = 0;
    clog_mutex_init(&ring->mutex);
    clog_cond_init(&ring->not_empty);

    return ring;
}

int socket_ring_put(socket_ring_buffer_t *ring, const char *line, size_t len)
{
    if (!ring || !line || len == 0) {
        return -1;
    }

    clog_mutex_lock(&ring->mutex);

    if (ring->closed) {
        clog_mutex_unlock(&ring->mutex);
        return -1;
    }

    /* If buffer is full, drop oldest entry (lossy backpressure). */
    if (ring->count == ring->capacity) {
        free(ring->slots[ring->tail].line);
        ring->slots[ring->tail].line = NULL;
        ring->slots[ring->tail].len  = 0;
        ring->tail                   = (ring->tail + 1) % ring->capacity;
        ring->count--;
        ring->dropped++;
    }

    /* Allocate and copy the line. */
    char *copy = malloc(len + 1);
    if (!copy) {
        clog_mutex_unlock(&ring->mutex);
        return -1;
    }
    memcpy(copy, line, len);
    copy[len] = '\0';

    ring->slots[ring->head].line = copy;
    ring->slots[ring->head].len  = len;
    ring->head                   = (ring->head + 1) % ring->capacity;
    ring->count++;

    clog_cond_signal(&ring->not_empty);
    clog_mutex_unlock(&ring->mutex);

    return 0;
}

int socket_ring_get_batch(socket_ring_buffer_t *ring,
                          const char          **lines,
                          size_t               *lengths,
                          size_t                max_lines)
{
    if (!ring || !lines || !lengths || max_lines == 0) {
        return -1;
    }

    clog_mutex_lock(&ring->mutex);

    /* Wait for data or close signal. */
    while (ring->count == 0 && !ring->closed) {
        clog_cond_wait(&ring->not_empty, &ring->mutex);
    }

    if (ring->count == 0 && ring->closed) {
        clog_mutex_unlock(&ring->mutex);
        return -1; /* Closed and drained. */
    }

    /* Dequeue up to max_lines. */
    size_t n = ring->count < max_lines ? ring->count : max_lines;
    for (size_t i = 0; i < n; i++) {
        lines[i]                     = ring->slots[ring->tail].line;
        lengths[i]                   = ring->slots[ring->tail].len;
        ring->slots[ring->tail].line = NULL;
        ring->slots[ring->tail].len  = 0;
        ring->tail                   = (ring->tail + 1) % ring->capacity;
    }
    ring->count -= n;

    clog_mutex_unlock(&ring->mutex);
    return (int)n;
}

void socket_ring_close(socket_ring_buffer_t *ring)
{
    if (!ring) {
        return;
    }

    clog_mutex_lock(&ring->mutex);
    ring->closed = 1;
    clog_cond_broadcast(&ring->not_empty);
    clog_mutex_unlock(&ring->mutex);
}

void socket_ring_signal(socket_ring_buffer_t *ring)
{
    if (!ring) {
        return;
    }

    clog_mutex_lock(&ring->mutex);
    clog_cond_broadcast(&ring->not_empty);
    clog_mutex_unlock(&ring->mutex);
}

void socket_ring_destroy(socket_ring_buffer_t *ring)
{
    if (!ring) {
        return;
    }

    /* Free any remaining lines. */
    for (size_t i = 0; i < ring->capacity; i++) {
        free(ring->slots[i].line);
    }
    free(ring->slots);

    clog_mutex_destroy(&ring->mutex);
    clog_cond_destroy(&ring->not_empty);
    free(ring);
}

/* ── Exponential Backoff Helpers ── */

static uint32_t backoff_next(uint32_t current_ms, uint32_t max_ms)
{
    /* Double the delay, cap at max, add ±10% jitter. */
    uint32_t next = current_ms * 2;
    if (next > max_ms) {
        next = max_ms;
    }
    /* Simple LCG jitter: ±10%. */
    uint32_t jitter = next / 10;
    uint32_t seed   = (uint32_t)clog_get_now_ms();
    uint32_t delta  = (seed % (2 * jitter + 1));
    if (delta > jitter) {
        next += (delta - jitter);
    } else {
        next -= (jitter - delta);
    }
    /* Clamp back to min. */
    if (next < 100) {
        next = 100;
    }
    return next;
}

static void backoff_sleep(uint32_t ms)
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

static int socket_set_nonblocking(clog_socket_t sockfd)
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

static int socket_connect_nonblocking(clog_socket_t sockfd, const char *host, int port)
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
        /* Wait for connection with a short timeout (1 second). */
        fd_set writefds;
        FD_ZERO(&writefds);
        FD_SET(sockfd, &writefds);

        struct timeval tv;
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        int sel = select((int)sockfd + 1, NULL, &writefds, NULL, &tv);
        if (sel > 0) {
            /* Check if connect succeeded. */
            int       err    = 0;
            socklen_t errlen = sizeof(err);
            getsockopt(sockfd, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen);
            return (err == 0) ? 0 : -1;
        }
        return -1; /* Timeout or error. */
    }

    return -1;
}

static int socket_send_nonblocking(clog_socket_t sockfd, const char *buf, size_t len)
{
    size_t total_sent = 0;

    while (total_sent < len) {
        long sent =
            send(sockfd, buf + total_sent, (clog_sock_size_t)(len - total_sent), MSG_NOSIGNAL);
        if (sent < 0) {
#if defined(_WIN32)
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINTR) {
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
#endif
                /* Would block — caller should retry after backoff. */
                return (int)total_sent > 0 ? (int)total_sent : -1;
            }
            /* Real error. */
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
static int tls_send_nonblocking(SSL *ssl, const char *buf, size_t len)
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

/* ── Writer Thread ── */

static void socket_writer_cleanup(socket_writer_t *writer)
{
#ifdef CLOG_USE_TLS
    if (writer->ssl) {
        SSL_shutdown(writer->ssl);
        SSL_free(writer->ssl);
        writer->ssl = NULL;
    }
    if (writer->ssl_ctx) {
        SSL_CTX_free(writer->ssl_ctx);
        writer->ssl_ctx = NULL;
    }
#endif
    if (!clog_is_invalid_socket(writer->sockfd)) {
        clog_close_socket(writer->sockfd);
        writer->sockfd = CLOG_INVALID_SOCKET;
    }
    writer->connected = 0;
}

static int socket_writer_connect(socket_writer_t *writer)
{
    if (writer->connected) {
        return 0;
    }

    clog_net_init();

    writer->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (clog_is_invalid_socket(writer->sockfd)) {
        return -1;
    }

    /* Set non-blocking mode. */
    if (socket_set_nonblocking(writer->sockfd) != 0) {
        clog_close_socket(writer->sockfd);
        writer->sockfd = CLOG_INVALID_SOCKET;
        return -1;
    }

    /* Non-blocking connect with 1-second timeout. */
    if (socket_connect_nonblocking(writer->sockfd, writer->config.host, writer->config.port) != 0) {
        clog_close_socket(writer->sockfd);
        writer->sockfd = CLOG_INVALID_SOCKET;
        return -1;
    }

    /* TLS handshake (if enabled). */
    if (writer->config.use_tls) {
#ifdef CLOG_USE_TLS
        const SSL_METHOD *method = TLS_method();
        writer->ssl_ctx          = SSL_CTX_new(method);
        if (!writer->ssl_ctx) {
            clog_close_socket(writer->sockfd);
            writer->sockfd = CLOG_INVALID_SOCKET;
            return -1;
        }

        if (writer->config.ca_file && strlen(writer->config.ca_file) > 0) {
            SSL_CTX_load_verify_locations(writer->ssl_ctx, writer->config.ca_file, NULL);
        }

        if (writer->config.skip_verify) {
            SSL_CTX_set_verify(writer->ssl_ctx, SSL_VERIFY_NONE, NULL);
        } else {
            SSL_CTX_set_verify(writer->ssl_ctx, SSL_VERIFY_PEER, NULL);
        }

        writer->ssl = SSL_new(writer->ssl_ctx);
        if (!writer->ssl) {
            SSL_CTX_free(writer->ssl_ctx);
            writer->ssl_ctx = NULL;
            clog_close_socket(writer->sockfd);
            writer->sockfd = CLOG_INVALID_SOCKET;
            return -1;
        }

        if (!writer->config.skip_verify && writer->config.host) {
            SSL_set1_host(writer->ssl, writer->config.host);
        }

        SSL_set_fd(writer->ssl, (int)writer->sockfd);
        if (SSL_connect(writer->ssl) <= 0) {
            SSL_free(writer->ssl);
            writer->ssl = NULL;
            SSL_CTX_free(writer->ssl_ctx);
            writer->ssl_ctx = NULL;
            clog_close_socket(writer->sockfd);
            writer->sockfd = CLOG_INVALID_SOCKET;
            return -1;
        }
#else
        fprintf(stderr, "[clogx] TLS requested but compiled without OpenSSL\n");
        clog_close_socket(writer->sockfd);
        writer->sockfd = CLOG_INVALID_SOCKET;
        return -1;
#endif
    }

    writer->connected  = 1;
    writer->backoff_ms = writer->config.backoff_min_ms;
    return 0;
}

static int socket_writer_send(socket_writer_t *writer, const char *line, size_t len)
{
    if (!writer->connected) {
        return -1;
    }

#ifdef CLOG_USE_TLS
    if (writer->config.use_tls && writer->ssl) {
        return tls_send_nonblocking(writer->ssl, line, len);
    }
#endif

    return socket_send_nonblocking(writer->sockfd, line, len);
}

static void *socket_writer_thread(void *arg)
{
    socket_writer_t      *writer = (socket_writer_t *)arg;
    socket_ring_buffer_t *ring   = writer->ring;

    const size_t BATCH_SIZE = 64;
    const char  *lines[64];
    size_t       lengths[64];

    for (;;) {
        /* Dequeue a batch (blocks on condvar if empty). */
        int n = socket_ring_get_batch(ring, lines, lengths, BATCH_SIZE);
        if (n < 0) {
            /* Ring closed and drained — exit. */
            break;
        }
        if (n == 0) {
            continue;
        }

        /* Try to connect if not already connected. */
        if (!writer->connected) {
            if (socket_writer_connect(writer) != 0) {
                /* Backoff: sleep before next reconnect attempt. */
                backoff_sleep(writer->backoff_ms);
                writer->backoff_ms =
                    backoff_next(writer->backoff_ms, writer->config.backoff_max_ms);
                /* Free the batch lines we didn't send. */
                for (int i = 0; i < n; i++) {
                    free((void *)lines[i]);
                }
                continue;
            }
        }

        /* Send the batch. */
        int send_failed = 0;
        for (int i = 0; i < n; i++) {
            /* Append newline for TCP framing. */
            char *framed = malloc(lengths[i] + 1);
            if (!framed) {
                free((void *)lines[i]);
                continue;
            }
            memcpy(framed, lines[i], lengths[i]);
            framed[lengths[i]] = '\n';

            int rc = socket_writer_send(writer, framed, lengths[i] + 1);
            free(framed);
            free((void *)lines[i]);

            if (rc < 0) {
                send_failed = 1;
                /* Drop remaining lines in this batch. */
                for (int j = i + 1; j < n; j++) {
                    free((void *)lines[j]);
                }
                break;
            }
        }

        if (send_failed) {
            /* Disconnect and backoff. */
            socket_writer_cleanup(writer);
            backoff_sleep(writer->backoff_ms);
            writer->backoff_ms = backoff_next(writer->backoff_ms, writer->config.backoff_max_ms);
        } else {
            /* Success — reset backoff. */
            writer->backoff_ms = writer->config.backoff_min_ms;
        }
    }

    /* Final drain: send any remaining lines before exit. */
    if (writer->connected) {
        for (;;) {
            int n = socket_ring_get_batch(ring, lines, lengths, BATCH_SIZE);
            if (n <= 0) {
                break;
            }
            for (int i = 0; i < n; i++) {
                char *framed = malloc(lengths[i] + 1);
                if (framed) {
                    memcpy(framed, lines[i], lengths[i]);
                    framed[lengths[i]] = '\n';
                    socket_writer_send(writer, framed, lengths[i] + 1);
                    free(framed);
                }
                free((void *)lines[i]);
            }
        }
        socket_writer_cleanup(writer);
    }

    return NULL;
}

/* ── Public API ── */

socket_writer_t *socket_writer_start(const socket_writer_config_t *config)
{
    if (!config || !config->host || config->port <= 0 || config->port > 65535) {
        return NULL;
    }

    socket_writer_t *writer = calloc(1, sizeof(socket_writer_t));
    if (!writer) {
        return NULL;
    }

    /* Copy config. */
    writer->config = *config;
    /* Ensure sane defaults. */
    if (writer->config.ring_capacity == 0) {
        writer->config.ring_capacity = 8192;
    }
    if (writer->config.backoff_min_ms == 0) {
        writer->config.backoff_min_ms = 1000;
    }
    if (writer->config.backoff_max_ms == 0) {
        writer->config.backoff_max_ms = 60000;
    }
    writer->backoff_ms = writer->config.backoff_min_ms;

    /* Create ring buffer. */
    writer->ring = socket_ring_create(writer->config.ring_capacity);
    if (!writer->ring) {
        free(writer);
        return NULL;
    }

    /* Start writer thread. */
    clog_thread_create(&writer->thread, socket_writer_thread, writer);

    return writer;
}

socket_ring_buffer_t *socket_writer_ring(socket_writer_t *writer)
{
    if (!writer) {
        return NULL;
    }
    return writer->ring;
}

void socket_writer_stop(socket_writer_t *writer)
{
    if (!writer) {
        return;
    }

    /* Signal ring to close and wake the writer. */
    socket_ring_close(writer->ring);
    socket_ring_signal(writer->ring);

    /* Wait for writer thread to finish. */
    clog_thread_join(writer->thread);
    socket_ring_destroy(writer->ring);
    writer->ring = NULL;
}

uint64_t socket_writer_dropped(const socket_writer_t *writer)
{
    if (!writer || !writer->ring) {
        return 0;
    }
    return writer->ring->dropped;
}
