/**
 * @file test_redact.c
 * @brief Unit tests for record-time redaction: add/clear/count + hooks.
 */

#include "clog_port.h"
#include "log.h"
#include "log_config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static logger_t *make_json_file_logger(const char *path)
{
    log_config_t cfg;

    remove(path);
    memset(&cfg, 0, sizeof(cfg));
    cfg.level          = LOG_LEVEL_INFO;
    cfg.console_enable = 0;
    cfg.file_enable    = 1;
    snprintf(cfg.file_path, sizeof(cfg.file_path), "%s", path);
    cfg.format = "json";
    return logger_create_from_config(&cfg);
}

static void read_whole(const char *path, char *buf, size_t buf_size)
{
    FILE  *f;
    size_t n;

    f = fopen(path, "r");
    assert(f != NULL);
    n = fread(buf, 1, buf_size - 1, f);
    fclose(f);
    buf[n] = '\0';
}

static void test_default_mask(void)
{
    logger_t *logger;
    char      out[4096];

    clog_redact_clear();
    assert(clog_redact_add("secret123", NULL) == CLOG_OK);
    assert(clog_redact_count() == 1);

    logger = make_json_file_logger("logs/test_redact_default.log");
    assert(logger != NULL);
    LOGGER_INFO(logger, "token=secret123 end");
    logger_flush(logger);
    logger_destroy(logger);

    read_whole("logs/test_redact_default.log", out, sizeof(out));
    assert(strstr(out, "***") != NULL);
    assert(strstr(out, "secret123") == NULL);
    clog_redact_clear();
    printf("test_default_mask passed\n");
}

static void test_custom_mask(void)
{
    logger_t *logger;
    char      out[4096];

    clog_redact_clear();
    assert(clog_redact_add("password", "[HIDDEN]") == CLOG_OK);

    logger = make_json_file_logger("logs/test_redact_custom.log");
    assert(logger != NULL);
    LOGGER_INFO(logger, "login password=abc");
    logger_flush(logger);
    logger_destroy(logger);

    read_whole("logs/test_redact_custom.log", out, sizeof(out));
    assert(strstr(out, "[HIDDEN]") != NULL);
    assert(strstr(out, "password") == NULL);
    clog_redact_clear();
    printf("test_custom_mask passed\n");
}

static void test_rule_order(void)
{
    logger_t *logger;
    char      out[4096];

    clog_redact_clear();
    assert(clog_redact_add("ab", "X") == CLOG_OK);
    assert(clog_redact_add("X", "Y") == CLOG_OK);

    logger = make_json_file_logger("logs/test_redact_order.log");
    assert(logger != NULL);
    LOGGER_INFO(logger, "ab");
    logger_flush(logger);
    logger_destroy(logger);

    read_whole("logs/test_redact_order.log", out, sizeof(out));
    assert(strstr(out, "\"message\":\"Y\"") != NULL);
    clog_redact_clear();
    printf("test_rule_order passed\n");
}

static void test_no_rule_passthrough(void)
{
    logger_t *logger;
    char      out[4096];

    clog_redact_clear();
    assert(clog_redact_count() == 0);

    logger = make_json_file_logger("logs/test_redact_passthrough.log");
    assert(logger != NULL);
    LOGGER_INFO(logger, "token=secret123 end");
    logger_flush(logger);
    logger_destroy(logger);

    read_whole("logs/test_redact_passthrough.log", out, sizeof(out));
    assert(strstr(out, "secret123") != NULL);
    printf("test_no_rule_passthrough passed\n");
}

static void test_kv_str_redacted(void)
{
    logger_t *logger;
    char      out[4096];

    clog_redact_clear();
    assert(clog_redact_add("secret123", NULL) == CLOG_OK);

    logger = make_json_file_logger("logs/test_redact_kv.log");
    assert(logger != NULL);
    LOGGER_INFO_KV(logger, "login", CLOG_KV_STR("token", "secret123"));
    logger_flush(logger);
    logger_destroy(logger);

    read_whole("logs/test_redact_kv.log", out, sizeof(out));
    assert(strstr(out, "\"token\":\"***\"") != NULL);
    assert(strstr(out, "secret123") == NULL);
    clog_redact_clear();
    printf("test_kv_str_redacted passed\n");
}

