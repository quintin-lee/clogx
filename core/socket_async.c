/**
 * @file socket_async.c
 * @brief Lock-free async non-blocking socket writer with ring buffer and
 * exponential backoff.
 *
 * Implements the ring buffer for formatted log lines and a background writer
 * thread that drains the buffer over a TCP/TLS socket with non-blocking I/O.
 *
 * ## Lock-free Design
 *
 * The ring buffer uses atomic compare-exchange on `head` for producer slot
 * claiming — multiple producer threads may call @ref socket_ring_put concurrently
 * without any mutex. When the buffer is full, the new line is dropped (lossy)
 * and the `dropped` counter is incremented. The single consumer (writer thread)
 * blocks on a semaphore (`items_sem`) and reads from `tail`.
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

static size_t sk_next_pow2(size_t n)
{
    if (n < 2) {
        return 2;
    }
    size_t p = 1;
    while (p < n) {
        p <<= 1;
    }
    return p;
}

socket_ring_buffer_t *socket_ring_create(size_t capacity)
{
    if (capacity == 0) {
        return NULL;
    }

    socket_ring_buffer_t *ring = calloc(1, sizeof(socket_ring_buffer_t));
    if (!ring) {
        return NULL;
    }

    size_t cap = sk_next_pow2(capacity);

    ring->slots = calloc(cap, sizeof(socket_ring_slot_t));
    if (!ring->slots) {
        free(ring);
        return NULL;
    }

    ring->capacity = cap;
    ring->mask     = cap - 1;

    ring->head    = 0;
    ring->tail    = 0;
    ring->count   = 0;
    ring->dropped = 0;
    ring->closed  = 0;

    /* Slot i expects `seq == i + 1` for the first line at absolute position i. */
    for (size_t i = 0; i < cap; i++) {
        ring->slots[i].seq = (uint64_t)i;
    }

    if (clog_sem_init(&ring->items_sem, 0) != 0) {
        free(ring->slots);
        free(ring);
        return NULL;
    }

    return ring;
}

int socket_ring_put(socket_ring_buffer_t *ring, const char *line, size_t len)
{
    if (!ring || !line || len == 0) {
        return -1;
    }

    /* Fast check: closed ring rejects puts. */
    if (clog_atomic_load_int(&ring->closed)) {
        return -1;
    }

    /*
     * Lock-free producer path: use a CAS loop to atomically claim a slot in
     * `head`. This is the hot path — no mutex is ever acquired.
     */
    for (;;) {
        size_t head = clog_atomic_load_sz(&ring->head);
        size_t tail = clog_atomic_load_sz(&ring->tail);

        if (head - tail >= ring->capacity) {
            /*
             * Ring is full — drop the new entry (lossy backpressure).
             * This is intentional for a logging ring buffer: blocking the
             * caller would be worse than dropping a message.
             */
            clog_atomic_inc64(&ring->dropped);
            return 0;
        }

        if (clog_atomic_cas_sz(&ring->head, &head, head + 1)) {
            /*
             * Successfully claimed slot `head`. Allocate a copy of the line
             * and write it, then publish with a release-store of seq (making
             * the write visible to the consumer) before signalling.
             */
            socket_ring_slot_t *slot = &ring->slots[head & ring->mask];
            char               *copy = malloc(len + 1);
            if (!copy) {
                /*
                 * Allocation failed. The slot is already claimed — rolling
                 * head back would corrupt other producers' claims, so publish
                 * an empty slot instead and count it as dropped. The consumer
                 * frees NULL safely and skips zero-length lines.
                 */
                slot->line = NULL;
                slot->len  = 0;
                clog_atomic_store_u64(&slot->seq, (uint64_t)head + 1);
                clog_atomic_fetch_add_sz(&ring->count, 1);
                clog_sem_post(&ring->items_sem);
                clog_atomic_inc64(&ring->dropped);
                return -1;
            }
            memcpy(copy, line, len);
            copy[len] = '\0';

            slot->line = copy;
            slot->len  = len;

            clog_atomic_store_u64(&slot->seq, (uint64_t)head + 1);
            clog_atomic_fetch_add_sz(&ring->count, 1);
            clog_sem_post(&ring->items_sem);
            return 0;
        }
        /* CAS failed — another producer claimed the slot; retry. */
    }
}

