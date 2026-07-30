#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "dispatcher.h"
#include "log.h"
#include "log_config.h"
#include "log_sink.h"

static int g_sink_write_count;

/* Minimal test sink that counts writes (receives formatted buffer). */
static int test_sink_write(log_sink_t *sink, const char *buf, size_t len) {
    (void)sink;
    (void)buf;
    g_sink_write_count++;
    return (int)len;
}

static void test_sink_flush(log_sink_t *sink) {
    (void)sink;
}

static void test_sink_destroy(log_sink_t *sink) {
    free(sink);
}

static log_sink_t *make_test_sink(void) {
    log_sink_t *sink = (log_sink_t *)calloc(1, sizeof(log_sink_t));
    assert(sink != NULL);
    sink->write = test_sink_write;
    sink->flush = test_sink_flush;
    sink->destroy = test_sink_destroy;
    return sink;
}

/* ── NULL / error paths ── */

static void test_create_null(void) {
    logger_t *logger = logger_create(NULL);
    /* Default config enables console, so this should succeed. */
    assert(logger != NULL);
    logger_destroy(logger);
    printf("test_create_null PASSED\n");
}

static void test_create_from_config_null(void) {
    logger_t *logger = logger_create_from_config(NULL);
    assert(logger == NULL);
    printf("test_create_from_config_null PASSED\n");
}

static void test_destroy_null(void) {
    logger_destroy(NULL); /* must be a no-op */
    printf("test_destroy_null PASSED\n");
}

/* ── Instance creation from config fails with no sinks ── */

static void test_create_no_sinks_fails(void) {
    log_config_t cfg = {0};
    cfg.level = LOG_LEVEL_INFO;
    cfg.async = false;
    cfg.console_enable = false;
    cfg.file_enable = false;
    cfg.socket_enable = false;

    logger_t *logger = logger_create_from_config(&cfg);
    /* No sinks configured → init fails */
    assert(logger == NULL);
    printf("test_create_no_sinks_fails PASSED (no-sinks expected to fail)\n");
}

/* ── Instance level isolation ── */

static void test_level_isolation(void) {
    log_config_t cfg = {0};
    cfg.level = LOG_LEVEL_INFO;
    cfg.async = false;
    cfg.console_enable = true;
    cfg.console_stderr = true;
    cfg.color = false;

    logger_t *info_logger = logger_create_from_config(&cfg);
    assert(info_logger != NULL);
    assert(logger_get_level(info_logger) == LOG_LEVEL_INFO);

    cfg.level = LOG_LEVEL_ERROR;
    logger_t *error_logger = logger_create_from_config(&cfg);
    assert(error_logger != NULL);
    assert(logger_get_level(error_logger) == LOG_LEVEL_ERROR);

    /* Verify isolation */
    assert(logger_get_level(info_logger) == LOG_LEVEL_INFO);
    assert(logger_get_level(error_logger) == LOG_LEVEL_ERROR);

    logger_destroy(info_logger);
    logger_destroy(error_logger);
    printf("test_level_isolation PASSED\n");
}

/* ── Instance module isolation ── */

static void test_module_isolation(void) {
    log_config_t cfg = {0};
    cfg.level = LOG_LEVEL_TRACE;
    cfg.async = false;
    cfg.console_enable = true;
    cfg.console_stderr = true;
    cfg.color = false;

    logger_t *a = logger_create_from_config(&cfg);
    logger_t *b = logger_create_from_config(&cfg);
    assert(a && b);

    logger_set_module(a, "module_a");
    logger_set_module(b, "module_b");

    char buf_a[64], buf_b[64];
    logger_get_module(a, buf_a, sizeof(buf_a));
    logger_get_module(b, buf_b, sizeof(buf_b));
    assert(strcmp(buf_a, "module_a") == 0);
    assert(strcmp(buf_b, "module_b") == 0);

    logger_destroy(a);
    logger_destroy(b);
    printf("test_module_isolation PASSED\n");
}

/* ── Basic instance logging ── */

static void test_instance_logging(void) {
    g_sink_write_count = 0;

    logger_t *logger = logger_create_from_config(NULL);
    assert(logger == NULL);

    /* Create a logger with a custom sink so we can count writes. */
    log_config_t cfg = {0};
    cfg.level = LOG_LEVEL_TRACE;
    cfg.async = false;
    cfg.console_enable = false;
    cfg.file_enable = false;
    cfg.socket_enable = false;

    logger = logger_create_from_config(&cfg);
    assert(logger == NULL); /* no sinks → fails */

    /* Now create with a test sink via log_dispatcher_add_sink_for usage.
       We need to create the logger with some enabled sink first.
       Use console to stderr, then add a custom sink. */
    cfg.console_enable = true;
    cfg.console_stderr = true;
    logger = logger_create_from_config(&cfg);
    assert(logger != NULL);

    /* Add a custom sink */
    log_sink_t *sink = make_test_sink();
    int ret = logger_add_sink(logger, sink);
    assert(ret == CLOG_OK);

    /* Log a few messages */
    LOGGER_TRACE(logger, "trace msg");
    LOGGER_INFO(logger, "info msg");
    LOGGER_ERROR(logger, "error msg");
    logger_flush(logger);

    /* Custom sink write is called twice per record (message + newline). */
    assert(g_sink_write_count == 6);

    LOGGER_FATAL(logger, "fatal msg");
    logger_flush(logger);
    assert(g_sink_write_count == 8);

    logger_destroy(logger);
    printf("test_instance_logging PASSED (%d writes)\n", g_sink_write_count);
}

/* ── Instance-level config set/get ── */

