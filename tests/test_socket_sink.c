/**
 * @file test_socket_sink.c
 * @brief Regression test: TCP socket sink connection, write, and TLS options.
 */

#include "clog_port.h"
#include "log.h"
#include "log_sink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_MSGS 50
#define BUF_SIZE 16384

typedef struct {
    int          port;
    int          received_lines;
    int          ready;
    clog_mutex_t mutex;
    clog_cond_t  cond;
} server_ctx_t;

static void *listener_thread(void *arg)
{
    server_ctx_t *ctx = (server_ctx_t *)arg;

    clog_net_init();

    clog_socket_t server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (clog_is_invalid_socket(server_fd)) {
        perror("server socket");
        return NULL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("server bind");
        clog_close_socket(server_fd);
        return NULL;
    }

#if defined(_WIN32) || defined(_WIN64)
    int len = sizeof(addr);
#else
    socklen_t len = sizeof(addr);
#endif
    if (getsockname(server_fd, (struct sockaddr *)&addr, &len) < 0) {
        perror("getsockname");
        clog_close_socket(server_fd);
        return NULL;
    }
    ctx->port = ntohs(addr.sin_port);

    if (listen(server_fd, 1) < 0) {
        perror("server listen");
        clog_close_socket(server_fd);
        return NULL;
    }

    clog_mutex_lock(&ctx->mutex);
    ctx->ready = 1;
    clog_cond_signal(&ctx->cond);
    clog_mutex_unlock(&ctx->mutex);

    clog_socket_t client_fd = accept(server_fd, NULL, NULL);
    if (clog_is_invalid_socket(client_fd)) {
        perror("server accept");
        clog_close_socket(server_fd);
        return NULL;
    }

    char buf[BUF_SIZE];
    int  total = 0;
    int  n;
    while ((n = (int)recv(
                client_fd, buf + total, (clog_sock_size_t)(sizeof(buf) - 1 - (size_t)total), 0)) >
           0) {
        total += n;
        buf[total] = '\0';
    }

    clog_close_socket(client_fd);
    clog_close_socket(server_fd);

    int lines = 0;
    for (int i = 0; i < total; i++) {
        if (buf[i] == '\n') {
            lines++;
        }
    }
    ctx->received_lines = lines;

    return NULL;
}

