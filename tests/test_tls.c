/**
 * @file test_tls.c
 * @brief Comprehensive test suite for OpenSSL TLS socket sink and configuration.
 */

#include "clog_port.h"
#include "log.h"
#include "log_config.h"
#include "log_internal.h"
#include "log_sink.h"
#include "socket_async.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CLOG_USE_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#define CERT_PATH "build/test_tls_cert.pem"
#define KEY_PATH "build/test_tls_key.pem"

typedef struct {
    int          port;
    int          ready;
    int          received_lines;
    char         received_data[16384];
    clog_mutex_t mutex;
    clog_cond_t  cond;
    const char  *cert_path;
    const char  *key_path;
} tls_server_ctx_t;

static int generate_cert_files(const char *cert_path, const char *key_path)
{
    char cmd[512];
    snprintf(cmd,
             sizeof(cmd),
             "openssl req -x509 -newkey rsa:2048 -keyout %s -out %s "
             "-sha256 -days 1 -nodes -subj '/CN=127.0.0.1' -addext 'subjectAltName=IP:127.0.0.1' "
             ">/dev/null 2>&1",
             key_path,
             cert_path);
    return system(cmd);
}

static void *tls_server_thread_func(void *arg)
{
    tls_server_ctx_t *ctx = (tls_server_ctx_t *)arg;
    clog_net_init();

    clog_socket_t server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (clog_is_invalid_socket(server_fd)) {
        perror("tls server socket");
        return NULL;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("tls server bind");
        clog_close_socket(server_fd);
        return NULL;
    }

#if defined(_WIN32) || defined(_WIN64)
    int len = sizeof(addr);
#else
    socklen_t len = sizeof(addr);
#endif
    if (getsockname(server_fd, (struct sockaddr *)&addr, &len) < 0) {
        perror("tls server getsockname");
        clog_close_socket(server_fd);
        return NULL;
    }
    ctx->port = ntohs(addr.sin_port);

    if (listen(server_fd, 1) < 0) {
        perror("tls server listen");
        clog_close_socket(server_fd);
        return NULL;
    }

    clog_mutex_lock(&ctx->mutex);
    ctx->ready = 1;
    clog_cond_signal(&ctx->cond);
    clog_mutex_unlock(&ctx->mutex);

#ifdef CLOG_USE_TLS
    const SSL_METHOD *method  = TLS_server_method();
    SSL_CTX          *ssl_ctx = SSL_CTX_new(method);
    if (!ssl_ctx) {
        clog_close_socket(server_fd);
        return NULL;
    }

    if (SSL_CTX_use_certificate_file(ssl_ctx, ctx->cert_path, SSL_FILETYPE_PEM) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ssl_ctx, ctx->key_path, SSL_FILETYPE_PEM) <= 0) {
        fprintf(stderr, "tls server failed to load cert/key\n");
        SSL_CTX_free(ssl_ctx);
        clog_close_socket(server_fd);
        return NULL;
    }

    clog_socket_t client_fd = accept(server_fd, NULL, NULL);
    if (!clog_is_invalid_socket(client_fd)) {
        SSL *ssl = SSL_new(ssl_ctx);
        SSL_set_fd(ssl, (int)client_fd);

        int acc_ret = SSL_accept(ssl);
        if (acc_ret > 0) {
            char buf[2048];
            int  total = 0;
            int  n;
            while ((n = SSL_read(ssl, buf, (int)(sizeof(buf) - 1))) > 0) {
                buf[n] = '\0';
                if (total + n < (int)sizeof(ctx->received_data) - 1) {
                    memcpy(ctx->received_data + total, buf, (size_t)n);
                    total += n;
                    ctx->received_data[total] = '\0';
                }
            }
            SSL_set_shutdown(ssl, SSL_SENT_SHUTDOWN);
            SSL_shutdown(ssl);
        } else {
            fprintf(stderr,
                    "SSL_accept failed: ret=%d, err=%d\n",
                    acc_ret,
                    SSL_get_error(ssl, acc_ret));
            ERR_print_errors_fp(stderr);
        }
        SSL_free(ssl);
        clog_close_socket(client_fd);
    }
    SSL_CTX_free(ssl_ctx);
#else
    clog_socket_t client_fd = accept(server_fd, NULL, NULL);
    if (!clog_is_invalid_socket(client_fd)) {
        char buf[2048];
        recv(client_fd, buf, sizeof(buf), 0);
        clog_close_socket(client_fd);
    }
#endif

    clog_close_socket(server_fd);

    int lines = 0;
    for (int i = 0; ctx->received_data[i] != '\0'; i++) {
        if (ctx->received_data[i] == '\n') {
            lines++;
        }
    }
    ctx->received_lines = lines;
    return NULL;
}