static void test_config_set_get(void) {
    log_config_t cfg = {0};
    cfg.level = LOG_LEVEL_WARN;
    cfg.async = false;
    cfg.console_enable = true;
    cfg.console_stderr = true;
    cfg.color = false;

    logger_t *logger = logger_create_from_config(&cfg);
    assert(logger != NULL);
    assert(logger_get_level(logger) == LOG_LEVEL_WARN);

    /* Change level */
    logger_set_level(logger, LOG_LEVEL_DEBUG);
    assert(logger_get_level(logger) == LOG_LEVEL_DEBUG);

    /* Get and verify config pointer */
    log_config_t *got = logger_config_get(logger);
    assert(got != NULL);
    assert(got->level == LOG_LEVEL_DEBUG);

    /* Set config */
    log_config_t new_cfg = *got;
    new_cfg.level = LOG_LEVEL_FATAL;
    new_cfg.format = "[%level] %msg";
    int ret = logger_config_set(logger, &new_cfg);
    assert(ret == 0);
    assert(logger_get_level(logger) == LOG_LEVEL_FATAL);

    logger_destroy(logger);
    printf("test_config_set_get PASSED\n");
}

/* ── Stats ── */

static void test_stats(void) {
    g_sink_write_count = 0;

    log_config_t cfg = {0};
    cfg.level = LOG_LEVEL_TRACE;
    cfg.async = false;
    cfg.console_enable = true;
    cfg.console_stderr = true;
    cfg.color = false;

    logger_t *logger = logger_create_from_config(&cfg);
    assert(logger != NULL);

    log_sink_t *sink = make_test_sink();
    assert(logger_add_sink(logger, sink) == CLOG_OK);

    LOGGER_INFO(logger, "stats-1");
    LOGGER_INFO(logger, "stats-2");
    LOGGER_INFO(logger, "stats-3");
    logger_flush(logger);

    log_stats_t stats;
    logger_get_stats(logger, &stats);
    assert(stats.total_logged_count >= 3);
    /* Queue depth should be 0 in sync mode after flush */
    assert(stats.current_queue_depth == 0);

    logger_destroy(logger);
    printf("test_stats PASSED (logged=%llu)\n", (unsigned long long)stats.total_logged_count);
}

/* ── Default logger still works ── */

static void test_default_logger_unaffected(void) {
    /* Just verify we can call log_init/log_destroy normally
       (other tests cover this in detail). */
    /* Destroy and re-init the default logger (it was destroyed above) */
    int ret = log_init(NULL);
    assert(ret == 0 || ret == CLOG_ERR_NO_SINKS);

    if (ret == 0) {
        LOG_INFO("default logger still works after instance tests");
        log_destroy();
    }
    printf("test_default_logger_unaffected PASSED\n");
}

static void test_null_logger_error_paths(void) {
    logger_set_level(NULL, LOG_LEVEL_INFO);
    assert(logger_get_level(NULL) == LOG_LEVEL_INFO);

    logger_set_module(NULL, "test");
    char mod[32] = {0};
    logger_get_module(NULL, mod, sizeof(mod));
    assert(mod[0] == '\0');

    assert(logger_config_get(NULL) == NULL);
    assert(logger_config_set(NULL, NULL) == CLOG_ERR_INVALID_ARG);

    log_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    logger_get_stats(NULL, &stats);

    assert(logger_add_sink(NULL, NULL) == CLOG_ERR_INVALID_ARG);
    assert(logger_remove_sink(NULL, NULL) == CLOG_ERR_INVALID_ARG);

    logger_flush(NULL);
    logger_writevprintf(NULL, LOG_LEVEL_INFO, "file", 10, "func", "msg");

    printf("test_null_logger_error_paths PASSED\n");
}

static void test_dispatcher_multi_instance_gaps(void) {
    log_config_t cfg = {0};
    cfg.level = LOG_LEVEL_WARN;
    cfg.async = false;
    cfg.console_enable = true;
    cfg.console_stderr = true;
    cfg.color = true;

    logger_t *logger = logger_create_from_config(&cfg);
    assert(logger != NULL);

    log_record_t trace_rec = {0};
    trace_rec.level = LOG_LEVEL_TRACE;
    log_dispatcher_dispatch_for(logger, &trace_rec);

    log_record_t warn_rec = {0};
    warn_rec.level = LOG_LEVEL_WARN;
    warn_rec.message = "warn test";
    log_dispatcher_dispatch_for(logger, &warn_rec);

    log_sink_t *s1 = make_test_sink();
    log_sink_t *s2 = make_test_sink();
    assert(logger_add_sink(logger, s1) == CLOG_OK);
    assert(logger_add_sink(logger, s2) == CLOG_OK);

    assert(logger_remove_sink(logger, s1) == CLOG_OK);
    assert(logger_remove_sink(logger, s2) == CLOG_OK);
    s1->destroy(s1);
    s2->destroy(s2);

    logger_destroy(logger);
    printf("test_dispatcher_multi_instance_gaps PASSED\n");
}

int main(void) {
    printf("=== multi-instance tests ===\n");

    test_dispatcher_multi_instance_gaps();
    test_null_logger_error_paths();
    test_create_null();
    test_create_from_config_null();
    test_destroy_null();
    test_create_no_sinks_fails();
    test_level_isolation();
    test_module_isolation();
    test_instance_logging();
    test_config_set_get();
    test_stats();

    /* Default logger test must come last since other tests
       don't clean up the default logger. */
    test_default_logger_unaffected();

    printf("=== all multi-instance tests PASSED ===\n");
    return 0;
}
