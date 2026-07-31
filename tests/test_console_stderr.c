/**
 * @file test_console_stderr.c
 * @brief Regression test: console sink stderr output mode (console_stderr: true).
 */

#include "clog_port.h"
#include "log.h"
#include "log_sink.h"
#include <stdio.h>
#include <string.h>

#define CONFIG_PATH "build/config_console_stderr_test.yaml"

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
            "  format: '%%msg'\n"
            "  console_enable: true\n"
            "  console_stderr: true\n"
            "  file_enable: false\n"
            "  socket_enable: false\n");
    fclose(f);
    return 0;
}

int main(void)
{
    if (write_config() != 0 || log_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "init failed\n");
        return 1;
    }

    log_config_t *cfg = log_config_get();
    if (!cfg->console_stderr) {
        fprintf(stderr, "console_stderr not set\n");
        log_destroy();
        return 1;
    }

    /* Smoke: writing should succeed with stderr console sink. */
    LOG_INFO("stderr-console");
    log_flush();
    log_destroy();

    printf("console stderr config test passed\n");
    return 0;
}
