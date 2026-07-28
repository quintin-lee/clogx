/**
 * @file socket_sink.c
 * @brief TCP / TLS socket sink with lazy connect and reconnect-on-send-failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clog_port.h"
#include "log_sink.h"
#include "log_record.h"

#ifdef CLOG_USE_TLS
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

typedef struct {
    clog_socket_t sockfd;
    const char *host;
    int port;
    int connected;
    bool use_tls;
    char *ca_file;
    bool skip_verify;
#ifdef CLOG_USE_TLS
    SSL_CTX *ssl_ctx;
    SSL *ssl;
#endif
} socket_sink_data_t;

static int socket_connect(log_sink_t *sink) {
    socket_sink_data_t *data = (socket_sink_data_t *)sink->private_data;
    if (data->connected)
        return 0;

    clog_net_init();

    data->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (clog_is_invalid_socket(data->sockfd)) {
        perror("Failed to create socket");
        return -1;
    }
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons((uint16_t)data->port);
    if (inet_pton(AF_INET, data->host, &serv_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid socket host: %s\n", data->host);
        clog_close_socket(data->sockfd);
        data->sockfd = CLOG_INVALID_SOCKET;
        return -1;
    }
    if (connect(data->sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Failed to connect to socket server");
        clog_close_socket(data->sockfd);
        data->sockfd = CLOG_INVALID_SOCKET;
        data->connected = 0;
        return -1;
    }

    if (data->use_tls) {
#ifdef CLOG_USE_TLS
        const SSL_METHOD *method = TLS_method();
        data->ssl_ctx = SSL_CTX_new(method);
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

static int socket_write(log_sink_t *sink, const char *buf, size_t len) {
    socket_sink_data_t *data = (socket_sink_data_t *)sink->private_data;
    if (clog_is_invalid_socket(data->sockfd)) {
        if (socket_connect(sink) != 0)
            return -1;
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
        long sent = send(data->sockfd, buf + total_sent, (int)(len - total_sent), 0);
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

static void socket_flush(log_sink_t *sink) {
    (void)sink;
}

static void socket_destroy(log_sink_t *sink) {
    socket_sink_data_t *data = (socket_sink_data_t *)sink->private_data;
    if (data) {
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

static void socket_atfork_child(log_sink_t *sink) {
    if (!sink || !sink->private_data)
        return;
    socket_sink_data_t *data = (socket_sink_data_t *)sink->private_data;
#ifdef CLOG_USE_TLS
    if (data->ssl) {
        SSL_free(data->ssl);
        data->ssl = NULL;
    }
    if (data->ssl_ctx) {
        SSL_CTX_free(data->ssl_ctx);
        data->ssl_ctx = NULL;
    }
#endif
    if (!clog_is_invalid_socket(data->sockfd)) {
        clog_close_socket(data->sockfd);
        data->sockfd = CLOG_INVALID_SOCKET;
    }
    data->connected = 0;
}

log_sink_t *socket_sink_create_tls(const char *host, int port, bool use_tls, const char *ca_file,
                                   bool skip_verify) {
    if (!host || strlen(host) == 0 || port <= 0 || port > 65535)
        return NULL;
    log_sink_t *sink = malloc(sizeof(log_sink_t));
    if (!sink)
        return NULL;
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
    data->port = port;
    data->sockfd = CLOG_INVALID_SOCKET;
    data->connected = 0;
    data->use_tls = use_tls;
    data->ca_file = ca_file ? strdup(ca_file) : NULL;
    data->skip_verify = skip_verify;
#ifdef CLOG_USE_TLS
    data->ssl_ctx = NULL;
    data->ssl = NULL;
#endif

    sink->write = socket_write;
    sink->flush = socket_flush;
    sink->destroy = socket_destroy;
    sink->atfork_child = socket_atfork_child;
    sink->private_data = data;
    sink->min_level = LOG_LEVEL_TRACE;
    return sink;
}

log_sink_t *socket_sink_create(const char *host, int port) {
    return socket_sink_create_tls(host, port, false, NULL, false);
}
