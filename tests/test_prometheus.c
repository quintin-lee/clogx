/**
 * @file test_prometheus.c
 * @brief Unit tests for Prometheus metrics renderer and HTTP exporter.
 */

#include "clog_port.h"
#include "log.h"
#include "log_prometheus.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_prometheus_render(void)
{
    char buf[2048];
    int  len = clog_prometheus_render_metrics(buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "# HELP clogx_log_events_total") != NULL);
    assert(strstr(buf, "clogx_log_events_total{level=\"info\"}") != NULL);
    assert(strstr(buf, "# HELP clogx_log_dropped_events_total") != NULL);
    assert(strstr(buf, "# HELP clogx_async_queue_depth") != NULL);
    printf("test_prometheus_render passed\n");
}

static void test_prometheus_exporter(void)
{
    int port = 19090;
    int ret  = -1;
    for (int p = 19090; p < 19100; p++) {
        ret = clog_prometheus_exporter_start(p);
        if (ret == CLOG_OK) {
            port = p;
            break;
        }
    }
    assert(ret == CLOG_OK);
    assert(clog_prometheus_exporter_is_running() == true);

    /* Starting again when running is idempotent */
    ret = clog_prometheus_exporter_start(port);
    assert(ret == CLOG_OK);

    clog_prometheus_exporter_stop();
    assert(clog_prometheus_exporter_is_running() == false);
    printf("test_prometheus_exporter passed (port %d)\n", port);
}

static void test_prometheus_http_socket(void)
{
    int port = 19100;
    int ret  = -1;
    for (int p = 19100; p < 19110; p++) {
        ret = clog_prometheus_exporter_start(p);
        if (ret == CLOG_OK) {
            port = p;
            break;
        }
    }
    assert(ret == CLOG_OK);

    clog_socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    assert(ret == 0);

    const char *req = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    send(fd, req, (clog_sock_size_t)strlen(req), 0);

    char    resp[4096];
    size_t  total = 0;
    ssize_t n;
    while ((n = recv(fd, resp + total, (clog_sock_size_t)(sizeof(resp) - total - 1), 0)) > 0) {
        total += (size_t)n;
        if (total >= sizeof(resp) - 1) {
            break;
        }
    }
    resp[total] = '\0';
    clog_close_socket(fd);

    assert(strstr(resp, "HTTP/1.1 200 OK") != NULL);
    assert(strstr(resp, "Content-Type: text/plain; version=0.0.4; charset=utf-8") != NULL);
    assert(strstr(resp, "clogx_log_events_total") != NULL);

    clog_prometheus_exporter_stop();
    printf("test_prometheus_http_socket passed (port %d)\n", port);
}

int main(void)
{
    test_prometheus_render();
    test_prometheus_exporter();
    test_prometheus_http_socket();
    printf("all prometheus tests passed!\n");
    return 0;
}
