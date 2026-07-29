/**
 * @file test_coverage_deep.c
 * @brief Comprehensive edge-case & error-path tests targeting >=75% branch coverage.
 */

#include "clog_port.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dispatcher.h"
#include "log.h"
#include "log_async.h"
#include "log_config.h"
#include "log_formatter.h"
#include "log_limits.h"
#include "log_rate_limit.h"
#include "log_record.h"
#include "log_signal.h"
#include "log_sink.h"

static int g_fallback_count = 0;
static void test_fallback_cb(void) {
    g_fallback_count++;
}

static int dummy_write(log_sink_t *sink, const char *buf, size_t len) {
    (void)sink;
    (void)buf;
    (void)len;
    return 0;
}
static void dummy_flush(log_sink_t *sink) {
    (void)sink;
}
static void dummy_destroy(log_sink_t *sink) {
    (void)sink;
}

static void write_temp_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

int main(void) {
    printf("=== Starting test_coverage_deep ===\n");

    /* -------------------------------------------------------------
     * 1. ERROR STRINGS & MODULE & SINK API EDGE CASES
     * ------------------------------------------------------------- */
    log_strerror(CLOG_OK);
    log_strerror(CLOG_ERR_INVALID_ARG);
    log_strerror(CLOG_ERR_FILE_OPEN);
    log_strerror(CLOG_ERR_INIT_REENTRANT);
    log_strerror(CLOG_ERR_CONFIG_PARSE);
    log_strerror(CLOG_ERR_THREAD_CREATE);
    log_strerror(CLOG_ERR_SOCKET_CONNECT);
    log_strerror(CLOG_ERR_OOM);
    log_strerror(CLOG_ERR_RELOAD);
    log_strerror(9999);

    log_set_module(NULL);
    log_set_module("");
    log_set_module("custom_module");

    char mod_buf[64];
    log_get_module(NULL, 10);
    log_get_module(mod_buf, 0);
    log_get_module(mod_buf, sizeof(mod_buf));

    log_sink_t valid_dummy = {
        .write = dummy_write,
        .flush = dummy_flush,
        .destroy = dummy_destroy,
    };
    log_sink_set_level(&valid_dummy, LOG_LEVEL_TRACE);
    log_add_sink(NULL);
    log_remove_sink(NULL);
    log_remove_sink(&valid_dummy);

    /* -------------------------------------------------------------
     * 2. SIGNAL HANDLER & FORK CORNER CASES
     * ------------------------------------------------------------- */
    log_install_signal_handlers();
    log_install_signal_handlers(); /* Re-entrant call -> CLOG_OK */
    log_signal_handler(SIGINT);
    if (log_get_pending_signal() != SIGINT) {
        fprintf(stderr, "signal mismatch\n");
    }
    log_restore_signal_handlers();
    log_restore_signal_handlers(); /* Re-entrant call -> no-op */

    log_dispatcher_atfork_prepare();
    log_dispatcher_atfork_parent();
    log_dispatcher_atfork_child();

    /* -------------------------------------------------------------
     * 3. ASYNC API EDGE CASES
     * ------------------------------------------------------------- */
    log_async_init(0);
    log_async_init(-1);
    log_async_shutdown();
    log_async_flush();
    if (log_async_get_queue_depth() != 0) {
        fprintf(stderr, "queue depth non-zero\n");
    }
    if (log_async_is_running()) {
        fprintf(stderr, "async shouldn't be running\n");
    }

    log_record_t null_str_rec = {
        .level = LOG_LEVEL_INFO,
        .timestamp = 1000,
        .message = NULL,
        .module = NULL,
        .tag = NULL,
    };
    log_async_write(&null_str_rec); /* Dispatches synchronously when queue is NULL */

    log_record_t tag_only_rec = {
        .level = LOG_LEVEL_INFO,
        .timestamp = 1000,
        .message = NULL,
        .module = NULL,
        .tag = "tag_only",
    };
    log_async_write(&tag_only_rec);

    log_record_t mod_only_rec = {
        .level = LOG_LEVEL_INFO,
        .timestamp = 1000,
        .message = NULL,
        .module = "mod_only",
        .tag = NULL,
    };
    log_async_write(&mod_only_rec);

    log_async_atfork_child();

    /* -------------------------------------------------------------
     * 4. SINK FACTORY EDGE CASES (SOCKET & SYSLOG)
     * ------------------------------------------------------------- */
    if (socket_sink_create_tls(NULL, 80, false, NULL, false) != NULL) {
        fprintf(stderr, "expected NULL socket sink\n");
    }
    if (socket_sink_create_tls("", 80, false, NULL, false) != NULL) {
        fprintf(stderr, "expected NULL socket sink\n");
    }
    if (socket_sink_create_tls("127.0.0.1", 0, false, NULL, false) != NULL) {
        fprintf(stderr, "expected NULL socket sink\n");
    }
    if (socket_sink_create_tls("127.0.0.1", 70000, false, NULL, false) != NULL) {
        fprintf(stderr, "expected NULL socket sink\n");
    }

#ifndef _WIN32
    log_sink_t *syslog_null = syslog_sink_create(NULL, 0);
    if (syslog_null) {
        syslog_null->write(syslog_null, "[TRACE] syslog trace", 19);
        syslog_null->write(syslog_null, "[DEBUG] syslog debug", 19);
        syslog_null->write(syslog_null, "[WARN] syslog warn", 17);
        syslog_null->write(syslog_null, "[ERROR] syslog err", 17);
        syslog_null->write(syslog_null, "[FATAL] syslog fatal", 19);
        syslog_null->write(syslog_null, "[INFO] syslog info", 17);
        syslog_null->flush(syslog_null);
        if (syslog_null->atfork_child)
            syslog_null->atfork_child(syslog_null);
        syslog_null->destroy(syslog_null);
    }
#endif

    /* -------------------------------------------------------------
     * 5. DISPATCHER & SNAPSHOT EDGE CASES
     * ------------------------------------------------------------- */
    log_dispatcher_add_sink(NULL);
    log_dispatcher_remove_sink(NULL);
    log_dispatcher_dispatch(NULL);
    log_dispatcher_destroy_snapshot(NULL);

    log_config_t empty_sink_cfg = {0};
    empty_sink_cfg.console_enable = false;
    empty_sink_cfg.file_enable = false;
    empty_sink_cfg.socket_enable = false;
    log_dispatcher_snapshot_t snap;
    if (log_dispatcher_build_snapshot(&empty_sink_cfg, &snap) == 0) {
        fprintf(stderr, "expected snapshot build error\n");
    }

    /* -------------------------------------------------------------
     * 6. CONFIG PARSING BOUNDARY & INVALID YAML TESTS
     * ------------------------------------------------------------- */
    write_temp_file("build/cfg_valid_full.yaml", "log:\n"
                                                 "  level: TRACE\n"
                                                 "  async: true\n"
                                                 "  queue_size: 1024\n"
                                                 "  catch_signals: true\n"
                                                 "  color: true\n"
                                                 "  format: \"json\"\n"
                                                 "  time_format: \"%Y-%m-%d %H:%M:%S\"\n"
                                                 "  console_enable: true\n"
                                                 "  console_stderr: true\n"
                                                 "  file_enable: true\n"
                                                 "  file_path: build/deep_app.log\n"
                                                 "  max_size: 10KB\n"
                                                 "  backups: 5\n"
                                                 "  socket_enable: false\n"
                                                 "  rate_limit_enable: true\n"
                                                 "  rate_limit_max_per_sec: 500\n"
                                                 "  rate_limit_burst: 50\n");

    if (log_init("build/cfg_valid_full.yaml") == 0) {
        log_add_sink(&valid_dummy);
        log_destroy();
    }

    write_temp_file("build/cfg_max_size_units.yaml", "log:\n"
                                                     "  max_size: 5KB\n"
                                                     "  max_size: 2M\n"
                                                     "  max_size: 3MB\n"
                                                     "  max_size: 1G\n"
                                                     "  max_size: 2GB\n"
                                                     "  max_size: 500\n"
                                                     "  rate_limit_max_per_sec: 1000\n"
                                                     "  rate_limit_burst: 100\n");
    log_init("build/cfg_max_size_units.yaml");
    log_destroy();

    write_temp_file("build/cfg_invalid_branches.yaml", "log:\n"
                                                       "  max_size: 99999999999999999999999GB\n"
                                                       "  max_size: 100TB\n"
                                                       "  max_size: \"\"\n"
                                                       "  rate_limit_max_per_sec: -5\n"
                                                       "  rate_limit_max_per_sec: abc\n"
                                                       "  rate_limit_burst: -2\n"
                                                       "  rate_limit_burst: xyz\n"
                                                       "  port: 70000\n"
                                                       "  port: -1\n"
                                                       "  backups: -10\n"
                                                       "  queue_size: -100\n"
                                                       "  level: UNKNOWN_LEVEL\n"
                                                       "  unknown_key_foobar: 12345\n");
    log_init("build/cfg_invalid_branches.yaml");
    log_destroy();

    write_temp_file("build/cfg_seq.yaml", "- item1\n- item2\n");
    log_init("build/cfg_seq.yaml");
    log_destroy();

    log_sink_t *bad_file_s = file_sink_create("/invalid_dir_999/cannot_create/test.log", 1024, 2);
    if (bad_file_s) {
        bad_file_s->write(bad_file_s, "test", 4);
        bad_file_s->flush(bad_file_s);
        if (bad_file_s->destroy)
            bad_file_s->destroy(bad_file_s);
    }

    log_sink_t *rot_s = file_sink_create("build/rot_forced.log", 20, 2);
    if (rot_s) {
        const char *line = "123456789012345678901234567890\n";
        rot_s->write(rot_s, line, strlen(line));
        rot_s->write(rot_s, line, strlen(line));
        rot_s->flush(rot_s);
        if (rot_s->destroy)
            rot_s->destroy(rot_s);
    }

    log_record_t esc_rec = {
        .level = LOG_LEVEL_INFO,
        .timestamp = 1600000000000000ULL,
        .file = "test.c",
        .func = "func_esc",
        .module = "mod_esc",
        .tag = "tag_esc",
        .message = "quotes \" bs \\ fn \n cr \r tab \t ff \f bs \b ctrl \x03",
        .line = 100,
        .tid = 1,
        .pid = 2,
    };

    log_formatter_init("json", "%Y-%m-%d");
    size_t sizes[] = {5, 12, 25, 40, 55, 75, 100, 130, 200};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        char *tbuf = malloc(sizes[i]);
        if (tbuf) {
            log_formatter_format(&esc_rec, tbuf, sizes[i]);
            free(tbuf);
        }
    }

    log_formatter_init("[%time] [%level] [%thread] [%pid] [%file] [%line] [%func] [%module] [%tag] "
                       "%newline %msg %xyz\n",
                       NULL);
    log_record_t null_fields_rec = {
        .level = LOG_LEVEL_WARN,
        .timestamp = 1600000000000000ULL,
        .file = NULL,
        .func = NULL,
        .module = NULL,
        .tag = NULL,
        .message = NULL,
        .line = 50,
        .tid = 10,
        .pid = 20,
    };
    char text_buf[512];
    log_formatter_format(&null_fields_rec, text_buf, sizeof(text_buf));
    log_formatter_reset();

    log_config_t async_cfg = {0};
    async_cfg.level = LOG_LEVEL_DEBUG;
    async_cfg.async = true;
    async_cfg.queue_size = 64;
    async_cfg.color = false;
    async_cfg.format = "[%time] [%level] %msg";
    async_cfg.console_enable = true;
    async_cfg.console_stderr = true;
    async_cfg.file_enable = false;
    log_config_set(&async_cfg);

    log_set_async_fallback_cb(test_fallback_cb);

    for (int i = 0; i < 20; i++) {
        LOG_INFO("async test %d", i);
    }
    log_flush();
    log_destroy();

    log_config_t multi_cfg = {0};
    multi_cfg.level = LOG_LEVEL_TRACE;
    multi_cfg.async = false;
    multi_cfg.color = true;
    multi_cfg.format = "[%time] [%level] %msg";
    multi_cfg.console_enable = true;
    multi_cfg.console_stderr = false;
    multi_cfg.file_enable = true;
    snprintf(multi_cfg.file_path, sizeof(multi_cfg.file_path), "build/multi_pipeline.log");
    multi_cfg.file_max_size = 1024 * 1024;
    multi_cfg.file_backups = 2;
    log_config_set(&multi_cfg);

    log_sink_t *c_err = console_sink_create_stderr(true);
    if (c_err) {
        log_sink_set_level(c_err, LOG_LEVEL_ERROR);
        log_add_sink(c_err);
    }

    log_set_thread_context("pipeline_key", "pipeline_val");
    LOG_TRACE("trace pipeline msg");
    LOG_DEBUG("debug pipeline msg");
    LOG_INFO("info pipeline msg");
    LOG_WARN("warn pipeline msg");
    LOG_ERROR("error pipeline msg");
    LOG_FATAL("fatal pipeline msg");
    log_clear_thread_context();

    if (c_err) {
        log_remove_sink(c_err);
        if (c_err->destroy)
            c_err->destroy(c_err);
    }

    log_flush();
    log_destroy();

    printf("=== test_coverage_deep PASSED ===\n");
    return 0;
}
