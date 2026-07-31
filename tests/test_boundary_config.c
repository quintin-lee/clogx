#include "clog_port.h"
#include "log.h"
#include "log_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_PATH "build/config_boundary_test.yaml"
#define LOG_PATH "logs/boundary_test.log"

static int write_config(const char *content)
{
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        return -1;
    }
    fputs(content, f);
    fclose(f);
    return 0;
}

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
    int ret = 0;

    /* 1. Empty YAML file -> defaults */
    if (write_config("") != 0) {
        fprintf(stderr, "write empty config failed\n");
        return 1;
    }
    if (log_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "log_init with empty YAML should succeed\n");
        return 1;
    }
    if (log_config_get()->level != LOG_LEVEL_INFO) {
        fprintf(stderr, "empty YAML should default to INFO\n");
        ret = 1;
    }
    log_destroy();

    /* 2. YAML with only comments -> defaults */
    if (write_config("# just a comment\n# another comment\n") != 0) {
        fprintf(stderr, "write comment-only config failed\n");
        return 1;
    }
    if (log_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "log_init with comment-only YAML should succeed\n");
        return 1;
    }
    if (log_config_get()->level != LOG_LEVEL_INFO) {
        fprintf(stderr, "comment-only YAML should default to INFO\n");
        ret = 1;
    }
    log_destroy();

    /* 3. Old-style top-level keys (no log: section) -> backward compat */
    if (write_config("level: WARN\n"
                     "async: false\n"
                     "color: false\n"
                     "format: '[%level] %msg'\n"
                     "console_enable: false\n"
                     "file_enable: true\n"
                     "file_path: " LOG_PATH "\n"
                     "max_size: 100MB\n"
                     "backups: 2\n"
                     "socket_enable: false\n") != 0) {
        fprintf(stderr, "write old-style config failed\n");
        return 1;
    }
    if (log_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "log_init with old-style YAML should succeed\n");
        return 1;
    }
    if (log_config_get()->level != LOG_LEVEL_WARN) {
        fprintf(stderr, "old-style YAML should parse level=WARN\n");
        ret = 1;
    }
    if (log_config_get()->file_enable != 1) {
        fprintf(stderr, "old-style YAML should parse file_enable\n");
        ret = 1;
    }

    LOG_WARN("old-style-warn");
    LOG_INFO("old-style-info-should-not-appear");
    log_flush();
    log_destroy();

    {
        int lines = count_lines(LOG_PATH);
        if (lines != 1) {
            fprintf(stderr, "old-style expected 1 line, got %d\n", lines);
            ret = 1;
        }
    }
    remove(LOG_PATH);

    /* 4. Unknown keys -> silently skipped, defaults preserved */
    if (write_config("log:\n"
                     "  unknown_key: 123\n"
                     "  level: DEBUG\n"
                     "  another_unknown: abc\n") != 0) {
        fprintf(stderr, "write unknown-keys config failed\n");
        return 1;
    }
    if (log_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "log_init with unknown keys should succeed\n");
        return 1;
    }
    if (log_config_get()->level != LOG_LEVEL_DEBUG) {
        fprintf(stderr, "unknown keys should not affect level\n");
        ret = 1;
    }
    log_destroy();

    /* 5. NULL config path -> defaults */
    if (log_init(NULL) != 0) {
        fprintf(stderr, "log_init(NULL) should succeed\n");
        return 1;
    }
    if (log_config_get()->level != LOG_LEVEL_INFO) {
        fprintf(stderr, "NULL path should default to INFO\n");
        ret = 1;
    }
    log_destroy();

    /* 6. Invalid queue_size (non-numeric) -> log_init fails */
    if (write_config("log:\n"
                     "  queue_size: not_a_number\n"
                     "  async: false\n") != 0) {
        fprintf(stderr, "write invalid queue_size config failed\n");
        return 1;
    }
    if (log_init(CONFIG_PATH) == 0) {
        fprintf(stderr, "Expected log_init to fail with invalid queue_size\n");
        log_destroy();
        ret = 1;
    }

    if (log_init(NULL) == 0) {
        char big_msg[3000];
        memset(big_msg, 'A', sizeof(big_msg) - 1);
        big_msg[sizeof(big_msg) - 1] = '\0';
        LOG_INFO("%s", big_msg);
        log_destroy();
    }

    if (ret == 0) {
        printf("boundary config test passed\n");
    }
    return ret;
}
