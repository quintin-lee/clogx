/**
 * @file socket_sink.c
 * @brief TCP / TLS socket sink with lazy connect, reconnect-on-send-failure,
 *        and optional async non-blocking mode with ring buffer.
 *
 * ## Design
 *
 * Sends formatted log lines over a TCP socket to a remote receiver
 * (e.g. logstash, rsyslog, a custom collector). The connection is
 * established lazily on the first write and automatically reconnected
 * on send failure.
 *
 * ## Modes
 *
 * ### Synchronous (default)
 * `socket_write()` calls `send()` directly. If the send fails, the socket
 * is closed and reconnected once. Simple and predictable.
 *
 * ### Asynchronous (socket_async: true in config)
 * `socket_write()` enqueues the line into a ring buffer and returns
 * immediately. A background writer thread drains the buffer and sends
 * over a non-blocking socket with exponential backoff reconnection.
 * Lines are dropped (lossy) when the buffer is full — logging must
 * never block the application.
 *
 * ## Connection Lifecycle
 *
 * ```
 * socket_create()
 *   ├─ store host/port (no connect yet)
 *   └─ sink->write = socket_write
 *
 * socket_write() [sync]
 *   ├─ if sockfd == INVALID: socket_connect()
 *   ├─ send(buf, len)
 *   └─ on failure: close + reconnect + retry once
 *
 * socket_write() [async]
 *   ├─ ring_put(buf)  (non-blocking, lossy)
 *   └─ return immediately
 * ```
 *
 * ## TLS Support (compile-time)
 *
 * When built with `CLOG_USE_TLS` (OpenSSL), the socket wraps the TCP
 * connection in an SSL context. TLS is negotiated during the lazy
 * connect phase. Certificate verification can be skipped via
 * `skip_verify` for development environments.
 *
 * ## Reconnect Backoff (async mode)
 *
 * The background writer uses exponential backoff with jitter:
 *   - Initial delay: configurable (default 1000ms)
 *   - Multiplier: 2x on each failure
 *   - Max delay: configurable (default 60000ms)
 *   - Jitter: ±10% to prevent thundering herd
 *   - Reset: backoff resets to minimum on successful send
 *
 * ## Plugin Interface
 *
 * Implements the `clogx_plugin_v1` ABI.
 */
#include "clog_port.h"
#include "clogx_plugin.h"
#include "log_record.h"
#include "log_sink.h"
#include "socket_async.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CLOG_USE_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

typedef struct {
    clog_socket_t sockfd;
    const char   *host;
    int           port;
    int           connected;
    bool          use_tls;
    char         *ca_file;
    bool          skip_verify;
#ifdef CLOG_USE_TLS
    SSL_CTX *ssl_ctx;
    SSL     *ssl;
#endif
    /* Async mode fields. */
    bool                  async_enabled;
    socket_writer_t      *async_writer; /**< Background writer (NULL if sync). */
    socket_ring_buffer_t *async_ring;   /**< Ring buffer (NULL if sync). */
} socket_sink_data_t;

