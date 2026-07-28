/**
 * @file socket_sink.c
 * @brief TCP / TLS socket sink with lazy connect and reconnect-on-send-failure.
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "log_sink.h"
#include "log_record.h"

#ifdef CLOG_USE_TLS
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

typedef struct {
    int sockfd;
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
    data->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (data->sockfd < 0) {
        perror("Failed to create socket");
        return -1;
    }
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons((in_port_t)data->port);
    if (inet_pton(AF_INET, data->host, &serv_addr.sin_addr) <= 0) {
        fprintf(stderr, "Invalid socket host: %s\n", data->host);
        close(data->sockfd);
        data->sockfd = -1;
        return -1;
    }
    if (connect(data->sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Failed to connect to socket server");
        close(data->sockfd);
        data->sockfd = -1;
        data->connected = 0;
        return -1;
    }

    if (data->use_tls) {
#ifdef CLOG_USE_TLS
        const SSL_METHOD *method = TLS_client_method();
        data->ssl_ctx = SSL_CTX_new(method);
        if (!data->ssl_ctx) {
            fprintf(stderr, "SSL_CTX_new failed\n");
            close(data->sockfd);
            data->sockfd = -1;
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
            close(data->sockfd);
            data->sockfd = -1;
            return -1;
        }

        SSL_set_fd(data->ssl, data->sockfd);
        if (SSL_connect(data->ssl) <= 0) {
            fprintf(stderr, "SSL_connect failed\n");
            SSL_free(data->ssl);
            data->ssl = NULL;
            SSL_CTX_free(data->ssl_ctx);
            data->ssl_ctx = NULL;
            close(data->sockfd);
            data->sockfd = -1;
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
    if (data->sockfd < 0) {
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
                close(data->sockfd);
                data->sockfd = -1;
                return -1;
            }
            total_sent += (size_t)sent;
            continue;
        }
#endif
        ssize_t sent = send(data->sockfd, buf + total_sent, len - total_sent, 0);
        if (sent < 0) {
            perror("Failed to send socket log");
            data->connected = 0;
            close(data->sockfd);
            data->sockfd = -1;
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
        if (data->sockfd >= 0) {
            close(data->sockfd);
        }
        free((char *)data->host);
        free(data->ca_file);
        free(data);
    }
    free(sink);
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
    data->sockfd = -1;
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
    sink->private_data = data;
    sink->min_level = LOG_LEVEL_TRACE;
    return sink;
}

log_sink_t *socket_sink_create(const char *host, int port) {
    return socket_sink_create_tls(host, port, false, NULL, false);
}