static void test_tls_config_parsing(void)
{
    printf("=== test_tls_config_parsing ===\n");
    const char *yaml_content = "log:\n"
                               "  socket_enable: true\n"
                               "  socket_host: \"127.0.0.1\"\n"
                               "  socket_port: 9090\n"
                               "  socket_tls: true\n"
                               "  socket_tls_ca_file: \"" CERT_PATH "\"\n"
                               "  socket_tls_skip_verify: true\n";

    const char *yaml_file = "build/test_tls_cfg.yaml";
    FILE       *f         = fopen(yaml_file, "w");
    assert(f != NULL);
    fputs(yaml_content, f);
    fclose(f);

    logger_t logger;
    memset(&logger, 0, sizeof(logger));
    int res = log_config_load_into(&logger, yaml_file);
    assert(res == 0);
    assert(logger.config.socket_enable != 0);
    assert(logger.config.socket_tls == true);
    assert(strcmp(logger.config.socket_tls_ca_file, CERT_PATH) == 0);
    assert(logger.config.socket_tls_skip_verify == true);

    /* Test alias keys: tls_enable, tls_ca_file, tls_skip_verify */
    const char *alias_yaml_content = "log:\n"
                                     "  socket_enable: true\n"
                                     "  tls_enable: true\n"
                                     "  tls_ca_file: \"/tmp/ca.crt\"\n"
                                     "  tls_skip_verify: false\n";
    f                              = fopen(yaml_file, "w");
    assert(f != NULL);
    fputs(alias_yaml_content, f);
    fclose(f);

    res = log_config_load_into(&logger, yaml_file);
    assert(res == 0);
    assert(logger.config.socket_tls == true);
    assert(strcmp(logger.config.socket_tls_ca_file, "/tmp/ca.crt") == 0);
    assert(logger.config.socket_tls_skip_verify == false);

    remove(yaml_file);
    printf("  test_tls_config_parsing PASSED\n");
}

static void test_sync_socket_tls_skip_verify(void)
{
    printf("=== test_sync_socket_tls_skip_verify ===\n");
#ifndef CLOG_USE_TLS
    printf("  [SKIPPED] CLOG_USE_TLS not defined\n");
    return;
#else
    tls_server_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    clog_mutex_init(&ctx.mutex);
    clog_cond_init(&ctx.cond);
    ctx.cert_path = CERT_PATH;
    ctx.key_path  = KEY_PATH;

    clog_thread_t server_thread;
    int           ret = clog_thread_create(&server_thread, tls_server_thread_func, &ctx);
    assert(ret == 0);

    clog_mutex_lock(&ctx.mutex);
    while (!ctx.ready) {
        clog_cond_wait(&ctx.cond, &ctx.mutex);
    }
    clog_mutex_unlock(&ctx.mutex);

    log_sink_t *tls_sink = socket_sink_create_tls("127.0.0.1", ctx.port, true, NULL, true);
    assert(tls_sink != NULL);

    assert(log_init(NULL) == 0);
    log_set_level(LOG_LEVEL_INFO);
    assert(log_add_sink(tls_sink) == 0);

    for (int i = 0; i < 5; i++) {
        LOG_INFO("tls-sync-skip-verify-msg-%d", i);
    }

    clog_sleep_ms(50);
    log_flush();
    log_destroy();

    clog_thread_join(server_thread);
    clog_mutex_destroy(&ctx.mutex);
    clog_cond_destroy(&ctx.cond);

    assert(ctx.received_lines == 5);
    printf("  test_sync_socket_tls_skip_verify PASSED (5/5 lines received over TLS)\n");
#endif
}

static void test_sync_socket_tls_with_ca(void)
{
    printf("=== test_sync_socket_tls_with_ca ===\n");
#ifndef CLOG_USE_TLS
    printf("  [SKIPPED] CLOG_USE_TLS not defined\n");
    return;
#else
    tls_server_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    clog_mutex_init(&ctx.mutex);
    clog_cond_init(&ctx.cond);
    ctx.cert_path = CERT_PATH;
    ctx.key_path  = KEY_PATH;

    clog_thread_t server_thread;
    int           ret = clog_thread_create(&server_thread, tls_server_thread_func, &ctx);
    assert(ret == 0);

    clog_mutex_lock(&ctx.mutex);
    while (!ctx.ready) {
        clog_cond_wait(&ctx.cond, &ctx.mutex);
    }
    clog_mutex_unlock(&ctx.mutex);

    /* Verify using self-signed CA cert file with skip_verify = false */
    log_sink_t *tls_sink = socket_sink_create_tls("127.0.0.1", ctx.port, true, CERT_PATH, false);
    assert(tls_sink != NULL);

    assert(log_init(NULL) == 0);
    log_set_level(LOG_LEVEL_INFO);
    assert(log_add_sink(tls_sink) == 0);

    for (int i = 0; i < 5; i++) {
        LOG_INFO("tls-sync-ca-verify-msg-%d", i);
    }

    clog_sleep_ms(50);
    log_flush();
    log_destroy();

    clog_thread_join(server_thread);
    clog_mutex_destroy(&ctx.mutex);
    clog_cond_destroy(&ctx.cond);

    assert(ctx.received_lines == 5);
    printf("  test_sync_socket_tls_with_ca PASSED (5/5 lines received over verified TLS)\n");
#endif
}

