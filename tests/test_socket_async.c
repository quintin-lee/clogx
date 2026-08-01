/**
 * @file test_socket_async.c
 * @brief Regression tests for async socket ring buffer and writer thread.
 */

#include "socket_async.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Ring buffer unit tests ── */

static void test_ring_create_destroy(void)
{
    socket_ring_buffer_t *ring = socket_ring_create(16);
    assert(ring != NULL);
    assert(ring->capacity == 16);
    assert(ring->count == 0);
    assert(ring->dropped == 0);
    socket_ring_destroy(ring);
    printf("  test_ring_create_destroy PASSED\n");
}

static void test_ring_create_zero_capacity(void)
{
    socket_ring_buffer_t *ring = socket_ring_create(0);
    assert(ring == NULL);
    printf("  test_ring_create_zero_capacity PASSED\n");
}

static void test_ring_put_get(void)
{
    socket_ring_buffer_t *ring = socket_ring_create(4);
    assert(ring != NULL);

    assert(socket_ring_put(ring, "line1", 5) == 0);
    assert(socket_ring_put(ring, "line2", 5) == 0);
    assert(ring->count == 2);

    const char *lines[4];
    size_t      lengths[4];
    int         n = socket_ring_get_batch(ring, lines, lengths, 4);
    assert(n == 2);
    assert(lengths[0] == 5);
    assert(memcmp(lines[0], "line1", 5) == 0);
    assert(lengths[1] == 5);
    assert(memcmp(lines[1], "line2", 5) == 0);
    assert(ring->count == 0);

    for (int i = 0; i < n; i++) {
        free((void *)lines[i]);
    }

    socket_ring_destroy(ring);
    printf("  test_ring_put_get PASSED\n");
}

static void test_ring_overflow_drops(void)
{
    socket_ring_buffer_t *ring = socket_ring_create(2);
    assert(ring != NULL);

    assert(socket_ring_put(ring, "aaa", 3) == 0);
    assert(socket_ring_put(ring, "bbb", 3) == 0);
    /* ring full — next put should drop oldest */
    assert(socket_ring_put(ring, "ccc", 3) == 0);
    assert(ring->dropped == 1);
    assert(ring->count == 2);

    const char *lines[2];
    size_t      lengths[2];
    int         n = socket_ring_get_batch(ring, lines, lengths, 2);
    assert(n == 2);
    /* oldest (aaa) was dropped, so we get bbb then ccc */
    assert(memcmp(lines[0], "bbb", 3) == 0);
    assert(memcmp(lines[1], "ccc", 3) == 0);

    for (int i = 0; i < n; i++) {
        free((void *)lines[i]);
    }

    socket_ring_destroy(ring);
    printf("  test_ring_overflow_drops PASSED\n");
}

static void test_ring_put_null(void)
{
    socket_ring_buffer_t *ring = socket_ring_create(4);
    assert(ring != NULL);
    assert(socket_ring_put(NULL, "x", 1) == -1);
    assert(socket_ring_put(ring, NULL, 1) == -1);
    assert(socket_ring_put(ring, "x", 0) == -1);
    socket_ring_destroy(ring);
    printf("  test_ring_put_null PASSED\n");
}

static void test_ring_close_rejects_puts(void)
{
    socket_ring_buffer_t *ring = socket_ring_create(4);
    assert(ring != NULL);
    assert(socket_ring_put(ring, "a", 1) == 0);
    socket_ring_close(ring);
    assert(socket_ring_put(ring, "b", 1) == -1);
    /* existing entry still retrievable */
    const char *lines[1];
    size_t      lengths[1];
    int         n = socket_ring_get_batch(ring, lines, lengths, 1);
    assert(n == 1);
    free((void *)lines[0]);
    /* drained + closed → -1 */
    n = socket_ring_get_batch(ring, lines, lengths, 1);
    assert(n == -1);
    socket_ring_destroy(ring);
    printf("  test_ring_close_rejects_puts PASSED\n");
}

static void test_ring_empty_batch(void)
{
    socket_ring_buffer_t *ring = socket_ring_create(4);
    assert(ring != NULL);
    const char *lines[4];
    size_t      lengths[4];
    /* get_batch blocks on empty ring — close first so it returns -1 */
    socket_ring_close(ring);
    int n = socket_ring_get_batch(ring, lines, lengths, 4);
    assert(n == -1);
    socket_ring_destroy(ring);
    printf("  test_ring_empty_batch PASSED\n");
}

