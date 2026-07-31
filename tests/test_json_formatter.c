/**
 * @file test_json_formatter.c
 * @brief Regression test: JSON structured logging format output.
 */

#include "log.h"
#include "log_formatter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_PATH "build/config_json_test.yaml"
#define LOG_PATH "logs/json_test.log"

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
            "  format: json\n"
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
    remove(LOG_PATH);
    if (write_config() != 0 || log_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "json test log_init failed\n");
        return 1;
    }

    log_set_module("test_mod");
    LOG_INFO("hello \"quoted\" and \\slash and \nnewline");
    log_flush();
    log_destroy();

    FILE *f = fopen(LOG_PATH, "rb");
    if (!f) {
        fprintf(stderr, "missing json log file\n");
        return 1;
    }

    char   buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    if (!strstr(buf, "{\"timestamp\":")) {
        fprintf(stderr, "json timestamp missing: %s\n", buf);
        return 1;
    }
    if (!strstr(buf, "\"level\":\"INFO\"")) {
        fprintf(stderr, "json level missing: %s\n", buf);
        return 1;
    }
    if (!strstr(buf, "\"module\":\"test_mod\"")) {
        fprintf(stderr, "json module missing: %s\n", buf);
        return 1;
    }
    if (!strstr(buf, "hello \\\"quoted\\\" and \\\\slash and \\nnewline")) {
        fprintf(stderr, "json message escaping failed: %s\n", buf);
        return 1;
    }

    printf("json formatter test passed\n");
    return 0;
}
