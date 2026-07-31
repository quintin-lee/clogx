/**
 * @file test_dispatcher_lifecycle.c
 * @brief Regression test: dispatcher init, dispatch, and destroy lifecycle.
 */

#include "clog_port.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_PATH "build/config_reload_test.yaml"
#define LOG_PATH "logs/reload_test.log"

static int write_config(void)
{
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        return -1;
    }
    fprintf(f,
            "log:\n"
            "  async: false\n"
            "  color: false\n"
            "  format: '[%%level] %%msg'\n"
            "  console_enable: false\n"
            "  file_enable: true\n"
            "  file_path: %s\n"
            "  max_size: 100MB\n"
            "  backups: 3\n"
            "  socket_enable: false\n",
            LOG_PATH);
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
    if (write_config() != 0) {
        fprintf(stderr, "write config failed\n");
        return 1;
    }

    if (log_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "log_init failed\n");
        return 1;
    }

    LOG_INFO("before-reload");
    if (log_reload() != 0) {
        fprintf(stderr, "log_reload failed\n");
        return 1;
    }
    LOG_INFO("after-reload-1");
    if (log_reload() != 0) {
        fprintf(stderr, "second log_reload failed\n");
        return 1;
    }
    LOG_INFO("after-reload-2");

    log_flush();
    log_destroy();

    int lines = count_lines(LOG_PATH);
    printf("dispatcher lifecycle: %d lines\n", lines);
    if (lines != 3) {
        fprintf(stderr, "expected 3 lines after reload cycle, got %d\n", lines);
        return 1;
    }
    return 0;
}