static void test_ring_get_batch_limit(void)
{
    socket_ring_buffer_t *ring = socket_ring_create(8);
    assert(ring != NULL);
    for (int i = 0; i < 6; i++) {
        char buf[8];
        snprintf(buf, sizeof(buf), "l%d", i);
        assert(socket_ring_put(ring, buf, strlen(buf)) == 0);
    }
    const char *lines[8];
    size_t      lengths[8];
    /* request max 3 */
    int n = socket_ring_get_batch(ring, lines, lengths, 3);
    assert(n == 3);
    assert(ring->count == 3);
    for (int i = 0; i < n; i++) {
        free((void *)lines[i]);
    }
    /* get the rest */
    n = socket_ring_get_batch(ring, lines, lengths, 8);
    assert(n == 3);
    assert(ring->count == 0);
    for (int i = 0; i < n; i++) {
        free((void *)lines[i]);
    }
    socket_ring_destroy(ring);
    printf("  test_ring_get_batch_limit PASSED\n");
}

static void test_ring_signal_wakes(void)
{
    /* signal on empty ring should not crash */
    socket_ring_buffer_t *ring = socket_ring_create(4);
    assert(ring != NULL);
    socket_ring_signal(ring);
    socket_ring_destroy(ring);
    printf("  test_ring_signal_wakes PASSED\n");
}

/* ── Writer thread integration tests (with real TCP server) ── */

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct {
    int             port;
    int             received_lines;
    int             ready;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
} test_server_ctx_t;

