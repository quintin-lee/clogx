/**
 * @file test_json_formatter.c
 * @brief Regression test: JSON structured logging format output.
 */

#include "log.h"
#include "log_formatter.h"
#include "log_internal.h"
#include "log_limits.h"
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

static void test_json_truncation_stays_valid(void)
{
    /* A string field that escapes to far more than CLOG_MAX_FORMATTED_SIZE
     * must not silently drop the whole line: the renderer closes the object
     * with a valid `"}` suffix instead of returning -1. The record is shaped
     * so the escaping breaks with exactly 3 bytes of buffer left (enough for
     * the closing suffix) — deterministic for any run. */
    remove("logs/json_trunc_test.log");

    log_config_t cfg   = {0};
    cfg.level          = LOG_LEVEL_INFO;
    cfg.console_enable = 0;
    cfg.file_enable    = 1;
    snprintf(cfg.file_path, sizeof(cfg.file_path), "logs/json_trunc_test.log");
    cfg.format      = "json";
    cfg.time_format = "%Y-%m-%d %H:%M:%S";

    logger_t *logger = logger_create_from_config(&cfg);
    assert(logger != NULL);

    char big[CLOG_MAX_MESSAGE_SIZE * 2];
    for (size_t i = 0; i < sizeof(big) - 1; i++) {
        big[i] = (char)0x01; /* escapes to 6 bytes each — far past the buffer */
    }
    big[sizeof(big) - 1] = '\0';

    /* Prefix before the file field is 77 bytes (fixed): 14 timestamp label +
     * 26 time + 11 level label + 4 "INFO" + 12 module label + 10 file label.
     * 8192 - 77 = 8115, and 8115 % 6 == 3 → the \u0001 escape loop breaks
     * with exactly 3 bytes free: room for the `"}` suffix but nothing else. */
    log_record_t rec = {0};
    rec.level        = LOG_LEVEL_INFO;
    rec.timestamp    = 1700000000000000ULL;
    rec.file         = big; /* overflows the line buffer */
    rec.line         = 0;
    rec.tid          = 1;
    rec.pid          = 1;
    rec.message      = "x";

    char buf[CLOG_MAX_FORMATTED_SIZE];
    int  len = log_formatter_format_for(logger, &rec, buf, sizeof(buf));

    assert(len > 0); /* line was NOT silently dropped */
    assert(buf[0] == '{');
    assert(len >= 3);
    assert(strcmp(buf + len - 2, "\"}") == 0); /* closed string + object brace */

    /* Pipeline smoke check: an overlong LOGGER_INFO message must produce
     * either a valid JSON line or no line at all — never invalid JSON. */
    char bigmsg[CLOG_MAX_MESSAGE_SIZE];
    for (size_t i = 0; i < sizeof(bigmsg) - 1; i++) {
        bigmsg[i] = (i % 2) ? '"' : (char)0x01; /* mixes \" and \u0001 escapes */
    }
    bigmsg[sizeof(bigmsg) - 1] = '\0';
    LOGGER_INFO(logger, "%s", bigmsg);
    logger_flush(logger);
    logger_destroy(logger);

    FILE *f = fopen("logs/json_trunc_test.log", "rb");
    if (f) {
        char   out[CLOG_MAX_FORMATTED_SIZE + 16];
        size_t n = fread(out, 1, sizeof(out) - 1, f);
        fclose(f);
        out[n] = '\0';
        if (n > 0 && out[n - 1] == '\n') {
            out[--n] = '\0';
        }
        if (n > 0) {
            assert(out[0] == '{');
            assert(n >= 3);
            assert(strcmp(out + n - 2, "\"}") == 0);
        }
    }

    printf("test_json_truncation_stays_valid PASSED\n");
}

int main(void)
{
    test_timestamp_cache_formatting();
    test_format_cache_switch();
    test_json_truncation_stays_valid();

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
