/**
 * @file test_json_formatter.c
 * @brief Regression test: JSON structured logging format output.
 */

#include "log.h"
#include "log_formatter.h"
#include "log_internal.h"
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

#include <assert.h>

static void test_timestamp_cache_formatting(void)
{
    log_record_t rec = {0};
    rec.level        = LOG_LEVEL_INFO;
    rec.timestamp    = 1700000000000000ULL; /* fixed epoch time in microseconds */
    rec.message      = "cache time test";
    rec.module       = "main";
    rec.file         = "test.c";
    rec.line         = 10;
    rec.func         = "test_fn";

    char buf1[256];
    char buf2[256];

    logger_t logger      = {0};
    logger.config.level  = LOG_LEVEL_DEBUG;
    logger.config.format = "[%time] %msg";

    int len1 = log_formatter_format_for(&logger, &rec, buf1, sizeof(buf1));
    rec.timestamp += 500ULL; /* same second, +500 us */
    int len2 = log_formatter_format_for(&logger, &rec, buf2, sizeof(buf2));

    assert(len1 > 0 && len2 > 0);
    assert(strncmp(buf1, buf2, 20) == 0); /* date and second portion must match */
    printf("test_timestamp_cache_formatting PASSED\n");
}

static void test_format_cache_switch(void)
{
    /* Warm the per-thread compiled-format cache with format A, then switch
     * formats and verify the cache recompiles (output must follow the new
     * format every time). */
    log_record_t rec = {0};
    rec.level        = LOG_LEVEL_INFO;
    rec.timestamp    = 1700000000000000ULL; /* fixed epoch time in microseconds */
    rec.message      = "switch test";

    logger_t logger_a     = {0};
    logger_t logger_b     = {0};
    logger_a.config.level = LOG_LEVEL_DEBUG;
    logger_b.config.level = LOG_LEVEL_DEBUG;
    log_formatter_init_for(&logger_a, "[%time] %msg", NULL);
    log_formatter_init_for(&logger_b, "%level|%msg", NULL);

    char buf[256];

    int len1 = log_formatter_format_for(&logger_a, &rec, buf, sizeof(buf));
    assert(len1 > 0);
    assert(strstr(buf, "] ") != NULL); /* format A shape */
    assert(strstr(buf, "|") == NULL);  /* format B marker absent */

    int len2 = log_formatter_format_for(&logger_b, &rec, buf, sizeof(buf));
    assert(len2 > 0);
    assert(strcmp(buf, "INFO|switch test") == 0);

    int len3 = log_formatter_format_for(&logger_a, &rec, buf, sizeof(buf));
    assert(len3 > 0);
    assert(strstr(buf, "] ") != NULL && strstr(buf, "switch test") != NULL);

    /* Reload logger_a with a third format; cache must follow. */
    log_formatter_init_for(&logger_a, "%level|%file:%line", NULL);
    int len4 = log_formatter_format_for(&logger_a, &rec, buf, sizeof(buf));
    assert(len4 > 0);
    assert(strcmp(buf, "INFO|(unknown):0") == 0);

    printf("test_format_cache_switch PASSED\n");
}

int main(void)
{
    test_timestamp_cache_formatting();
    test_format_cache_switch();

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