int socket_ring_get_batch(socket_ring_buffer_t *ring,
                          const char          **lines,
                          size_t               *lengths,
                          size_t                max_lines)
{
    if (!ring || !lines || !lengths || max_lines == 0) {
        return -1;
    }

    /*
     * Block until at least one item is available or the ring is closed.
     * The semaphore is posted by producers after each put, and by
     * socket_ring_close() / socket_ring_signal() to wake blocked consumers.
     *
     * A closed ring must never block: the closed flag is checked before
     * waiting (fast path for callers that come back after close) and again
     * after waking, so a close racing with the wait still terminates through
     * the wake-up post. Without the pre-wait check, a second get_batch on a
     * closed empty ring would consume the close's post and then block
     * forever (semaphore count already zero).
     */
    for (;;) {
        /*
         * Closed AND empty → terminate immediately. This keeps repeated
         * calls after close from blocking on the consumed wake-up post
         * (lost-wakeup deadlock). Closed-but-nonempty still returns the
         * remaining items below (the semaphore count can never be zero while
         * items exist), preserving drain-on-close semantics.
         */
        if (clog_atomic_load_int(&ring->closed) &&
            clog_atomic_load_sz(&ring->head) == clog_atomic_load_sz(&ring->tail)) {
            return -1;
        }
        clog_sem_wait(&ring->items_sem);

        /* Load the latest head (producer writes) and local tail (consumer reads). */
        size_t head = clog_atomic_load_sz(&ring->head);
        size_t tail = clog_atomic_load_sz(&ring->tail);

        size_t available = head - tail;

        if (available == 0) {
            /*
             * Woken by close/signal with no actual items to read.
             * If closed, signal shutdown; otherwise keep waiting.
             */
            if (clog_atomic_load_int(&ring->closed)) {
                return -1;
            }
            continue;
        }

        /*
         * Only read slots whose producer has published them (seq == pos + 1).
         * A producer that won the head CAS but was preempted before writing has
         * committed — it will finish and post items_sem, so a short yield and a
         * retry from the caller is safe.
         */
        size_t n = 0;
        while (n < available && n < max_lines) {
            size_t              pos  = tail + n;
            socket_ring_slot_t *slot = &ring->slots[pos & ring->mask];
            if (clog_atomic_load_u64(&slot->seq) != (uint64_t)pos + 1) {
                break;
            }
            lines[n]   = slot->line;
            lengths[n] = slot->len;
            slot->line = NULL;
            slot->len  = 0;
            n++;
        }

        if (n == 0) {
            /* Claimed but unpublished slots — yield once and report empty; the
             * committed producer's post will wake the next get_batch. */
            clog_sleep_ms(0);
            continue;
        }

        /* Advance the consumer read position. */
        clog_atomic_store_sz(&ring->tail, tail + n);
        clog_atomic_fetch_sub_sz(&ring->count, n);

        return (int)n;
    }
}

void socket_ring_close(socket_ring_buffer_t *ring)
{
    if (!ring) {
        return;
    }

    clog_atomic_store_int(&ring->closed, 1);

    /*
     * Post to the semaphore so any blocked consumer wakes up and can
     * observe the closed state.
     */
    clog_sem_post(&ring->items_sem);
}

void socket_ring_signal(socket_ring_buffer_t *ring)
{
    if (!ring) {
        return;
    }

    /*
     * Wake the consumer thread. The consumer will check the actual head/tail
     * after waking.
     */
    clog_sem_post(&ring->items_sem);
}

void socket_ring_destroy(socket_ring_buffer_t *ring)
{
    if (!ring) {
        return;
    }

    /* Wake any blocked consumer so it can observe closed state. */
    clog_atomic_store_int(&ring->closed, 1);
    clog_sem_post(&ring->items_sem);

    /* Free any remaining lines. */
    size_t head      = clog_atomic_load_sz(&ring->head);
    size_t tail      = clog_atomic_load_sz(&ring->tail);
    size_t remaining = head - tail;
    for (size_t i = 0; i < remaining && i < ring->capacity; i++) {
        size_t idx = (tail + i) & ring->mask;
        free(ring->slots[idx].line);
        ring->slots[idx].line = NULL;
        ring->slots[idx].len  = 0;
    }

    clog_sem_destroy(&ring->items_sem);
    free(ring->slots);
    free(ring);
}