static void test_kv_non_str_untouched(void)
{
    logger_t *logger;
    char      out[4096];

    clog_redact_clear();
    assert(clog_redact_add("secret123", NULL) == CLOG_OK);

    logger = make_json_file_logger("logs/test_redact_kvint.log");
    assert(logger != NULL);
    LOGGER_INFO_KV(logger, "counted", CLOG_KV_INT("code", 200));
    logger_flush(logger);
    logger_destroy(logger);

    read_whole("logs/test_redact_kvint.log", out, sizeof(out));
    assert(strstr(out, "\"code\":200") != NULL);
    clog_redact_clear();
    printf("test_kv_non_str_untouched passed\n");
}

static void test_kv_redact_short_str(void)
{
    logger_t *logger;
    char      out[4096];

    clog_redact_clear();
    assert(clog_redact_add("secret123", NULL) == CLOG_OK);

    logger = make_json_file_logger("logs/test_redact_kv_short.log");
    assert(logger != NULL);
    LOGGER_INFO_KV(logger, "user login", CLOG_KV_STR("token", "abc secret123 xyz"));
    logger_flush(logger);
    logger_destroy(logger);

    read_whole("logs/test_redact_kv_short.log", out, sizeof(out));
    assert(strstr(out, "secret123") == NULL);
    assert(strstr(out, "***") != NULL);
    clog_redact_clear();
    printf("test_kv_redact_short_str passed\n");
}

static void test_kv_redact_multi_rule(void)
{
    logger_t *logger;
    char      out[4096];
    char      msg[256];

    clog_redact_clear();
    assert(clog_redact_add("alpha", "[A]") == CLOG_OK);
    assert(clog_redact_add("beta", "[B]") == CLOG_OK);

    snprintf(msg, sizeof(msg), "alpha and beta");
    logger = make_json_file_logger("logs/test_redact_kv_multi.log");
    assert(logger != NULL);
    LOGGER_INFO_KV(logger, msg, CLOG_KV_STR("k", "v"));
    logger_flush(logger);
    logger_destroy(logger);

    read_whole("logs/test_redact_kv_multi.log", out, sizeof(out));
    assert(strstr(out, "alpha") == NULL);
    assert(strstr(out, "beta") == NULL);
    assert(strstr(out, "[A]") != NULL);
    assert(strstr(out, "[B]") != NULL);
    clog_redact_clear();
    printf("test_kv_redact_multi_rule passed\n");
}

static void test_kv_redact_long_value(void)
{
    /* 600B value forces the heap-fallback path — output must stay exact. */
    static char big[640];
    logger_t   *logger;
    char        out[4096];

    memset(big, 'x', 600);
    memcpy(big + 600, "secret123", 9);
    big[609] = '\0';

    clog_redact_clear();
    assert(clog_redact_add("secret123", NULL) == CLOG_OK);

    logger = make_json_file_logger("logs/test_redact_kv_long.log");
    assert(logger != NULL);
    LOGGER_INFO_KV(logger, "big value", CLOG_KV_STR("blob", big));
    logger_flush(logger);
    logger_destroy(logger);

    read_whole("logs/test_redact_kv_long.log", out, sizeof(out));
    assert(strstr(out, "secret123") == NULL);
    assert(strstr(out, "***") != NULL);
    clog_redact_clear();
    printf("test_kv_redact_long_value passed\n");
}

static void test_invalid_add_rejected(void)
{
    clog_redact_clear();
    assert(clog_redact_add(NULL, NULL) == CLOG_ERR_INVALID_ARG);
    assert(clog_redact_add("", NULL) == CLOG_ERR_INVALID_ARG);
    assert(clog_redact_count() == 0);
    printf("test_invalid_add_rejected passed\n");
}

static void test_table_full_rejected(void)
{
    char pattern[64];
    int  i;

    clog_redact_clear();
    for (i = 0; i < 16; i++) {
        snprintf(pattern, sizeof(pattern), "pattern-%d-zz", i);
        assert(clog_redact_add(pattern, NULL) == CLOG_OK);
    }
    assert(clog_redact_count() == 16);
    assert(clog_redact_add("one-too-many", NULL) == CLOG_ERR_INVALID_ARG);
    assert(clog_redact_count() == 16);
    clog_redact_clear();
    assert(clog_redact_count() == 0);
    printf("test_table_full_rejected passed\n");
}

int main(void)
{
    test_default_mask();
    test_custom_mask();
    test_rule_order();
    test_no_rule_passthrough();
    test_kv_str_redacted();
    test_kv_non_str_untouched();
    test_kv_redact_short_str();
    test_kv_redact_multi_rule();
    test_kv_redact_long_value();
    test_invalid_add_rejected();
    test_table_full_rejected();
    printf("all redact tests passed!\n");
    return 0;
}
