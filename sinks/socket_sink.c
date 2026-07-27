/**
 * @file socket_sink.c
 * @brief TCP socket sink with lazy connect and reconnect-on-send-failure.
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

typedef struct {
    int sockfd;
    const char *host;
    int port;
    int connected;
} socket_sink_data_t;

static int socket_connect(log_sink_t *sink) {
    socket_sink_data_t *data = (socket_sink_data_t *)sink->private_data;
    if (data->connected) return 0;
    data->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (data->sockfd < 0) {
        perror("Failed to create socket");
        return -1;
    }
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(data->port);
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
    data->connected = 1;
    return 0;
}

static int socket_write(log_sink_t *sink, const char *buf, size_t len) {
    socket_sink_data_t *data = (socket_sink_data_t *)sink->private_data;
    if (data->sockfd < 0) {
        if (socket_connect(sink) != 0) return -1;
    }

    size_t total_sent = 0;
    while (total_sent < len) {
        int sent = send(data->sockfd, buf + total_sent, len - total_sent, 0);
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
        if (data->sockfd >= 0) {
            close(data->sockfd);
        }
        free((char *)data->host);
        free(data);
    }
    free(sink);
}

log_sink_t *socket_sink_create(const char *host, int port) {
    if (!host || strlen(host) == 0 || port <= 0 || port > 65535) return NULL;
    log_sink_t *sink = malloc(sizeof(log_sink_t));
    if (!sink) return NULL;
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
    sink->write = socket_write;
    sink->flush = socket_flush;
    sink->destroy = socket_destroy;
    sink->private_data = data;
    return sink;
}