int main(void)
{
    server_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    clog_mutex_init(&ctx.mutex);
    clog_cond_init(&ctx.cond);

    clog_thread_t server_thread;
    if (clog_thread_create(&server_thread, listener_thread, &ctx) != 0) {
        fprintf(stderr, "clog_thread_create failed\n");
        return 1;
    }

    clog_mutex_lock(&ctx.mutex);
    while (!ctx.ready) {
        clog_cond_wait(&ctx.cond, &ctx.mutex);
    }
    clog_mutex_unlock(&ctx.mutex);

    /* Logger with socket sink only — no console, no file */
    log_sink_t *sink = socket_sink_create("127.0.0.1", ctx.port);
    if (!sink) {
        fprintf(stderr, "socket_sink_create failed\n");
        return 1;
    }

    if (log_init(NULL) != 0) {
        fprintf(stderr, "log_init (null config) failed\n");
        return 1;
    }
    log_set_level(LOG_LEVEL_INFO);

    if (log_add_sink(sink) != 0) {
        fprintf(stderr, "log_add_sink failed\n");
        return 1;
    }

    for (int i = 0; i < NUM_MSGS; i++) {
        LOG_INFO("socket-test-msg-%d", i);
    }

    log_flush();
    log_destroy();

    clog_thread_join(server_thread);

    printf("socket sink test: %d/%d lines\n", ctx.received_lines, NUM_MSGS);
    if (ctx.received_lines != NUM_MSGS) {
        fprintf(stderr, "expected %d lines, got %d\n", NUM_MSGS, ctx.received_lines);
        return 1;
    }

    clog_mutex_destroy(&ctx.mutex);
    clog_cond_destroy(&ctx.cond);

    /* Verify socket_sink_create_tls factory function */
    log_sink_t *tls_sink = socket_sink_create_tls("127.0.0.1", 9000, true, NULL, true);
    if (!tls_sink) {
        fprintf(stderr, "socket_sink_create_tls failed\n");
        return 1;
    }
    tls_sink->destroy(tls_sink);

    if (socket_sink_create_tls(NULL, 9000, true, NULL, false) != NULL) {
        fprintf(stderr, "socket_sink_create_tls with NULL host should fail\n");
        return 1;
    }

    /* Test socket_sink_create_tls with ca_file */
    log_sink_t *ca_sink = socket_sink_create_tls("127.0.0.1", 9000, false, "dummy_ca.pem", false);
    if (!ca_sink) {
        fprintf(stderr, "socket_sink_create_tls with ca_file failed\n");
        return 1;
    }
    ca_sink->destroy(ca_sink);

    /* Test socket_sink_create_async boundary checks */
    if (socket_sink_create_async(NULL, 9000, false, NULL, false, 0, 0, 0) != NULL) {
        fprintf(stderr, "socket_sink_create_async with NULL host should fail\n");
        return 1;
    }

    /* Test socket_sink_create_async with default 0 options */
    log_sink_t *async_sink =
        socket_sink_create_async("127.0.0.1", 9000, false, NULL, false, 0, 0, 0);
    if (!async_sink) {
        fprintf(stderr, "socket_sink_create_async failed\n");
        return 1;
    }
    async_sink->write(async_sink, "async msg\n", 10);
    async_sink->flush(async_sink);
    async_sink->atfork_child(async_sink);
    async_sink->destroy(async_sink);

    /* Test socket_sink_create_async with explicit capacity and backoff, destroyed directly */
    log_sink_t *async_sink2 =
        socket_sink_create_async("127.0.0.1", 9000, false, NULL, false, 1024, 100, 1000);
    if (!async_sink2) {
        fprintf(stderr, "socket_sink_create_async explicit config failed\n");
        return 1;
    }
    async_sink2->write(async_sink2, "async msg 2\n", 12);
    async_sink2->destroy(async_sink2);

    /* Test atfork_child NULL checks */
    log_sink_t *dummy_sink = socket_sink_create("127.0.0.1", 9000);
    dummy_sink->atfork_child(NULL);
    log_sink_t empty_sink_struct;
    memset(&empty_sink_struct, 0, sizeof(empty_sink_struct));
    dummy_sink->atfork_child(&empty_sink_struct);

    /* Test connected socket atfork_child and destroy */
    clog_socket_t      sfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family      = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    saddr.sin_port        = 0;
    if (bind(sfd, (struct sockaddr *)&saddr, sizeof(saddr)) == 0) {
#if defined(_WIN32) || defined(_WIN64)
        int slen = sizeof(saddr);
#else
        socklen_t slen = sizeof(saddr);
#endif
        getsockname(sfd, (struct sockaddr *)&saddr, &slen);
        listen(sfd, 2);
        int test_port = ntohs(saddr.sin_port);

        log_sink_t *conn_sink = socket_sink_create("127.0.0.1", test_port);
        if (conn_sink) {
            conn_sink->write(conn_sink, "test\n", 5);
            clog_socket_t cfd = accept(sfd, NULL, NULL);

            /* Test atfork_child on connected socket */
            conn_sink->atfork_child(conn_sink);

            /* Re-connect by writing again */
            conn_sink->write(conn_sink, "test2\n", 6);
            clog_socket_t cfd2 = accept(sfd, NULL, NULL);

            /* Destroy while connected */
            conn_sink->destroy(conn_sink);

            if (!clog_is_invalid_socket(cfd)) {
                clog_close_socket(cfd);
            }
            if (!clog_is_invalid_socket(cfd2)) {
                clog_close_socket(cfd2);
            }
        }
        clog_close_socket(sfd);
    }

    /* Test socket_connect / socket_write when sockfd is invalid but connected flag is set */
    typedef struct {
        clog_socket_t sockfd;
        const char   *host;
        int           port;
        int           connected;
    } dummy_socket_data_t;
    dummy_socket_data_t *ddata = (dummy_socket_data_t *)dummy_sink->private_data;
    ddata->sockfd              = CLOG_INVALID_SOCKET;
    ddata->connected           = 1;
    if (dummy_sink->write(dummy_sink, "fail\n", 5) != -1) {
        fprintf(stderr, "write with invalid sockfd and connected=1 should return -1\n");
        return 1;
    }
    ddata->connected = 0;
    dummy_sink->destroy(dummy_sink);

    return 0;
}
