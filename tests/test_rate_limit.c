/**
 * @file test_rate_limit.c
 * @brief Regression test: token-bucket rate limiter suppression and refill.
 */

#include "clog_port.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_PATH "build/config_rate_limit_test.yaml"
#define LOG_PATH "logs/rate_limit_test.log"

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
            "  format: \"[%%level] %%msg\\n\"\n"
            "  rate_limit_enable: true\n"
            "  rate_limit_max_per_sec: 10\n"
            "  rate_limit_burst: 5\n"
            "  console_enable: false\n"
            "  file_enable: true\n"
            "  file_path: %s\n"
            "  socket_enable: false\n",
            LOG_PATH);
    fclose(f);
    return 0;
}

static int count_lines(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    int  count = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        count++;
    }
    fclose(f);
    return count;
}

int main(void)
{
    remove(LOG_PATH);
    if (write_config() != 0 || log_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "rate limit test log_init failed\n");
        return 1;
    }

    /* Burst 5 logs rapidly */
    for (int i = 0; i < 20; i++) {
        LOG_INFO("rapid msg %d", i);
    }
    log_flush();

    int initial_lines = count_lines(LOG_PATH);
    if (initial_lines != 5) {
        fprintf(stderr, "expected 5 burst lines, got %d\n", initial_lines);
        return 1;
    }

    /* Wait 200ms to allow ~20 tokens to refill (max burst is 5) */
    clog_sleep_ms(200);

    /* Send another log -> should emit Suppressed warning + this log */
    LOG_INFO("after wait msg");
    log_flush();
    log_destroy();

    FILE *f = fopen(LOG_PATH, "rb");
    if (!f) {
        fprintf(stderr, "missing log file\n");
        return 1;
    }

    char   buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    if (!strstr(buf, "Suppressed 15 log messages due to rate limit")) {
        fprintf(stderr, "suppressed notice missing: %s\n", buf);
        return 1;
    }

    printf("rate limit test passed\n");
    return 0;
}
