/**
 * @file test_prometheus.c
 * @brief Unit tests for Prometheus metrics renderer and HTTP exporter.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "log.h"
#include "log_prometheus.h"

static void test_prometheus_render(void) {
    char buf[2048];
    int len = clog_prometheus_render_metrics(buf, sizeof(buf));
    assert(len > 0);
    assert(strstr(buf, "# HELP clogx_log_events_total") != NULL);
    assert(strstr(buf, "clogx_log_events_total{level=\"info\"}") != NULL);
    assert(strstr(buf, "# HELP clogx_log_dropped_events_total") != NULL);
    assert(strstr(buf, "# HELP clogx_async_queue_depth") != NULL);
    printf("test_prometheus_render passed\n");
}

static void test_prometheus_exporter(void) {
    int port = 19090;
    int ret = -1;
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

int main(void) {
    test_prometheus_render();
    test_prometheus_exporter();
    printf("all prometheus tests passed!\n");
    return 0;
}
