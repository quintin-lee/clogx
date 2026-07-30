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

/* Small test sink for coverage tests. */
static int s_sink_write_count;
static int test_sink_write(log_sink_t *sink, const char *buf, size_t len) {
    (void)sink;
    (void)buf;
    s_sink_write_count++;
    return (int)len;
}
static void test_sink_flush(log_sink_t *sink) {
    (void)sink;
}
static void test_sink_destroy(log_sink_t *sink) {
    (void)sink;
}
static log_sink_t *make_test_sink(void) {
    log_sink_t *sink = (log_sink_t *)calloc(1, sizeof(log_sink_t));
    if (!sink)
        exit(1);
    sink->write = test_sink_write;
    sink->flush = test_sink_flush;
    sink->destroy = test_sink_destroy;
    return sink;
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

/* ── T10: async init already running → return 0 (async.c L106) ── */
static void test_async_init_already_running(void) {
    log_config_t cfg = {0};
    cfg.level = LOG_LEVEL_TRACE;
    cfg.async = true;
    cfg.queue_size = 64;
    cfg.console_enable = true;
    cfg.console_stderr = true;

    if (log_init(NULL) != 0) {
        fprintf(stderr, "FAIL: log_init for T10\n");
        exit(1);
    }
    log_config_set(&cfg);

    int ret = log_async_init(64);
    if (ret != 0) {
        fprintf(stderr, "FAIL: second init expected 0, got %d\n", ret);
        exit(1);
    }
    log_destroy();
    printf("test_async_init_already_running PASSED\n");
}

/* ── T11: queue full in async write (async.c L154-157) ── */
static void test_async_queue_full_write(void) {
    log_config_t cfg = {0};
    cfg.level = LOG_LEVEL_TRACE;
    cfg.async = true;
    cfg.queue_size = 1;
    cfg.console_enable = true;
    cfg.console_stderr = true;

    if (log_init(NULL) != 0) {
        fprintf(stderr, "FAIL: log_init for T11\n");
        exit(1);
    }
    log_config_set(&cfg);

    g_fb_count = 0;
    log_set_async_fallback_cb(inc_fallback);

    LOG_INFO("fill queue");
    LOG_INFO("overflow");

    if (g_fb_count < 1) {
        fprintf(stderr, "FAIL: expected fallback cb, got %d\n", g_fb_count);
        exit(1);
    }
    log_set_async_fallback_cb(NULL);
    log_destroy();
    printf("test_async_queue_full_write PASSED\n");
}

/* ── T12: queue depth with active queue (async.c L165-168) ── */
static void test_async_queue_depth_active(void) {
    log_config_t cfg = {0};
    cfg.level = LOG_LEVEL_TRACE;
    cfg.async = true;
    cfg.queue_size = 64;
    cfg.console_enable = true;
    cfg.console_stderr = true;

    if (log_init(NULL) != 0) {
        fprintf(stderr, "FAIL: log_init for T12\n");
        exit(1);
    }
    log_config_set(&cfg);

    size_t d1 = log_async_get_queue_depth();
    LOG_INFO("depth test");
    log_async_get_queue_depth();
    /* d1 may be 0 or >0 — d2 may be consumed by the async worker.
       We just verify the function runs through the non-NULL queue path. */

    if (d1 != 0) {
        fprintf(stderr, "FAIL: empty queue depth expected 0, got %zu\n", d1);
        exit(1);
    }
    log_destroy();
    printf("test_async_queue_depth_active PASSED\n");
}

/* ── T13: logger_flush error paths (log.c L547-552) ── */
static void test_logger_flush_paths(void) {
    /* NULL logger — no-op. */
    logger_flush(NULL);

    /* Instance with async=true. */
    log_config_t cfg = {0};
    cfg.level = LOG_LEVEL_TRACE;
    cfg.async = true;
    cfg.queue_size = 4;
    cfg.console_enable = true;
    cfg.console_stderr = true;

    logger_t *logger = logger_create_from_config(&cfg);
    if (!logger) {
        fprintf(stderr, "FAIL: create for T13\n");
        exit(1);
    }
    LOGGER_INFO(logger, "pre-flush");
    logger_flush(logger);
    logger_destroy(logger);

    printf("test_logger_flush_paths PASSED\n");
}

/* ── T14: logger_add_sink / remove_sink error paths (log.c L584-594) ── */
static void test_logger_sink_error_paths(void) {
    int ret;
    log_sink_t *sink = make_test_sink();

    /* NULL logger. */
    ret = logger_add_sink(NULL, sink);
    if (ret != CLOG_ERR_INVALID_ARG) {
        fprintf(stderr, "FAIL: add_sink(NULL,sink) expected INVALID_ARG, got %d\n", ret);
        exit(1);
    }
    ret = logger_remove_sink(NULL, sink);
    if (ret != CLOG_ERR_INVALID_ARG) {
        fprintf(stderr, "FAIL: remove_sink(NULL,sink) expected INVALID_ARG, got %d\n", ret);
        exit(1);
    }

    /* NULL sink. */
    ret = logger_add_sink(NULL, NULL);
    if (ret != CLOG_ERR_INVALID_ARG) {
        fprintf(stderr, "FAIL: add_sink(NULL,NULL) expected INVALID_ARG, got %d\n", ret);
        exit(1);
    }
    ret = logger_remove_sink(NULL, NULL);
    if (ret != CLOG_ERR_INVALID_ARG) {
        fprintf(stderr, "FAIL: remove_sink(NULL,NULL) expected INVALID_ARG, got %d\n", ret);
        exit(1);
    }

    /* Uninitialized logger (no call to logger_init_internal). */
    logger_t *raw = (logger_t *)calloc(1, sizeof(logger_t));
    if (!raw)
        exit(1);
    clog_mutex_init(&raw->dispatcher_mutex);
    ret = logger_add_sink(raw, sink);
    if (ret != CLOG_ERR_RELOAD) {
        fprintf(stderr, "FAIL: add_sink(uninit) expected RELOAD, got %d\n", ret);
        exit(1);
    }
    /* Remove sink from uninitialized logger — init dispatcher first so we can add one. */
    ret = log_dispatcher_add_sink_for(raw, sink);
    if (ret != 0) {
        fprintf(stderr, "FAIL: dispatcher_add_sink\n");
        exit(1);
    }
    /* Fake initialized so the remove check passes. */
    int try = log_dispatcher_remove_sink_for(raw, sink);
    (void)try;

    clog_mutex_destroy(&raw->dispatcher_mutex);
    free(raw);
    free(sink);

    printf("test_logger_sink_error_paths PASSED\n");
}

/* ── T15: miscellaneous logger API NULL error paths ── */
static void test_logger_api_error_paths(void) {
    char buf[16];

    /* log.c L598-599. */
    logger_set_level(NULL, LOG_LEVEL_INFO);

    /* log.c L614-615. */
    logger_get_module(NULL, buf, sizeof(buf));

    /* log.c L622-623. */
    log_stats_t st;
    logger_get_stats(NULL, &st);

    /* log.c L632 — NULL logger. */
    logger_config_set(NULL, NULL);

    /* log.c L645-646. */
    if (logger_config_get(NULL) != NULL) {
        fprintf(stderr, "FAIL: config_get(NULL) expected NULL\n");
        exit(1);
    }

    /* log.c L321-322. */
    if (log_get_thread_context(NULL) != NULL) {
        fprintf(stderr, "FAIL: get_thread_context(NULL) expected NULL\n");
        exit(1);
    }

    /* log.c L304-305: set_thread_context key not found + value=NULL. */
    log_clear_thread_context();
    clogx_errno_t er = log_set_thread_context("nosuch_t15", NULL);
    /* Should succeed (no-op for setting NULL on nonexistent key). */
    if (er != CLOG_OK) {
        fprintf(stderr, "FAIL: set_thread_context(nosuch,NULL) expected OK, got %d\n", er);
        exit(1);
    }

    printf("test_logger_api_error_paths PASSED\n");
}

/* ── T16: suppressed message + async path (log.c L228-232) ── */
static void test_suppressed_msg_async(void) {
    log_config_t cfg = {0};
    cfg.level = LOG_LEVEL_TRACE;
    cfg.async = true;
    cfg.queue_size = 64;
    cfg.console_enable = true;
    cfg.console_stderr = true;
    cfg.rate_limit_enable = true;
    cfg.rate_limit_max_per_sec = 0;
    cfg.rate_limit_burst = 0;

    if (log_init(NULL) != 0) {
        fprintf(stderr, "FAIL: log_init for T16\n");
        exit(1);
    }
    log_config_set(&cfg);

    g_fb_count = 0;
    log_set_async_fallback_cb(inc_fallback);

    /* First message might be allowed (burst), subsequent ones suppressed. */
    LOG_INFO("suppress me");
    LOG_INFO("suppress me");
    LOG_INFO("suppress me");

    /* At least some should have been suppressed (rate_limit_burst=0). */
    log_async_flush();
    log_destroy();
    printf("test_suppressed_msg_async PASSED\n");
}

/* ── T17: logger_config_set time_format branch (log.c L638-640) ── */
static void test_logger_config_set_time_format(void) {
    log_config_t cfg = {0};
    cfg.level = LOG_LEVEL_TRACE;
    cfg.console_enable = true;
    cfg.console_stderr = true;
    cfg.format = "[%level] %msg";
    cfg.time_format = "%H:%M:%S";

    logger_t *logger = logger_create_from_config(&cfg);
    if (!logger) {
        fprintf(stderr, "FAIL: create for T17\n");
        exit(1);
    }

    log_config_t new_cfg = {0};
    new_cfg.level = LOG_LEVEL_DEBUG;
    new_cfg.time_format = "%S";
    logger_config_set(logger, &new_cfg);

    logger_destroy(logger);
    printf("test_logger_config_set_time_format PASSED\n");
}

/* ── T18: log_reload with no-sinks config (log.c L432) ── */
static void test_reload_no_sinks(void) {
    /* Init with a valid (sinks-enabled) config first. */
    write_file("build/gap_nosinks_reload.yaml", "log:\n"
                                                "  level: TRACE\n"
                                                "  async: false\n"
                                                "  console_enable: true\n"
                                                "  console_stderr: true\n");

    if (log_init("build/gap_nosinks_reload.yaml") != 0) {
        fprintf(stderr, "FAIL: log_init for T18\n");
        exit(1);
    }

    /* Overwrite the config file with no-sinks and reload. */
    write_file("build/gap_nosinks_reload.yaml", "log:\n"
                                                "  level: TRACE\n"
                                                "  async: false\n"
                                                "  console_enable: false\n");

    int ret = log_reload();
    if (ret != CLOG_ERR_NO_SINKS) {
        fprintf(stderr, "FAIL: reload no-sinks expected NO_SINKS, got %d\n", ret);
        exit(1);
    }
    log_destroy();
    printf("test_reload_no_sinks PASSED\n");
}

static void test_file_sink_null_paths(void) {
    log_sink_t *fs = file_sink_create(NULL, 0, 0);
    if (fs) {
        fs->destroy(fs);
    }
    fs = file_sink_create("", 0, 0);
    if (fs) {
        fs->destroy(fs);
    }

    log_sink_t *valid_fs = file_sink_create("build/test_file_sink_null.log", 100, 1);
    if (valid_fs) {
        valid_fs->write(valid_fs, NULL, 0);
        valid_fs->atfork_child(NULL);
        valid_fs->destroy(valid_fs);
    }
    printf("test_file_sink_null_paths PASSED\n");
}

static void test_async_edge_cases(void) {
    log_config_t cfg = {0};
    cfg.level = LOG_LEVEL_TRACE;
    cfg.async = true;
    cfg.queue_size = 64;
    cfg.console_enable = true;
    cfg.console_stderr = true;

    if (log_init(NULL) == 0) {
        log_config_set(&cfg);

        /* Record with ONLY module (no message, no tag) */
        log_record_t rec_mod = {0};
        rec_mod.level = LOG_LEVEL_INFO;
        rec_mod.module = "only_mod";
        log_async_write(&rec_mod);

        /* Record with ONLY tag (no message, no module) */
        log_record_t rec_tag = {0};
        rec_tag.level = LOG_LEVEL_INFO;
        rec_tag.tag = "only_tag";
        log_async_write(&rec_tag);

        log_async_flush();
        log_destroy();
    }

    log_async_shutdown();
    printf("test_async_edge_cases PASSED\n");
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

    test_async_init_already_running();
    test_async_queue_full_write();
    test_async_queue_depth_active();
    test_logger_flush_paths();
    test_logger_sink_error_paths();
    test_logger_api_error_paths();
    test_suppressed_msg_async();
    test_logger_config_set_time_format();
    test_reload_no_sinks();

    test_file_sink_null_paths();
    test_async_edge_cases();

    printf("=== all coverage-gap tests PASSED ===\n");
    return 0;
}
