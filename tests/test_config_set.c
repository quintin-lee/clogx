#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clog_port.h"
#include "log.h"
#include "log_sink.h"

#define LOG_PATH "logs/config_set_test.log"

static int count_lines(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    int n = 0;
    char buf[256];
    while (fgets(buf, sizeof(buf), f))
        n++;
    fclose(f);
    return n;
}

int main(void) {
    remove(LOG_PATH);

    if (log_init(NULL) != 0) {
        fprintf(stderr, "log_init failed\n");
        return 1;
    }

    log_sink_t *sink = file_sink_create(LOG_PATH, 0, 0);
    if (!sink) {
        fprintf(stderr, "file_sink_create failed\n");
        log_destroy();
        return 1;
    }
    if (log_add_sink(sink) != 0) {
        fprintf(stderr, "log_add_sink failed\n");
        log_destroy();
        return 1;
    }

    log_config_t cfg = {0};
    cfg.level = LOG_LEVEL_WARN;
    cfg.async = false;
    cfg.color = false;
    cfg.format = "[%level] %msg";
    cfg.time_format = "%Y-%m-%d %H:%M:%S";
    cfg.console_enable = false;
    cfg.file_enable = true;
    snprintf(cfg.file_path, sizeof(cfg.file_path), "%s", LOG_PATH);
    cfg.file_max_size = 50 * 1024 * 1024;
    cfg.file_backups = 5;
    cfg.socket_enable = false;

    if (log_config_set(&cfg) != 0) {
        fprintf(stderr, "log_config_set failed\n");
        log_destroy();
        return 1;
    }

    const log_config_t *got = log_config_get();
    if (!got) {
        fprintf(stderr, "log_config_get returned NULL\n");
        log_destroy();
        return 1;
    }

    if (got->level != LOG_LEVEL_WARN) {
        fprintf(stderr, "level mismatch: expected WARN, got %d\n", got->level);
        log_destroy();
        return 1;
    }
    if (got->async != false) {
        fprintf(stderr, "async mismatch\n");
        log_destroy();
        return 1;
    }
    if (got->color != false) {
        fprintf(stderr, "color mismatch\n");
        log_destroy();
        return 1;
    }
    if (strcmp(got->format, "[%level] %msg") != 0) {
        fprintf(stderr, "format mismatch: %s\n", got->format);
        log_destroy();
        return 1;
    }
    if (strcmp(got->time_format, "%Y-%m-%d %H:%M:%S") != 0) {
        fprintf(stderr, "time_format mismatch: %s\n", got->time_format);
        log_destroy();
        return 1;
    }
    if (got->file_enable != 1) {
        fprintf(stderr, "file_enable mismatch\n");
        log_destroy();
        return 1;
    }
    if (strcmp(got->file_path, LOG_PATH) != 0) {
        fprintf(stderr, "file_path mismatch\n");
        log_destroy();
        return 1;
    }
    if (got->file_max_size != 50ULL * 1024 * 1024) {
        fprintf(stderr, "file_max_size mismatch\n");
        log_destroy();
        return 1;
    }
    if (got->file_backups != 5) {
        fprintf(stderr, "file_backups mismatch\n");
        log_destroy();
        return 1;
    }

    LOG_TRACE("should-be-filtered");
    LOG_DEBUG("should-be-filtered");
    LOG_INFO("should-be-filtered");
    LOG_WARN("warn-msg");
    LOG_ERROR("error-msg");

    log_flush();
    log_destroy();

    int lines = count_lines(LOG_PATH);
    printf("config_set test: %d/2 lines\n", lines);
    if (lines != 2) {
        fprintf(stderr, "expected 2 lines (WARN+), got %d\n", lines);
        return 1;
    }

    FILE *f = fopen(LOG_PATH, "rb");
    if (!f)
        return 1;
    char content[1024] = {0};
    fread(content, 1, sizeof(content) - 1, f);
    fclose(f);

    if (strstr(content, "warn-msg") == NULL) {
        fprintf(stderr, "missing warn-msg\n");
        return 1;
    }
    if (strstr(content, "error-msg") == NULL) {
        fprintf(stderr, "missing error-msg\n");
        return 1;
    }
    if (strstr(content, "should-be-filtered") != NULL) {
        fprintf(stderr, "filtered message leaked\n");
        return 1;
    }

    printf("config_set test passed\n");
    return 0;
}
