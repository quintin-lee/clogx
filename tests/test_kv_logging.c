/**
 * @file test_kv_logging.c
 * @brief Unit tests for structured key-value (KV) logging API.
 */

#include "log.h"
#include "log_config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_kv_global_singleton(void)
{
    assert(log_init("config.yaml") == CLOG_OK);

    LOG_INFO_KV("User login", CLOG_KV_STR("user", "admin"), CLOG_KV_INT("code", 200));
    LOG_WARN_KV("High memory usage", CLOG_KV_FLOAT("usage_pct", 87.5), CLOG_KV_BOOL("alert", true));
    LOG_ERROR_KV("Payment failed",
                 CLOG_KV_UINT("order_id", 9876543210ULL),
                 CLOG_KV_STR("reason", "declined"));

    log_flush();
    log_destroy();
    printf("  test_kv_global_singleton PASSED\n");
}

static void test_kv_json_formatting(void)
{
    log_config_t cfg   = {0};
    cfg.level          = LOG_LEVEL_INFO;
    cfg.console_enable = 0;
    cfg.file_enable    = 1;
    snprintf(cfg.file_path, sizeof(cfg.file_path), "logs/test_kv_json.log");
    cfg.format = "json";

    logger_t *logger = logger_create_from_config(&cfg);
    assert(logger != NULL);

    LOGGER_INFO_KV(logger,
                   "User action",
                   CLOG_KV_STR("user", "alice"),
                   CLOG_KV_INT("id", 42),
                   CLOG_KV_UINT("bytes", 1024ULL),
                   CLOG_KV_FLOAT("rate", 12.34),
                   CLOG_KV_BOOL("active", true));

    logger_flush(logger);
    logger_destroy(logger);

    FILE *f = fopen("logs/test_kv_json.log", "r");
    assert(f != NULL);
    char line[1024];
    assert(fgets(line, sizeof(line), f) != NULL);
    fclose(f);

    assert(strstr(line, "\"message\":\"User action\"") != NULL);
    assert(strstr(line, "\"user\":\"alice\"") != NULL);
    assert(strstr(line, "\"id\":42") != NULL);
    assert(strstr(line, "\"bytes\":1024") != NULL);
    assert(strstr(line, "\"rate\":12.34") != NULL);
    assert(strstr(line, "\"active\":true") != NULL);

    printf("  test_kv_json_formatting PASSED\n");
}

static void test_kv_async_mode(void)
{
    remove("logs/test_kv_async.log"); /* append-mode sink accumulates across runs */
    log_config_t cfg   = {0};
    cfg.level          = LOG_LEVEL_INFO;
    cfg.console_enable = 0;
    cfg.file_enable    = 1;
    snprintf(cfg.file_path, sizeof(cfg.file_path), "logs/test_kv_async.log");
    cfg.format     = "json";
    cfg.async      = true;
    cfg.queue_size = 1024;

    logger_t *logger = logger_create_from_config(&cfg);
    assert(logger != NULL);

    for (int i = 0; i < 50; i++) {
        char val_buf[32];
        snprintf(val_buf, sizeof(val_buf), "val-%d", i);
        LOGGER_INFO_KV(logger, "async event", CLOG_KV_STR("key", val_buf), CLOG_KV_INT("seq", i));
    }

    logger_flush(logger);
    logger_destroy(logger);

    FILE *f = fopen("logs/test_kv_async.log", "r");
    assert(f != NULL);
    int  count = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "\"message\":\"async event\"")) {
            count++;
        }
    }
    fclose(f);
    assert(count == 50);

    printf("  test_kv_async_mode PASSED\n");
}

int main(void)
{
    printf("=== test_kv_logging ===\n");
    test_kv_global_singleton();
    test_kv_json_formatting();
    test_kv_async_mode();
    printf("=== all kv logging tests PASSED ===\n");
    return 0;
}