static void test_async_socket_tls(void)
{
    printf("=== test_async_socket_tls ===\n");
#ifndef CLOG_USE_TLS
    printf("  [SKIPPED] CLOG_USE_TLS not defined\n");
    return;
#else
    tls_server_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    clog_mutex_init(&ctx.mutex);
    clog_cond_init(&ctx.cond);
    ctx.cert_path = CERT_PATH;
    ctx.key_path  = KEY_PATH;

    clog_thread_t server_thread;
    int           ret = clog_thread_create(&server_thread, tls_server_thread_func, &ctx);
    assert(ret == 0);

    clog_mutex_lock(&ctx.mutex);
    while (!ctx.ready) {
        clog_cond_wait(&ctx.cond, &ctx.mutex);
    }
    clog_mutex_unlock(&ctx.mutex);

    log_sink_t *async_tls_sink =
        socket_sink_create_async("127.0.0.1", ctx.port, true, CERT_PATH, true, 1024, 20, 200);
    assert(async_tls_sink != NULL);

    assert(log_init(NULL) == 0);
    log_set_level(LOG_LEVEL_INFO);
    assert(log_add_sink(async_tls_sink) == 0);

    for (int i = 0; i < 5; i++) {
        LOG_INFO("tls-async-msg-%d", i);
    }

    clog_sleep_ms(100);
    log_flush();
    log_destroy();

    clog_thread_join(server_thread);
    clog_mutex_destroy(&ctx.mutex);
    clog_cond_destroy(&ctx.cond);

    if (ctx.received_lines != 5) {
        printf("  [async error] received_lines=%d, received_data='%.200s'\n",
               ctx.received_lines,
               ctx.received_data);
        fflush(stdout);
    }
    assert(ctx.received_lines == 5);
    printf("  test_async_socket_tls PASSED (5/5 lines received over Async TLS)\n");
    fflush(stdout);
#endif
}

static void *accept_and_close_thread(void *arg)
{
    clog_socket_t server_fd = *(clog_socket_t *)arg;
    clog_socket_t client_fd = accept(server_fd, NULL, NULL);
    if (!clog_is_invalid_socket(client_fd)) {
        char dummy[128];
        (void)recv(client_fd, dummy, sizeof(dummy), 0);
        clog_close_socket(client_fd);
    }
    return NULL;
}

static void test_tls_error_paths(void)
{
    printf("=== test_tls_error_paths ===\n");
    fflush(stdout);
    /* Boundary check: NULL host */
    assert(socket_sink_create_tls(NULL, 9000, true, NULL, false) == NULL);

    /* Boundary check: Empty host */
    assert(socket_sink_create_tls("", 9000, true, NULL, false) == NULL);

    /* Boundary check: Port out of range */
    assert(socket_sink_create_tls("127.0.0.1", 0, true, NULL, false) == NULL);
    assert(socket_sink_create_tls("127.0.0.1", 70000, true, NULL, false) == NULL);
    assert(socket_sink_create_tls("127.0.0.1", -5, true, NULL, false) == NULL);

    /* Connect to closed port */
    log_sink_t *closed_sink = socket_sink_create_tls("127.0.0.1", 59998, true, NULL, true);
    if (closed_sink) {
        /* write should fail gracefully */
        closed_sink->write(closed_sink, "test", 4);
        closed_sink->destroy(closed_sink);
    }

#ifdef CLOG_USE_TLS
    /* Connect to non-TLS TCP listener (handshake failure) */
    clog_net_init();
    clog_socket_t tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (!clog_is_invalid_socket(tcp_fd)) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;
        if (bind(tcp_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
#if defined(_WIN32) || defined(_WIN64)
            int len = sizeof(addr);
#else
            socklen_t len = sizeof(addr);
#endif
            getsockname(tcp_fd, (struct sockaddr *)&addr, &len);
            listen(tcp_fd, 1);
            int port = ntohs(addr.sin_port);

            clog_thread_t acc_thread;
            if (clog_thread_create(&acc_thread, accept_and_close_thread, &tcp_fd) == 0) {
                log_sink_t *handshake_fail_sink =
                    socket_sink_create_tls("127.0.0.1", port, true, NULL, true);
                if (handshake_fail_sink) {
                    handshake_fail_sink->write(handshake_fail_sink, "test_handshake", 14);
                    handshake_fail_sink->destroy(handshake_fail_sink);
                }
                clog_thread_join(acc_thread);
            }
        }
        clog_close_socket(tcp_fd);
    }
#endif

#ifdef CLOG_USE_TLS
    ERR_clear_error();
#endif
    printf("  test_tls_error_paths PASSED\n");
    fflush(stdout);
}

int main(void)
{
    printf("Starting test_tls...\n");
    fflush(stdout);

#ifdef CLOG_USE_TLS
    ERR_clear_error();
    if (generate_cert_files(CERT_PATH, KEY_PATH) != 0) {
        fprintf(stderr, "Warning: Failed to generate test cert/key files via openssl\n");
    }
#endif

    test_tls_config_parsing();
    test_tls_error_paths();
    test_sync_socket_tls_skip_verify();
    test_sync_socket_tls_with_ca();
    test_async_socket_tls();

    printf("ALL TLS TESTS PASSED!\n");
    fflush(stdout);
    return 0;
}
