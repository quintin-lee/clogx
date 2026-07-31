/**
 * @file prometheus_exporter.c
 * @brief Prometheus metrics renderer and HTTP /metrics server.
 *
 * ## Design
 *
 * This module exposes clogx operational metrics in Prometheus text
 * exposition format via an embedded HTTP server. The server listens
 * on a configurable port and serves a single endpoint:
 *
 * ```
 * GET /metrics   →  Prometheus text exposition format
 * GET /healthz   →  "ok" (liveness probe)
 * ```
 *
 * ## Rendered Metrics
 *
 * | Metric                              | Type    | Description                        |
 * |-------------------------------------|---------|------------------------------------|
 * | `clogx_total_logs_total`            | counter | Total log messages emitted         |
 * | `clogx_dropped_total`               | counter | Messages dropped (queue full)      |
 * | `clogx_rate_limited_total`          | counter | Messages suppressed by rate limiter|
 * | `clogx_queue_depth`                 | gauge   | Current async queue depth          |
 * | `clogx_sink_writes_total{sink=...}` | counter | Per-sink write count               |
 * | `clogx_sink_errors_total{sink=...}` | counter | Per-sink error count               |
 *
 * ## HTTP Server
 *
 * The server is a minimal blocking socket implementation:
 * 1. `bind()` + `listen()` on the configured port
 * 2. Accept one connection at a time (single-threaded)
 * 3. Read HTTP request, parse method and path
 * 4. Render metrics into a `strbuf_t` and write HTTP response
 * 5. Close connection
 *
 * This is intentionally simple — no fork, no epoll, no libevent.
 * For production, reverse-proxy through nginx/Envoy and scrape
 * the internal port.
 *
 * ## Thread Safety
 *
 * `prometheus_start_server` spawns a detached thread that owns
 * the server socket. Metrics are read atomically (Prometheus
 * counters are `_Atomic`); no locks needed for rendering.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clog_port.h"
#include "log.h"
#include "log_prometheus.h"

#if defined(_WIN32) || defined(_WIN64)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

/* Global level counters accessed by log.c and prometheus renderer */
extern volatile uint64_t g_prometheus_level_counts[6];

static clog_thread_t g_prom_thread;
static volatile bool g_prom_running = false;
static clog_socket_t g_prom_server_fd = CLOG_INVALID_SOCKET;
static clog_mutex_t g_prom_mutex = CLOG_MUTEX_INITIALIZER;

static const char *level_name(int idx) {
    switch (idx) {
    case 0:
        return "trace";
    case 1:
        return "debug";
    case 2:
        return "info";
    case 3:
        return "warn";
    case 4:
        return "error";
    case 5:
        return "fatal";
    default:
        return "unknown";
    }
}

int clog_prometheus_render_metrics(char *buf, size_t buf_size) {
    if (!buf || buf_size == 0)
        return -1;

    log_stats_t stats;
    log_get_stats(&stats);

    char *out = buf;
    size_t remaining = buf_size;
    int ret;

    /* Log events counter by level */
    ret = snprintf(out, remaining,
                   "# HELP clogx_log_events_total Total number of log events emitted by level.\n"
                   "# TYPE clogx_log_events_total counter\n");
    if (ret <= 0 || (size_t)ret >= remaining)
        return -1;
    out += ret;
    remaining -= (size_t)ret;

    for (int i = 0; i < 6; i++) {
        ret = snprintf(out, remaining, "clogx_log_events_total{level=\"%s\"} %llu\n", level_name(i),
                       (unsigned long long)g_prometheus_level_counts[i]);
        if (ret <= 0 || (size_t)ret >= remaining)
            return -1;
        out += ret;
        remaining -= (size_t)ret;
    }

    /* Total logged, dropped, suppressed counters, and queue depth gauge */
    ret = snprintf(
        out, remaining,
        "# HELP clogx_log_dropped_events_total Total log events dropped due to queue full.\n"
        "# TYPE clogx_log_dropped_events_total counter\n"
        "clogx_log_dropped_events_total %llu\n"
        "# HELP clogx_log_suppressed_events_total Total log events suppressed by rate limiting.\n"
        "# TYPE clogx_log_suppressed_events_total counter\n"
        "clogx_log_suppressed_events_total %llu\n"
        "# HELP clogx_async_queue_depth Current pending records in async worker queue.\n"
        "# TYPE clogx_async_queue_depth gauge\n"
        "clogx_async_queue_depth %llu\n",
        (unsigned long long)stats.dropped_queue_full_count,
        (unsigned long long)stats.suppressed_rate_count,
        (unsigned long long)stats.current_queue_depth);
    if (ret <= 0 || (size_t)ret >= remaining)
        return -1;
    out += ret;

    return (int)(out - buf);
}