size_t socket_ring_depth(const socket_ring_buffer_t *ring)
{
    if (!ring) {
        return 0;
    }
    return clog_atomic_load_sz(&ring->head) - clog_atomic_load_sz(&ring->tail);
}

/* ── Exponential Backoff Helpers ── */

static uint32_t backoff_next(uint32_t current_ms, uint32_t max_ms)
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

static int
socket_connect_nonblocking(clog_socket_t sockfd, const char *host, int port, uint32_t timeout_ms)
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

static int socket_send_nonblocking(clog_socket_t sockfd, const char *buf, size_t len)
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
        SSL_set_shutdown(writer->ssl, SSL_SENT_SHUTDOWN);
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
#if !defined(_WIN32) && !defined(_WIN64)
        shutdown(writer->sockfd, SHUT_WR);
#endif
        clog_close_socket(writer->sockfd);
        writer->sockfd = CLOG_INVALID_SOCKET;
    }
    clog_net_cleanup();
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
        clog_net_cleanup();
        return -1;
    }

    uint32_t timeout_ms =
        writer->config.connect_timeout_ms > 0 ? writer->config.connect_timeout_ms : 1000;

    if (socket_set_nonblocking(writer->sockfd) != 0 ||
        socket_connect_nonblocking(
            writer->sockfd, writer->config.host, writer->config.port, timeout_ms) != 0) {
        socket_writer_cleanup(writer);
        return -1;
    }

    if (writer->config.use_tls) {
#ifdef CLOG_USE_TLS
        const SSL_METHOD *method = TLS_method();
        writer->ssl_ctx          = SSL_CTX_new(method);
        if (!writer->ssl_ctx) {
            socket_writer_cleanup(writer);
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
            socket_writer_cleanup(writer);
            return -1;
        }

        if (!writer->config.skip_verify && writer->config.host) {
            SSL_set1_host(writer->ssl, writer->config.host);
        }

        SSL_set_fd(writer->ssl, (int)writer->sockfd);
        int ssl_res = 0;
        while ((ssl_res = SSL_connect(writer->ssl)) <= 0) {
            int err = SSL_get_error(writer->ssl, ssl_res);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                fd_set fds;
                FD_ZERO(&fds);
                FD_SET(writer->sockfd, &fds);
                struct timeval tv;
                tv.tv_sec  = (long)(timeout_ms / 1000);
                tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
                if (err == SSL_ERROR_WANT_READ) {
                    select((int)writer->sockfd + 1, &fds, NULL, NULL, &tv);
                } else {
                    select((int)writer->sockfd + 1, NULL, &fds, NULL, &tv);
                }
                continue;
            }
            break;
        }
        if (ssl_res <= 0) {
            socket_writer_cleanup(writer);
            return -1;
        }
#else
        fprintf(stderr, "[clogx] TLS requested but compiled without OpenSSL\n");
        socket_writer_cleanup(writer);
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
        /* Dequeue a batch (blocks on semaphore if empty). */
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
            /* Skip empty slots (published on producer-side alloc failure). */
            if (!lines[i] || lengths[i] == 0) {
                free((void *)lines[i]);
                continue;
            }
            int rc = 0;
            if (lines[i][lengths[i] - 1] == '\n') {
                rc = socket_writer_send(writer, lines[i], lengths[i]);
            } else {
                char *framed = malloc(lengths[i] + 1);
                if (framed) {
                    memcpy(framed, lines[i], lengths[i]);
                    framed[lengths[i]] = '\n';
                    rc                 = socket_writer_send(writer, framed, lengths[i] + 1);
                    free(framed);
                }
            }
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
    if (writer->connected || socket_writer_connect(writer) == 0) {
        for (;;) {
            int n = socket_ring_get_batch(ring, lines, lengths, BATCH_SIZE);
            if (n <= 0) {
                break;
            }
            for (int i = 0; i < n; i++) {
                if (!lines[i] || lengths[i] == 0) {
                    free((void *)lines[i]);
                    continue;
                }
                if (lines[i][lengths[i] - 1] == '\n') {
                    socket_writer_send(writer, lines[i], lengths[i]);
                } else {
                    char *framed = malloc(lengths[i] + 1);
                    if (framed) {
                        memcpy(framed, lines[i], lengths[i]);
                        framed[lengths[i]] = '\n';
                        socket_writer_send(writer, framed, lengths[i] + 1);
                        free(framed);
                    }
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
    return clog_atomic_get64(&writer->ring->dropped);
}
