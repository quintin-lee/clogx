/**
 * @file test_log_level.c
 * @brief Regression test: log level filtering (TRACE through FATAL).
 */

#include "clog_port.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_PATH "logs/log_level_test.log"

static int count_lines(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    int  n = 0;
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        n++;
    }
    fclose(f);
    return n;
}

int main(void)
{
    remove(LOG_PATH);

    /* Init with defaults, only file sink. */
    log_sink_t *sink = file_sink_create(LOG_PATH, 0, 0);
    if (!sink) {
        fprintf(stderr, "file_sink_create failed\n");
        return 1;
    }

    /* Start with TRACE — everything passes. */
    if (log_init(NULL) != 0) {
        fprintf(stderr, "log_init failed\n");
        return 1;
    }
    log_set_level(LOG_LEVEL_TRACE);
    if (log_add_sink(sink) != 0) {
        fprintf(stderr, "log_add_sink failed\n");
        return 1;
    }

    LOG_TRACE("trace-1");
    LOG_INFO("info-1");

    log_flush();
    if (count_lines(LOG_PATH) != 2) {
        fprintf(stderr, "expected 2 lines at TRACE level, got %d\n", count_lines(LOG_PATH));
        return 1;
    }

    /* Raise to INFO — TRACE should be filtered. */
    log_set_level(LOG_LEVEL_INFO);
    LOG_TRACE("trace-2-should-not-appear");
    LOG_INFO("info-2");

    log_flush();
    if (count_lines(LOG_PATH) != 3) {
        fprintf(stderr, "expected 3 lines after raising to INFO, got %d\n", count_lines(LOG_PATH));
        return 1;
    }

    /* Raise to ERROR — INFO and below filtered. */
    log_set_level(LOG_LEVEL_ERROR);
    LOG_TRACE("trace-3-should-not-appear");
    LOG_INFO("info-3-should-not-appear");
    LOG_WARN("warn-3-should-not-appear");
    LOG_ERROR("error-3");
    LOG_FATAL("fatal-3");

    log_flush();
    if (count_lines(LOG_PATH) != 5) {
        fprintf(stderr, "expected 5 lines after raising to ERROR, got %d\n", count_lines(LOG_PATH));
        return 1;
    }

    /* Lower back to DEBUG. */
    log_set_level(LOG_LEVEL_DEBUG);
    LOG_DEBUG("debug-4");
    LOG_INFO("info-4");

    log_flush();
    if (count_lines(LOG_PATH) != 7) {
        fprintf(
            stderr, "expected 7 lines after lowering to DEBUG, got %d\n", count_lines(LOG_PATH));
        return 1;
    }

    /* Verify content: no filtered messages leaked. */
    FILE *f = fopen(LOG_PATH, "rb");
    if (!f) {
        fprintf(stderr, "cannot open log\n");
        return 1;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "should-not-appear")) {
            fprintf(stderr, "filtered message leaked: %s", line);
            fclose(f);
            return 1;
        }
    }
    fclose(f);

    log_destroy();
    printf("log_level test passed\n");
    return 0;
}
