#include "clog_port.h"
#include "socket_async.h"
#include "socket_async_internal.h"
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
    int          heap_flags[64];
    char         framed_stack[512];

    for (;;) {
        /* Dequeue a batch (blocks on semaphore if empty). */
        int n = socket_ring_get_batch(ring, lines, lengths, heap_flags, BATCH_SIZE);
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
                    if (heap_flags[i]) {
                        free((void *)lines[i]);
                    }
                }
                continue;
            }
        }

        /* Send the batch. */
        int send_failed = 0;
        for (int i = 0; i < n; i++) {
            /* Skip empty slots (published on producer-side alloc failure). */
            if (!lines[i] || lengths[i] == 0) {
                if (heap_flags[i]) {
                    free((void *)lines[i]);
                }
                continue;
            }
            int rc = 0;
            if (lines[i][lengths[i] - 1] == '\n') {
                rc = socket_writer_send(writer, lines[i], lengths[i]);
            } else {
                size_t framed_len = lengths[i] + 1;
                char  *framed =
                    (framed_len <= sizeof(framed_stack)) ? framed_stack : malloc(framed_len);
                if (framed) {
                    memcpy(framed, lines[i], lengths[i]);
                    framed[lengths[i]] = '\n';
                    rc                 = socket_writer_send(writer, framed, framed_len);
                    if (framed != framed_stack) {
                        free(framed);
                    }
                }
            }
            if (heap_flags[i]) {
                free((void *)lines[i]);
            }

            if (rc < 0) {
                send_failed = 1;
                /* Drop remaining lines in this batch. */
                for (int j = i + 1; j < n; j++) {
                    if (heap_flags[j]) {
                        free((void *)lines[j]);
                    }
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
            int n = socket_ring_get_batch(ring, lines, lengths, heap_flags, BATCH_SIZE);
            if (n <= 0) {
                break;
            }
            for (int i = 0; i < n; i++) {
                if (!lines[i] || lengths[i] == 0) {
                    if (heap_flags[i]) {
                        free((void *)lines[i]);
                    }
                    continue;
                }
                if (lines[i][lengths[i] - 1] == '\n') {
                    socket_writer_send(writer, lines[i], lengths[i]);
                } else {
                    size_t framed_len = lengths[i] + 1;
                    char  *framed =
                        (framed_len <= sizeof(framed_stack)) ? framed_stack : malloc(framed_len);
                    if (framed) {
                        memcpy(framed, lines[i], lengths[i]);
                        framed[lengths[i]] = '\n';
                        socket_writer_send(writer, framed, framed_len);
                        if (framed != framed_stack) {
                            free(framed);
                        }
                    }
                }
                if (heap_flags[i]) {
                    free((void *)lines[i]);
                }
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
