/**
 * @file test_config_reload.c
 * @brief Tests hot-reload of logging configuration via log_reload().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clog_port.h"
#include "log.h"

#define CONFIG_PATH "build/config_reload_path_test.yaml"
#define LOG_PATH "logs/reload_path_test.log"

static int write_config(const char *level) {
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f)
        return -1;
    fprintf(f,
            "log:\n"
            "  level: %s\n"
            "  async: false\n"
            "  color: false\n"
            "  format: '[%%level] %%msg'\n"
            "  console_enable: false\n"
            "  file_enable: true\n"
            "  file_path: %s\n"
            "  max_size: 100MB\n"
            "  backups: 2\n"
            "  socket_enable: false\n",
            level, LOG_PATH);
    fclose(f);
    return 0;
}

int main(void) {
    remove(LOG_PATH);
    if (write_config("ERROR") != 0)
        return 1;

    if (log_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "log_init failed\n");
        return 1;
    }

    LOG_INFO("should-be-filtered");
    LOG_ERROR("before-change");

    /* Change only the custom config file; reload must re-read that path */
    if (write_config("INFO") != 0)
        return 1;
    if (log_reload() != 0) {
        fprintf(stderr, "log_reload failed\n");
        return 1;
    }

    LOG_INFO("after-reload-should-appear");
    log_flush();
    log_destroy();

    FILE *f = fopen(LOG_PATH, "rb");
    if (!f) {
        fprintf(stderr, "log missing\n");
        return 1;
    }
    char content[1024] = {0};
    fread(content, 1, sizeof(content) - 1, f);
    fclose(f);

    printf("config reload path test content:\n%s", content);
    if (strstr(content, "should-be-filtered") != NULL) {
        fprintf(stderr, "INFO leaked before reload\n");
        return 1;
    }
    if (strstr(content, "before-change") == NULL) {
        fprintf(stderr, "missing ERROR before reload\n");
        return 1;
    }
    if (strstr(content, "after-reload-should-appear") == NULL) {
        fprintf(stderr, "reload did not pick up updated custom config\n");
        return 1;
    }
    return 0;
}