static void *test_listener(void *arg)
{
    test_server_ctx_t *ctx       = (test_server_ctx_t *)arg;
    int                server_fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(server_fd >= 0);
    int yes = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;

    assert(bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    assert(getsockname(server_fd, (struct sockaddr *)&addr, &len) == 0);
    ctx->port = ntohs(addr.sin_port);
    assert(listen(server_fd, 1) == 0);

    pthread_mutex_lock(&ctx->mutex);
    ctx->ready = 1;
    pthread_cond_signal(&ctx->cond);
    pthread_mutex_unlock(&ctx->mutex);

    int client_fd = accept(server_fd, NULL, NULL);
    assert(client_fd >= 0);

    char buf[16384];
    int  total = 0;
    int  n;
    while ((n = (int)recv(client_fd, buf + total, sizeof(buf) - 1 - (size_t)total, 0)) > 0) {
        total += n;
    }
    close(client_fd);
    close(server_fd);

    int lines = 0;
    for (int i = 0; i < total; i++) {
        if (buf[i] == '\n') {
            lines++;
        }
    }
    ctx->received_lines = lines;
    return NULL;
}

static void test_writer_integration(void)
{
    test_server_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    pthread_mutex_init(&ctx.mutex, NULL);
    pthread_cond_init(&ctx.cond, NULL);

    pthread_t server_tid;
    assert(pthread_create(&server_tid, NULL, test_listener, &ctx) == 0);

    pthread_mutex_lock(&ctx.mutex);
    while (!ctx.ready) {
        pthread_cond_wait(&ctx.cond, &ctx.mutex);
    }
    pthread_mutex_unlock(&ctx.mutex);

    socket_writer_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.host           = "127.0.0.1";
    cfg.port           = ctx.port;
    cfg.ring_capacity  = 256;
    cfg.backoff_min_ms = 50;
    cfg.backoff_max_ms = 500;

    socket_writer_t *writer = socket_writer_start(&cfg);
    assert(writer != NULL);

    socket_ring_buffer_t *ring = socket_writer_ring(writer);
    assert(ring != NULL);

    for (int i = 0; i < 20; i++) {
        char line[64];
        snprintf(line, sizeof(line), "async-msg-%d", i);
        assert(socket_ring_put(ring, line, strlen(line)) == 0);
    }

    /* Wait for ring to drain (writer appends \n and sends). */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long deadline_sec = tv.tv_sec + 5;
    while (ring->count > 0 && tv.tv_sec < deadline_sec) {
        usleep(10000);
        gettimeofday(&tv, NULL);
    }

    socket_writer_stop(writer);
    free(writer);

    pthread_join(server_tid, NULL);
    assert(ctx.received_lines == 20);
    printf("  test_writer_integration PASSED (20 lines received)\n");
}

static void test_writer_connect_failure_backoff(void)
{
    /* connect to a port that nobody listens on — should not crash */
    socket_writer_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.host              = "127.0.0.1";
    cfg.port              = 1, /* invalid port, nobody listens */
        cfg.ring_capacity = 64;
    cfg.backoff_min_ms    = 10;
    cfg.backoff_max_ms    = 100;

    socket_writer_t *writer = socket_writer_start(&cfg);
    assert(writer != NULL);

    socket_ring_buffer_t *ring = socket_writer_ring(writer);
    /* put a few lines — they should be buffered, not crash */
    for (int i = 0; i < 5; i++) {
        char line[32];
        snprintf(line, sizeof(line), "line-%d\n", i);
        socket_ring_put(ring, line, strlen(line));
    }

    /* give writer a moment to attempt (and fail) connection */
    usleep(50000);

    socket_writer_stop(writer);
    free(writer);
    printf("  test_writer_connect_failure_backoff PASSED\n");
}

static void test_writer_dropped_count(void)
{
    socket_writer_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.host = "127.0.0.1";
    cfg.port = 1, cfg.ring_capacity = 4;
    cfg.backoff_min_ms = 10;
    cfg.backoff_max_ms = 50;

    socket_writer_t *writer = socket_writer_start(&cfg);
    assert(writer != NULL);

    socket_ring_buffer_t *ring = socket_writer_ring(writer);
    /* flood the ring to trigger drops */
    for (int i = 0; i < 20; i++) {
        char line[32];
        snprintf(line, sizeof(line), "flood-%d\n", i);
        socket_ring_put(ring, line, strlen(line));
    }

    usleep(20000);
    uint64_t dropped = socket_writer_dropped(writer);
    assert(dropped > 0);
    printf("  test_writer_dropped_count PASSED (dropped=%lu)\n", (unsigned long)dropped);

    socket_writer_stop(writer);
    free(writer);
}

static void test_writer_stop_null(void)
{
    /* should not crash */
    socket_writer_stop(NULL);
    printf("  test_writer_stop_null PASSED\n");
}

static void test_socket_async_boundary_cases(void)
{
    /* socket_ring_get_batch NULL/invalid args */
    const char *lines[4];
    size_t      lengths[4];
    assert(socket_ring_get_batch(NULL, lines, lengths, 4) == -1);
    socket_ring_buffer_t *ring = socket_ring_create(4);
    assert(socket_ring_get_batch(ring, NULL, lengths, 4) == -1);
    assert(socket_ring_get_batch(ring, lines, NULL, 4) == -1);
    assert(socket_ring_get_batch(ring, lines, lengths, 0) == -1);

    /* NULL checks for close, signal, destroy */
    socket_ring_close(NULL);
    socket_ring_signal(NULL);
    socket_ring_destroy(NULL);

    /* socket_writer_start NULL / invalid args */
    assert(socket_writer_start(NULL) == NULL);
    socket_writer_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    assert(socket_writer_start(&cfg) == NULL);
    cfg.host = "127.0.0.1";
    cfg.port = -1;
    assert(socket_writer_start(&cfg) == NULL);
    cfg.port = 70000;
    assert(socket_writer_start(&cfg) == NULL);

    /* socket_writer_start with zero defaults and TLS requested */
    cfg.port           = 9000;
    cfg.use_tls        = true;
    cfg.ring_capacity  = 0;
    cfg.backoff_min_ms = 0;
    cfg.backoff_max_ms = 0;
    socket_writer_t *w = socket_writer_start(&cfg);
    assert(w != NULL);
    assert(socket_writer_ring(w) != NULL);

    usleep(20000);
    socket_writer_stop(w);
    free(w);

    /* socket_writer_ring and dropped NULL checks */
    assert(socket_writer_ring(NULL) == NULL);
    assert(socket_writer_dropped(NULL) == 0);

    socket_ring_destroy(ring);
    printf("  test_socket_async_boundary_cases PASSED\n");
}

#endif /* !_WIN32 */

int main(void)
{
    printf("=== test_socket_async ===\n");

    /* ring buffer unit tests */
    test_ring_create_destroy();
    test_ring_create_zero_capacity();
    test_ring_put_get();
    test_ring_overflow_drops();
    test_ring_put_null();
    test_ring_close_rejects_puts();
    test_ring_empty_batch();
    test_ring_get_batch_limit();
    test_ring_signal_wakes();

#if !defined(_WIN32)
    /* writer integration tests */
    test_writer_integration();
    test_writer_connect_failure_backoff();
    test_writer_dropped_count();
    test_writer_stop_null();
    test_socket_async_boundary_cases();
#endif

    printf("=== all socket async tests PASSED ===\n");
    return 0;
}