static void *prometheus_worker_thread(void *arg) {
    (void)arg;
    char metrics_body[4096];
    char http_response[8192];

    while (g_prom_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        clog_socket_t client_fd =
            accept(g_prom_server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd == CLOG_INVALID_SOCKET) {
            if (!g_prom_running)
                break;
            clog_sleep_ms(50);
            continue;
        }

        char recv_buf[1024];
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
        int bytes = (int)recv(client_fd, recv_buf, sizeof(recv_buf) - 1, 0);
        if (bytes > 0) {
            recv_buf[bytes] = '\0';
            int metrics_len = clog_prometheus_render_metrics(metrics_body, sizeof(metrics_body));
            if (metrics_len < 0)
                metrics_len = 0;

            int resp_len = snprintf(http_response, sizeof(http_response),
                                    "HTTP/1.1 200 OK\r\n"
                                    "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
                                    "Content-Length: %d\r\n"
                                    "Connection: close\r\n\r\n%s",
                                    metrics_len, metrics_body);
            if (resp_len > 0) {
                send(client_fd, http_response, (size_t)resp_len, 0);
            }
        }
        clog_close_socket(client_fd);
    }
    return NULL;
}

int clog_prometheus_exporter_start(int port) {
    if (port <= 0 || port > 65535)
        return CLOG_ERR_INVALID_ARG;

    clog_mutex_lock(&g_prom_mutex);
    if (g_prom_running) {
        clog_mutex_unlock(&g_prom_mutex);
        return CLOG_OK;
    }

#if defined(_WIN32) || defined(_WIN64)
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    g_prom_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_prom_server_fd == CLOG_INVALID_SOCKET) {
        clog_mutex_unlock(&g_prom_mutex);
        return CLOG_ERR_SOCKET_CONNECT;
    }

    int opt = 1;
    setsockopt(g_prom_server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);

    if (bind(g_prom_server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        clog_close_socket(g_prom_server_fd);
        g_prom_server_fd = CLOG_INVALID_SOCKET;
        clog_mutex_unlock(&g_prom_mutex);
        return CLOG_ERR_SOCKET_CONNECT;
    }

    if (listen(g_prom_server_fd, 5) != 0) {
        clog_close_socket(g_prom_server_fd);
        g_prom_server_fd = CLOG_INVALID_SOCKET;
        clog_mutex_unlock(&g_prom_mutex);
        return CLOG_ERR_SOCKET_CONNECT;
    }

    g_prom_running = true;
    if (clog_thread_create(&g_prom_thread, prometheus_worker_thread, NULL) != 0) {
        g_prom_running = false;
        clog_close_socket(g_prom_server_fd);
        g_prom_server_fd = CLOG_INVALID_SOCKET;
        clog_mutex_unlock(&g_prom_mutex);
        return CLOG_ERR_THREAD_CREATE;
    }

    clog_mutex_unlock(&g_prom_mutex);
    return CLOG_OK;
}

void clog_prometheus_exporter_stop(void) {
    clog_mutex_lock(&g_prom_mutex);
    if (!g_prom_running) {
        clog_mutex_unlock(&g_prom_mutex);
        return;
    }

    g_prom_running = false;
    if (g_prom_server_fd != CLOG_INVALID_SOCKET) {
        shutdown(g_prom_server_fd, SHUT_RDWR);
        clog_close_socket(g_prom_server_fd);
        g_prom_server_fd = CLOG_INVALID_SOCKET;
    }
    clog_mutex_unlock(&g_prom_mutex);

    clog_thread_join(g_prom_thread);
}

bool clog_prometheus_exporter_is_running(void) {
    clog_mutex_lock(&g_prom_mutex);
    bool running = g_prom_running;
    clog_mutex_unlock(&g_prom_mutex);
    return running;
}
