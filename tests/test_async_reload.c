#include "clog_port.h"
#include "log.h"
#include "log_async.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_PATH "build/config_async_reload.yaml"
#define LOG_PATH "logs/async_reload.log"

static int write_config(int async)
{
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        return -1;
    }
    fprintf(f,
            "log:\n"
            "  async: %s\n"
            "  queue_size: 1024\n"
            "  color: false\n"
            "  format: '[%%level] %%msg'\n"
            "  console_enable: false\n"
            "  file_enable: true\n"
            "  file_path: %s\n"
            "  max_size: 100MB\n"
            "  backups: 2\n"
            "  socket_enable: false\n",
            async ? "true" : "false",
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

    if (write_config(1) != 0 || log_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "async init failed\n");
        return 1;
    }
    if (!log_async_is_running()) {
        fprintf(stderr, "async should be running after init\n");
        return 1;
    }
    LOG_INFO("async-1");
    log_flush();

    if (write_config(0) != 0 || log_reload() != 0) {
        fprintf(stderr, "reload to sync failed\n");
        return 1;
    }
    if (log_async_is_running()) {
        fprintf(stderr, "async should stop after reload to sync\n");
        return 1;
    }
    LOG_INFO("sync-1");
    log_flush();

    if (write_config(1) != 0 || log_reload() != 0) {
        fprintf(stderr, "reload to async failed\n");
        return 1;
    }
    if (!log_async_is_running()) {
        fprintf(stderr, "async should restart after reload to async\n");
        return 1;
    }
    LOG_INFO("async-2");
    log_flush();
    log_destroy();

    if (log_async_is_running()) {
        fprintf(stderr, "async should stop after destroy\n");
        return 1;
    }

    int lines = count_lines(LOG_PATH);
    printf("async reload: %d lines\n", lines);
    if (lines != 3) {
        fprintf(stderr, "expected 3 lines, got %d\n", lines);
        return 1;
    }
    return 0;
}