static int socket_connect(log_sink_t *sink)
{
    socket_sink_data_t *data = (socket_sink_data_t *)sink->private_data;
    if (data->connected) {
        return 0;
    }

    clog_net_init();

    data->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (clog_is_invalid_socket(data->sockfd)) {
        perror("Failed to create socket");
        return -1;
    }
#if !defined(_WIN32) && !defined(_WIN64)
    struct timeval tv;
    tv.tv_sec  = 2;
    tv.tv_usec = 0;
    setsockopt(data->sockfd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(data->sockfd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
#endif
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons((uint16_t)data->port);
    if (inet_pton(AF_INET, data->host, &serv_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid socket host: %s\n", data->host);
        clog_close_socket(data->sockfd);
        data->sockfd = CLOG_INVALID_SOCKET;
        return -1;
    }
    if (connect(data->sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Failed to connect to socket server");
        clog_close_socket(data->sockfd);
        data->sockfd    = CLOG_INVALID_SOCKET;
        data->connected = 0;
        return -1;
    }

    if (data->use_tls) {
#ifdef CLOG_USE_TLS
        const SSL_METHOD *method = TLS_method();
        data->ssl_ctx            = SSL_CTX_new(method);
        if (!data->ssl_ctx) {
            fprintf(stderr, "SSL_CTX_new failed\n");
            clog_close_socket(data->sockfd);
            data->sockfd = CLOG_INVALID_SOCKET;
            return -1;
        }

        if (data->ca_file && strlen(data->ca_file) > 0) {
            if (SSL_CTX_load_verify_locations(data->ssl_ctx, data->ca_file, NULL) != 1) {
                fprintf(stderr, "Failed to load CA file: %s\n", data->ca_file);
            }
        }

        if (data->skip_verify) {
            SSL_CTX_set_verify(data->ssl_ctx, SSL_VERIFY_NONE, NULL);
        } else {
            SSL_CTX_set_verify(data->ssl_ctx, SSL_VERIFY_PEER, NULL);
        }

        data->ssl = SSL_new(data->ssl_ctx);
        if (!data->ssl) {
            fprintf(stderr, "SSL_new failed\n");
            SSL_CTX_free(data->ssl_ctx);
            data->ssl_ctx = NULL;
            clog_close_socket(data->sockfd);
            data->sockfd = CLOG_INVALID_SOCKET;
            return -1;
        }

        if (!data->skip_verify && data->host && strlen(data->host) > 0) {
            SSL_set1_host(data->ssl, data->host);
        }

        SSL_set_fd(data->ssl, (int)data->sockfd);
        if (SSL_connect(data->ssl) <= 0) {
            fprintf(stderr, "SSL_connect failed\n");
            SSL_free(data->ssl);
            data->ssl = NULL;
            SSL_CTX_free(data->ssl_ctx);
            data->ssl_ctx = NULL;
            clog_close_socket(data->sockfd);
            data->sockfd = CLOG_INVALID_SOCKET;
            return -1;
        }
#else
        fprintf(stderr,
                "TLS support requested for socket sink, but clogx was compiled without OpenSSL "
                "(CLOG_USE_TLS)\n");
#endif
    }

    data->connected = 1;
    return 0;
}

static int socket_write(log_sink_t *sink, const char *buf, size_t len)
{
    socket_sink_data_t *data = (socket_sink_data_t *)sink->private_data;

    /* Async mode: enqueue into ring buffer and return immediately. */
    if (data->async_enabled && data->async_ring) {
        size_t send_len = len;
        if (send_len > 0 && buf[send_len - 1] == '\n') {
            send_len--;
        }
        return socket_ring_put(data->async_ring, buf, send_len);
    }

    /* Synchronous mode: blocking send. */
    if (clog_is_invalid_socket(data->sockfd)) {
        if (socket_connect(sink) != 0 || clog_is_invalid_socket(data->sockfd)) {
            return -1;
        }
    }

    size_t total_sent = 0;
    while (total_sent < len) {
#ifdef CLOG_USE_TLS
        if (data->use_tls && data->ssl) {
            int sent = SSL_write(data->ssl, buf + total_sent, (int)(len - total_sent));
            if (sent <= 0) {
                fprintf(stderr, "Failed to send SSL socket log\n");
                data->connected = 0;
                SSL_free(data->ssl);
                data->ssl = NULL;
                if (data->ssl_ctx) {
                    SSL_CTX_free(data->ssl_ctx);
                    data->ssl_ctx = NULL;
                }
                clog_close_socket(data->sockfd);
                data->sockfd = CLOG_INVALID_SOCKET;
                return -1;
            }
            total_sent += (size_t)sent;
            continue;
        }
#endif
        long sent = send(data->sockfd, buf + total_sent, (clog_sock_size_t)(len - total_sent), 0);
        if (sent < 0) {
            perror("Failed to send socket log");
            data->connected = 0;
            clog_close_socket(data->sockfd);
            data->sockfd = CLOG_INVALID_SOCKET;
            return -1;
        }
        total_sent += (size_t)sent;
    }

    return (int)total_sent;
}

static void socket_flush(log_sink_t *sink)
{
    (void)sink;
    /* In async mode, the writer thread handles flushing continuously.
     * In sync mode, TCP is byte-stream (no explicit flush needed). */
}

static void socket_destroy(log_sink_t *sink)
{
    socket_sink_data_t *data = (socket_sink_data_t *)sink->private_data;
    if (data) {
        /* Stop async writer if running. */
        if (data->async_enabled && data->async_writer) {
            socket_writer_stop(data->async_writer);
            free(data->async_writer);
            data->async_writer = NULL;
            data->async_ring   = NULL;
        }

#ifdef CLOG_USE_TLS
        if (data->ssl) {
            SSL_shutdown(data->ssl);
            SSL_free(data->ssl);
        }
        if (data->ssl_ctx) {
            SSL_CTX_free(data->ssl_ctx);
        }
#endif
        if (!clog_is_invalid_socket(data->sockfd)) {
            clog_close_socket(data->sockfd);
        }
        free((char *)data->host);
        free(data->ca_file);
        free(data);
    }
    free(sink);
}

static void socket_atfork_child(log_sink_t *sink)
{
    if (!sink || !sink->private_data) {
        return;
    }
    socket_sink_data_t *data = (socket_sink_data_t *)sink->private_data;

    /* In async mode, the writer thread doesn't survive fork.
     * Stop it and mark as disconnected so a new thread can be started. */
    if (data->async_enabled && data->async_writer) {
        socket_writer_stop(data->async_writer);
        free(data->async_writer);
        data->async_writer = NULL;
        data->async_ring   = NULL;
    }

#ifdef CLOG_USE_TLS
    if (data->ssl) {
        SSL_set_shutdown(data->ssl, SSL_SENT_SHUTDOWN);
        SSL_shutdown(data->ssl);
        SSL_free(data->ssl);
        data->ssl = NULL;
    }
    if (data->ssl_ctx) {
        SSL_CTX_free(data->ssl_ctx);
        data->ssl_ctx = NULL;
    }
#endif
    if (!clog_is_invalid_socket(data->sockfd)) {
#if !defined(_WIN32) && !defined(_WIN64)
        shutdown(data->sockfd, SHUT_WR);
#endif
        clog_close_socket(data->sockfd);
        data->sockfd = CLOG_INVALID_SOCKET;
    }
    data->connected = 0;
}

/**
 * @brief Create a TCP/TLS socket sink with lazy connect.
 *
 * The connection is not established until the first write. On send
 * failure, the socket is closed and reconnected automatically.
 *
 * @param host         Remote host. Non-NULL, non-empty, port 1–65535.
 * @param port         Remote port.
 * @param use_tls      Enable OpenSSL TLS transport (requires CLOG_USE_TLS).
 * @param ca_file      CA certificate path for TLS verification, or NULL.
 * @param skip_verify  Skip server certificate verification when true.
 * @return New sink, or NULL on error.
 */
log_sink_t *socket_sink_create_tls(
    const char *host, int port, bool use_tls, const char *ca_file, bool skip_verify)
{
    if (!host || strlen(host) == 0 || port <= 0 || port > 65535) {
        return NULL;
    }
    /* LCOV_EXCL_START - System allocation failure */
    log_sink_t *sink = malloc(sizeof(log_sink_t));
    if (!sink) {
        return NULL;
    }
    socket_sink_data_t *data = malloc(sizeof(socket_sink_data_t));
    if (!data) {
        free(sink);
        return NULL;
    }
    data->host = strdup(host);
    if (!data->host) {
        free(data);
        free(sink);
        return NULL;
    }
    /* LCOV_EXCL_STOP */
    data->port          = port;
    data->sockfd        = CLOG_INVALID_SOCKET;
    data->connected     = 0;
    data->use_tls       = use_tls;
    data->ca_file       = ca_file ? strdup(ca_file) : NULL;
    data->skip_verify   = skip_verify;
    data->async_enabled = false;
    data->async_writer  = NULL;
    data->async_ring    = NULL;
#ifdef CLOG_USE_TLS
    data->ssl_ctx = NULL;
    data->ssl     = NULL;
#endif

    sink->abi_version  = CLOGX_PLUGIN_ABI_VERSION;
    sink->write        = socket_write;
    sink->flush        = socket_flush;
    sink->destroy      = socket_destroy;
    sink->atfork_child = socket_atfork_child;
    sink->private_data = data;
    sink->min_level    = LOG_LEVEL_TRACE;
    return sink;
}

/**
 * @brief Create an async TCP/TLS socket sink with ring buffer and backoff.
 *
 * The writer thread connects lazily and drains the ring buffer with
 * non-blocking sends. Reconnection uses exponential backoff with jitter.
 *
 * @param host            Remote host. Non-NULL, non-empty, port 1–65535.
 * @param port            Remote port.
 * @param use_tls         Enable OpenSSL TLS transport (requires CLOG_USE_TLS).
 * @param ca_file         CA certificate path for TLS verification, or NULL.
 * @param skip_verify     Skip server certificate verification when true.
 * @param ring_capacity   Ring buffer capacity (number of lines). 0 = 8192 default.
 * @param backoff_min_ms  Initial backoff delay in ms. 0 = 1000 default.
 * @param backoff_max_ms  Maximum backoff delay in ms. 0 = 60000 default.
 * @return New sink, or NULL on error.
 */
log_sink_t *socket_sink_create_async(const char *host,
                                     int         port,
                                     bool        use_tls,
                                     const char *ca_file,
                                     bool        skip_verify,
                                     size_t      ring_capacity,
                                     uint32_t    backoff_min_ms,
                                     uint32_t    backoff_max_ms)
{
    log_sink_t *sink = socket_sink_create_tls(host, port, use_tls, ca_file, skip_verify);
    if (!sink) {
        return NULL;
    }

    socket_sink_data_t *data = (socket_sink_data_t *)sink->private_data;

    socket_writer_config_t wconfig;
    memset(&wconfig, 0, sizeof(wconfig));
    wconfig.host           = data->host;
    wconfig.port           = data->port;
    wconfig.use_tls        = data->use_tls;
    wconfig.ca_file        = data->ca_file;
    wconfig.skip_verify    = data->skip_verify;
    wconfig.ring_capacity  = ring_capacity > 0 ? ring_capacity : 8192;
    wconfig.backoff_min_ms = backoff_min_ms > 0 ? backoff_min_ms : 1000;
    wconfig.backoff_max_ms = backoff_max_ms > 0 ? backoff_max_ms : 60000;

    socket_writer_t *writer = socket_writer_start(&wconfig);
    if (!writer) {
        /* Fall back to sync mode. */
        return sink;
    }

    data->async_enabled = true;
    data->async_writer  = writer;
    data->async_ring    = socket_writer_ring(writer);

    return sink;
}

/**
 * @brief Create a plain TCP socket sink (no TLS).
 *
 * Convenience wrapper around socket_sink_create_tls() with TLS disabled.
 *
 * @param host  Remote host. Non-NULL, non-empty, port 1–65535.
 * @param port  Remote port.
 * @return New sink, or NULL on error.
 */
log_sink_t *socket_sink_create(const char *host, int port)
{
    return socket_sink_create_tls(host, port, false, NULL, false);
}
