#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dispatcher.h"
#include "log.h"
#include "log_async.h"
#include "log_config.h"
#include "log_internal.h"
#include "log_record.h"
#include "log_signal.h"
#include "log_sink.h"

static int g_fb_count;
static void inc_fallback(void) {
    g_fb_count++;
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

/* ── T1: log_strerror for missing error codes ── */
static void test_strerror_missing_codes(void) {
    const char *s;
    s = log_strerror(CLOG_ERR_CONFIG_OPEN);
    if (!s || strcmp(s, "unknown error") == 0) {
        fprintf(stderr, "FAIL: CLOG_ERR_CONFIG_OPEN string\n");
        exit(1);
    }
    s = log_strerror(CLOG_ERR_NO_SINKS);
    if (!s || strcmp(s, "unknown error") == 0) {
        fprintf(stderr, "FAIL: CLOG_ERR_NO_SINKS string\n");
        exit(1);
    }
    s = log_strerror(CLOG_ERR_FILE_WRITE);
    if (!s || strcmp(s, "unknown error") == 0) {
        fprintf(stderr, "FAIL: CLOG_ERR_FILE_WRITE string\n");
        exit(1);
    }
    s = log_strerror(CLOG_ERR_QUEUE_FULL);
    if (!s || strcmp(s, "unknown error") == 0) {
        fprintf(stderr, "FAIL: CLOG_ERR_QUEUE_FULL string\n");
        exit(1);
    }
    printf("test_strerror_missing_codes PASSED\n");
}

/* ── T2: log_get_async_fallback_cb (set then get) ── */
static void test_get_async_fallback_cb(void) {
    log_set_async_fallback_cb(inc_fallback);
    void (*got)(void) = log_get_async_fallback_cb();
    if (got != inc_fallback) {
        fprintf(stderr, "FAIL: get returned wrong cb\n");
        exit(1);
    }
    log_set_async_fallback_cb(NULL);
    printf("test_get_async_fallback_cb PASSED\n");
}

/* ── T3: logger_init_internal line 142 — bad YAML → CLOG_ERR_CONFIG_OPEN ── */
static void test_init_config_parse_fail(void) {
    int ret = log_init("tests/config/coverage_bad.yaml");
    if (ret != CLOG_ERR_CONFIG_OPEN) {
        fprintf(stderr, "FAIL: expected CONFIG_OPEN, got %d\n", ret);
        exit(1);
    }
    printf("test_init_config_parse_fail PASSED\n");
}

/* ── T4: logger_init_internal line 148 — no sinks → CLOG_ERR_NO_SINKS ── */
static void test_init_no_sinks(void) {
    int ret = log_init("tests/config/coverage_no_sinks.yaml");
    if (ret != CLOG_ERR_NO_SINKS) {
        fprintf(stderr, "FAIL: expected NO_SINKS, got %d\n", ret);
        exit(1);
    }
    printf("test_init_no_sinks PASSED\n");
}

/* ── T5: logger_writevprintf_internal lines 241-245 — async fallback ── */
static void test_async_write_fallback(void) {
    log_config_t cfg = {0};
    cfg.level = LOG_LEVEL_TRACE;
    cfg.async = true;
    cfg.queue_size = 64;
    cfg.console_enable = true;
    cfg.console_stderr = true;

    if (log_init(NULL) != 0) {
        fprintf(stderr, "FAIL: log_init for T5\n");
        exit(1);
    }
    log_config_set(&cfg);

    g_fb_count = 0;
    log_set_async_fallback_cb(inc_fallback);
    log_async_shutdown();

    LOG_INFO("fallback test");
    LOG_INFO("fallback test 2");

    if (g_fb_count != 2) {
        fprintf(stderr, "FAIL: expected 2 fallback calls, got %d\n", g_fb_count);
        exit(1);
    }
    log_set_async_fallback_cb(NULL);
    log_destroy();
    printf("test_async_write_fallback PASSED\n");
}

/* ── T6: log_set_thread_context line 298 — update existing key ── */
static void test_set_thread_context_update(void) {
    log_set_thread_context("t6key", "first");
    log_set_thread_context("t6key", "second");
    const char *v = log_get_thread_context("t6key");
    if (!v || strcmp(v, "second") != 0) {
        fprintf(stderr, "FAIL: expected 'second', got '%s'\n", v ? v : "NULL");
        exit(1);
    }
    log_clear_thread_context();
    printf("test_set_thread_context_update PASSED\n");
}

/* ── T7: logger_writevprintf_internal line 173-174 — signal in write path ── */
static void test_signal_processing_in_write(void) {
    /* Replace SIGTERM handler with SIG_IGN so the re-raise is a no-op. */
    typedef void (*sigh_t)(int);
    sigh_t old = signal(SIGTERM, SIG_IGN);

    write_file("build/gap_signal.yaml", "log:\n"
                                        "  level: TRACE\n"
                                        "  async: false\n"
                                        "  console_enable: true\n"
                                        "  console_stderr: true\n"
                                        "  catch_signals: true\n");

    if (log_init("build/gap_signal.yaml") != 0) {
        fprintf(stderr, "FAIL: log_init for T7\n");
        exit(1);
    }

    kill(getpid(), SIGTERM);

    LOG_INFO("signal-in-write test");

    log_destroy();
    if (old != SIG_ERR)
        signal(SIGTERM, old);
    printf("test_signal_processing_in_write PASSED\n");
}

/* ── T8: logger_reload (lines 555-581) ── */
static void test_logger_reload(void) {
    int ret;

    ret = logger_reload(NULL);
    if (ret != CLOG_ERR_RELOAD) {
        fprintf(stderr, "FAIL: NULL reload expected RELOAD, got %d\n", ret);
        exit(1);
    }

    logger_t *logger = logger_create(NULL);
    if (!logger) {
        fprintf(stderr, "FAIL: logger_create NULL for T8\n");
        exit(1);
    }
    ret = logger_reload(logger);
    if (ret != CLOG_OK) {
        fprintf(stderr, "FAIL: reload empty path expected OK, got %d\n", ret);
        exit(1);
    }

    /* Reload with a live config file (config_path was set by log_init — not here,
       so this just verifies the happy path through logger_reload). */
    logger_destroy(logger);

    /* Also exercise the default-logger reload path. */
    write_file("build/gap_reload.yaml", "log:\n"
                                        "  level: TRACE\n"
                                        "  async: false\n"
                                        "  console_enable: true\n"
                                        "  console_stderr: true\n");

    if (log_init("build/gap_reload.yaml") != 0) {
        fprintf(stderr, "FAIL: log_init for T8\n");
        exit(1);
    }
    ret = logger_reload(&g_default_logger);
    if (ret != CLOG_OK) {
        fprintf(stderr, "FAIL: reload good config expected OK, got %d\n", ret);
        exit(1);
    }
    log_destroy();

    printf("test_logger_reload PASSED\n");
}

/* ── T9: logger_create_from_config format/time_format (lines 489-495) ── */
static void test_logger_create_from_config_format(void) {
    log_config_t cfg = {0};
    cfg.level = LOG_LEVEL_TRACE;
    cfg.async = false;
    cfg.console_enable = true;
    cfg.console_stderr = true;
    cfg.format = "[%level] %msg";
    cfg.time_format = "%H:%M:%S";

    logger_t *logger = logger_create_from_config(&cfg);
    if (!logger) {
        fprintf(stderr, "FAIL: create_from_config with format\n");
        exit(1);
    }
    logger_destroy(logger);

    printf("test_logger_create_from_config_format PASSED\n");
}

int main(void) {
    printf("=== coverage-gap tests ===\n");

    test_strerror_missing_codes();
    test_get_async_fallback_cb();
    test_init_config_parse_fail();
    test_init_no_sinks();
    test_async_write_fallback();
    test_set_thread_context_update();
    test_signal_processing_in_write();
    test_logger_reload();
    test_logger_create_from_config_format();

    printf("=== all coverage-gap tests PASSED ===\n");
    return 0;
}
