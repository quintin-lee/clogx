/**
 * @file test_module_trunc.c
 * @brief Regression test: module name truncation at 63-char boundary.
 */

#include "clog_port.h"
#include "log.h"
#include <stdio.h>
#include <string.h>

#define CONFIG_PATH "build/config_module_trunc_test.yaml"
#define LOG_PATH "logs/module_trunc_test.log"

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
            "  format: '[%%module] %%msg'\n"
            "  console_enable: false\n"
            "  file_enable: true\n"
            "  file_path: %s\n"
            "  socket_enable: false\n",
            LOG_PATH);
    fclose(f);
    return 0;
}

int main(void)
{
    char   big[5000];
    char   module[64];
    FILE  *f;
    char   buf[6000];
    size_t n;
    size_t i;

    remove(LOG_PATH);
    if (write_config() != 0 || log_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "init failed\n");
        return 1;
    }

    log_set_module("auth");
    log_get_module(module, sizeof(module));
    if (strcmp(module, "auth") != 0) {
        fprintf(stderr, "module getter mismatch: %s\n", module);
        log_destroy();
        return 1;
    }

    for (i = 0; i < sizeof(big) - 1; i++) {
        big[i] = 'A';
    }
    big[sizeof(big) - 1] = '\0';

    LOG_INFO("%s", big);
    log_flush();
    log_destroy();

    f = fopen(LOG_PATH, "rb");
    if (!f) {
        fprintf(stderr, "missing log file\n");
        return 1;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    if (!strstr(buf, "[auth]")) {
        fprintf(stderr, "module token missing:\n%s\n", buf);
        return 1;
    }
    if (!strstr(buf, "...")) {
        fprintf(stderr, "truncation marker missing:\n%s\n", buf);
        return 1;
    }

    printf("module and truncation test passed\n");
    return 0;
}
