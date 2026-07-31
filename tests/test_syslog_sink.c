/**
 * @file test_syslog_sink.c
 * @brief Regression test: native POSIX syslog sink integration.
 */

#include "log.h"
#include "log_sink.h"
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
#ifndef _WIN32
    if (log_init(NULL) != 0) {
        fprintf(stderr, "log_init failed\n");
        return 1;
    }

    log_sink_t *sink = syslog_sink_create("test_clogx", 1 << 3 /* LOG_USER */);
    if (!sink) {
        fprintf(stderr, "failed to create syslog sink\n");
        return 1;
    }

    if (log_add_sink(sink) != 0) {
        fprintf(stderr, "failed to add syslog sink\n");
        return 1;
    }

    LOG_INFO("syslog sink test message");
    log_destroy();
    printf("syslog sink test passed\n");
#else
    printf("syslog sink test skipped on Windows\n");
#endif
    return 0;
}
