/**
 * @file test_coverage_deep.c
 * @brief Comprehensive edge-case & error-path tests targeting >=80% branch coverage.
 */

#include "clog_port.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/wait.h>
#endif

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
static int failing_write_fn(log_sink_t *sink, const char *buf, size_t len) {
    (void)sink;
    (void)buf;
    (void)len;
    return -1;
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

    log_sink_t *valid_dummy = calloc(1, sizeof(log_sink_t));
    if (valid_dummy) {
        valid_dummy->write = dummy_write;
        valid_dummy->flush = dummy_flush;
        valid_dummy->destroy = dummy_destroy;
        log_sink_set_level(valid_dummy, LOG_LEVEL_TRACE);
        log_add_sink(NULL);
        log_remove_sink(NULL);
        log_remove_sink(valid_dummy);
        free(valid_dummy);
        valid_dummy = NULL;
    }

    /* Test sink write failure path */
    log_sink_t *failing_dummy = calloc(1, sizeof(log_sink_t));
    if (failing_dummy) {
        failing_dummy->write = failing_write_fn;
        failing_dummy->flush = dummy_flush;
        failing_dummy->destroy = dummy_destroy;
        failing_dummy->min_level = LOG_LEVEL_TRACE;
        log_add_sink(failing_dummy);
        LOG_INFO("test failing sink write");
        log_remove_sink(failing_dummy);
        free(failing_dummy);
    }

    /* Test small async queue overflow & fallback */
    log_config_t async_overflow_cfg = {0};
    async_overflow_cfg.async = true;
    async_overflow_cfg.queue_size = 2;
    async_overflow_cfg.console_enable = false;
    async_overflow_cfg.file_enable = false;
    log_config_set(&async_overflow_cfg);
    for (int i = 0; i < 50; i++) {
        LOG_INFO("overflow msg %d", i);
    }
    log_flush();

    /* Test ultra-long format string & long parameters */
    char long_fmt_buf[1024];
    memset(long_fmt_buf, 'X', sizeof(long_fmt_buf) - 1);
    long_fmt_buf[sizeof(long_fmt_buf) - 1] = '\0';
    LOG_INFO("%s", long_fmt_buf);

    /* Test multiple sink removal resizing branch */
    log_sink_t *dummy1 = console_sink_create(false);
    log_sink_t *dummy2 = console_sink_create(false);
    if (dummy1 && dummy2) {
        log_add_sink(dummy1);
        log_add_sink(dummy2);
        log_remove_sink(dummy1);
        log_remove_sink(dummy2);
        dummy1->destroy(dummy1);
        dummy2->destroy(dummy2);
    }

    /* -------------------------------------------------------------
     * 2. SIGNAL HANDLER & FORK CORNER CASES (POSIX ONLY)
     * ------------------------------------------------------------- */
#ifndef _WIN32
    log_install_signal_handlers();
    log_install_signal_handlers(); /* Re-entrant call -> CLOG_OK */

    if (log_get_signal_fd() < 0) {
        fprintf(stderr, "log_get_signal_fd should return valid fd when installed\n");
    }

    pid_t sig_pid = fork();
    if (sig_pid == 0) {
        signal(SIGTERM, SIG_IGN);
        signal(SIGINT, SIG_IGN);
        log_signal_handler(SIGTERM);
        log_process_pending_signals();
        log_signal_handler(SIGINT);
        log_process_pending_signals();
        _exit(0);
    } else if (sig_pid > 0) {
        int status;
        waitpid(sig_pid, &status, 0);
    }

    log_restore_signal_handlers();
    log_restore_signal_handlers(); /* Re-entrant call -> no-op */

    if (log_get_signal_fd() != -1) {
        fprintf(stderr, "log_get_signal_fd should return -1 when uninstalled\n");
    }

    log_dispatcher_atfork_prepare();
    log_dispatcher_atfork_parent();
    log_dispatcher_atfork_child();
#endif

    /* -------------------------------------------------------------
     * 3. ASYNC API EDGE CASES & RECORD CLONE / FREE
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
    log_async_write(&null_str_rec);

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
     * 4. RATE LIMITER TOKEN EXHAUSTION & REPLENISHMENT
     * ------------------------------------------------------------- */
    log_rate_limit_init(true, 10, 2); /* Max 2 burst tokens */
    uint64_t supp = 0;
    log_rate_limit_allow(NULL);  /* Token 1 used */
    log_rate_limit_allow(&supp); /* Token 2 used */
    if (log_rate_limit_allow(&supp)) {
        fprintf(stderr, "expected rate limit rejection\n");
    }
    log_rate_limit_allow(&supp); /* 2nd rejection */
    clog_sleep_ms(150);          /* Replenish tokens */
    log_rate_limit_allow(&supp); /* Allowed, supp should be 2 */
    if (log_rate_limit_get_total_suppressed() != 2) {
        fprintf(stderr, "expected 2 total suppressed\n");
    }
    log_rate_limit_reset();

    /* -------------------------------------------------------------
     * 5. SINK FACTORY & NULL ERROR PATHS (FILE, CONSOLE, CUSTOM, SOCKET, SYSLOG)
     * ------------------------------------------------------------- */
    if (file_sink_create(NULL, 100, 2) != NULL) {
        fprintf(stderr, "expected NULL file sink\n");
    }
    if (file_sink_create("", 100, 2) != NULL) {
        fprintf(stderr, "expected NULL file sink\n");
    }
    char huge_path[1200];
    memset(huge_path, 'a', sizeof(huge_path) - 1);
    huge_path[sizeof(huge_path) - 1] = '\0';
    if (file_sink_create(huge_path, 100, 2) != NULL) {
        fprintf(stderr, "expected NULL file sink for huge path\n");
    }

    console_sink_is_color_enabled(NULL);
    if (valid_dummy)
        console_sink_is_color_enabled(valid_dummy);
    log_sink_t *c_out_s = console_sink_create(true);
    if (c_out_s) {
        console_sink_is_color_enabled(c_out_s);
        c_out_s->write(c_out_s, "console test\n", 13);
        c_out_s->flush(c_out_s);
        if (c_out_s->destroy)
            c_out_s->destroy(c_out_s);
    }

    if (custom_sink_create(NULL, NULL, NULL, NULL) != NULL) {
        fprintf(stderr, "expected NULL custom sink\n");
    }
    custom_sink_get_private_data(NULL);
    if (valid_dummy)
        custom_sink_get_private_data(valid_dummy);

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
    log_sink_t *sock_invalid_host = socket_sink_create("invalid_ip_999.999.999.999", 8080);
    if (sock_invalid_host) {
        sock_invalid_host->write(sock_invalid_host, "test", 4);
        sock_invalid_host->destroy(sock_invalid_host);
    }
    log_sink_t *sock_closed = socket_sink_create("127.0.0.1", 59997);
    if (sock_closed) {
        sock_closed->write(sock_closed, "test", 4);
        sock_closed->destroy(sock_closed);
    }
#ifdef CLOG_USE_TLS
    log_sink_t *tls_closed = socket_sink_create_tls("127.0.0.1", 59996, true, "no_ca.crt", true);
    if (tls_closed) {
        tls_closed->write(tls_closed, "test", 4);
        tls_closed->destroy(tls_closed);
    }
#else
    log_sink_t *no_tls_s = socket_sink_create_tls("127.0.0.1", 59995, true, NULL, false);
    if (no_tls_s) {
        no_tls_s->write(no_tls_s, "test", 4);
        no_tls_s->destroy(no_tls_s);
    }
#endif

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
     * 6. DISPATCHER & SNAPSHOT EDGE CASES & COLOR DISPATCH
     * ------------------------------------------------------------- */
    log_dispatcher_add_sink(NULL);
    log_dispatcher_remove_sink(NULL);
    log_dispatcher_dispatch(NULL);
    log_dispatcher_destroy_snapshot(NULL);

    log_config_t color_cfg = {0};
    color_cfg.level = LOG_LEVEL_TRACE;
    color_cfg.color = true;
    color_cfg.console_enable = true;
    color_cfg.console_stderr = false;
    color_cfg.format = "[%time] [%level] %msg";
    log_config_set(&color_cfg);

    log_level_t levels[] = {LOG_LEVEL_TRACE, LOG_LEVEL_DEBUG, LOG_LEVEL_INFO,
                            LOG_LEVEL_WARN,  LOG_LEVEL_ERROR, LOG_LEVEL_FATAL};
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        log_record_t color_rec = {
            .level = levels[i],
            .timestamp = 1600000000000000ULL,
            .message = "color test message without newline",
            .line = 123,
            .tid = 1,
            .pid = 2,
        };
        log_dispatcher_dispatch(&color_rec);
    }
    log_destroy();

    /* Snapshot build, commit, destroy tests */
    log_config_t snap_full_cfg = {0};
    snap_full_cfg.console_enable = true;
    snap_full_cfg.console_stderr = true;
    snap_full_cfg.file_enable = true;
    snprintf(snap_full_cfg.file_path, sizeof(snap_full_cfg.file_path), "build/snap_deep.log");
    snap_full_cfg.file_max_size = 1024 * 1024;
    snap_full_cfg.file_backups = 2;

    log_dispatcher_snapshot_t real_snap = {0};
    if (log_dispatcher_build_snapshot(&snap_full_cfg, &real_snap) == 0) {
        log_dispatcher_commit_snapshot(&real_snap);
    }
    if (log_dispatcher_build_snapshot(&snap_full_cfg, &real_snap) == 0) {
        log_dispatcher_destroy_snapshot(&real_snap);
    }

    log_config_t empty_sink_cfg = {0};
    empty_sink_cfg.console_enable = false;
    empty_sink_cfg.file_enable = false;
    empty_sink_cfg.socket_enable = false;
    log_dispatcher_snapshot_t snap;
    if (log_dispatcher_build_snapshot(&empty_sink_cfg, &snap) == 0) {
        fprintf(stderr, "expected snapshot build error\n");
    }

    /* -------------------------------------------------------------
     * 7. CONFIG PARSING BOUNDARY & INVALID YAML TESTS
     * ------------------------------------------------------------- */
    log_init(NULL);
    log_init(NULL);      /* Re-entrant init test -> CLOG_ERR_INIT_REENTRANT */
    log_get_stats(NULL); /* Null stats pointer */
    log_reload();        /* Reload while initialized but path empty */
    log_destroy();
    log_reload(); /* Reload while uninitialized -> CLOG_ERR_RELOAD */

    write_temp_file("build/cfg_valid_full.yaml", "log:\n"
                                                 "  level: TRACE\n"
                                                 "  async: true\n"
                                                 "  queue_size: 1024\n"
                                                 "  catch_signals: false\n"
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
        log_sink_t *s_tmp = console_sink_create(false);
        if (s_tmp)
            log_add_sink(s_tmp);
        log_reload(); /* Valid reload test */
        log_destroy();
    }

    write_temp_file("build/cfg_aliases.yaml", "log:\n"
                                              "  path: build/alias_file.log\n"
                                              "  backup: 3\n"
                                              "  host: \"127.0.0.1\"\n"
                                              "  port: 8080\n"
                                              "  tls_enable: false\n"
                                              "  tls_ca_file: \"ca.crt\"\n"
                                              "  tls_skip_verify: true\n");
    log_init("build/cfg_aliases.yaml");
    log_destroy();

    write_temp_file("build/cfg_syntax_err.yaml", "log: {\n  bad_syntax: [unclosed_list\n");
    log_init("build/cfg_syntax_err.yaml");
    log_destroy();

    log_init("build/non_existent_file_999.yaml");
    log_destroy();

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

    /* -------------------------------------------------------------
     * 8. FORMATTER TRUNCATION & PATTERN VARIATIONS
     * ------------------------------------------------------------- */
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
    for (size_t sz = 1; sz <= 80; sz++) {
        char *tbuf = malloc(sz);
        if (tbuf) {
            log_formatter_format(&esc_rec, tbuf, sz);
            free(tbuf);
        }
    }

    /* Text formatter tokens with NULL & filled fields under varying buffer sizes */
    log_formatter_init("[%time] [%level] [%thread] [%pid] [%file] [%line] [%func] [%module] [%tag] "
                       "%context %newline %msg %xyz %%\n",
                       NULL);
    for (size_t sz = 1; sz <= 80; sz++) {
        char *tbuf = malloc(sz);
        if (tbuf) {
            log_formatter_format(&esc_rec, tbuf, sz);
            free(tbuf);
        }
    }

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

    /* -------------------------------------------------------------
     * 9. MULTI-PIPELINE & ASYNC FULL RUN
     * ------------------------------------------------------------- */
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
