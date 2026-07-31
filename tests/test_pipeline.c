/**
 * @file test_pipeline.c
 * @brief End-to-end logging pipeline test: init → write → file output.
 */
#include "clog_port.h"
#include "log.h"
#include <stdio.h>
#include <string.h>

#define CONFIG_PATH "build/config_pipeline_test.yaml"
#define LOG_PATH "logs/pipeline_test.log"

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
            "  backups: 2\n"
            "  socket_enable: false\n",
            LOG_PATH);
    fclose(f);
    return 0;
}

int main(void)
{
    remove(LOG_PATH);
    if (write_config() != 0) {
        fprintf(stderr, "failed to write config\n");
        return 1;
    }

    if (log_init(CONFIG_PATH) != CLOG_OK) {
        fprintf(stderr, "log_init failed\n");
        return 1;
    }

    LOG_INFO("Server started");
    LOG_WARN("Disk space low");
    LOG_ERROR("Connection failed");
    log_flush();
    log_destroy();

    FILE *f = fopen(LOG_PATH, "rb");
    if (!f) {
        fprintf(stderr, "log file missing\n");
        return 1;
    }

    char   buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    if (!strstr(buf, "[INFO] Server started") || !strstr(buf, "[WARN] Disk space low") ||
        !strstr(buf, "[ERROR] Connection failed")) {
        fprintf(stderr, "pipeline log content mismatch:\n%s\n", buf);
        return 1;
    }

    printf("pipeline test passed\n");
    return 0;
}
