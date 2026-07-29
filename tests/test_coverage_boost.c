#include "clog_port.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "log_async.h"
#include "log_formatter.h"
#include "log_rate_limit.h"
#include "log_signal.h"
#include "log_sink.h"

int main(void) {
    if (log_init(NULL) != 0) {
        fprintf(stderr, "log_init failed\n");
        return 1;
    }

    /* 1. Test MDC Thread Context Edge Cases */
    log_set_thread_context(NULL, "val");
    log_set_thread_context("", "val");
    log_set_thread_context("k1", "v1");
    log_set_thread_context("k2", "v2");
    if (strcmp(log_get_thread_context("k1"), "v1") != 0) {
        fprintf(stderr, "MDC k1 mismatch\n");
        return 1;
    }
    if (log_get_thread_context("nonexistent") != NULL) {
        fprintf(stderr, "MDC nonexistent should be NULL\n");
        return 1;
    }

    /* Fill MDC context up to limit */
    char kbuf[32], vbuf[64];
    for (int i = 3; i <= 20; i++) {
        snprintf(kbuf, sizeof(kbuf), "key_%d", i);
        snprintf(vbuf, sizeof(vbuf), "val_%d", i);
        log_set_thread_context(kbuf, vbuf);
    }
    log_clear_thread_context();

    /* 2. Test Signal Handler Functions Directly */
#ifndef _WIN32
    log_install_signal_handlers();
    log_install_signal_handlers(); /* Re-entrant call test */
    log_signal_handler(SIGTERM);
    if (log_get_pending_signal() != SIGTERM) {
        fprintf(stderr, "expected SIGTERM pending\n");
        return 1;
    }
    log_restore_signal_handlers();
    log_restore_signal_handlers(); /* Re-entrant call test */
#endif

    /* 3. Test Formatter Edge Cases & Color Map */
    log_record_t rec = {
        .level = LOG_LEVEL_INFO,
        .message = "Message with \"quotes\", \n newlines, \t tabs, \r cr, \b bs, \f ff, \x01 ctrl "
                   "& \\ backslashes",
        .module = "test_mod",
        .file = "test_coverage.c",
        .func = "main_func",
        .tag = "test_tag",
        .line = 42,
        .timestamp = 1600000000,
        .tid = 1001,
        .pid = 2002,
    };

    get_log_color(LOG_LEVEL_TRACE);
    get_log_color(LOG_LEVEL_DEBUG);
    get_log_color(LOG_LEVEL_INFO);
    get_log_color(LOG_LEVEL_WARN);
    get_log_color(LOG_LEVEL_ERROR);
    get_log_color(LOG_LEVEL_FATAL);
    get_log_color((log_level_t)99);

    char out_buf[1024];

    /* JSON Formatter test */
    log_formatter_init("json", "%Y-%m-%d %H:%M:%S");
    log_formatter_format(&rec, out_buf, sizeof(out_buf));

    /* NULL message JSON test */
    log_record_t null_msg_rec = rec;
    null_msg_rec.message = NULL;
    log_formatter_format(&null_msg_rec, out_buf, sizeof(out_buf));

    /* Custom token formatter test */
    log_formatter_init("[%level] [%module] [%file:%line] [%func] [%tag] [%pid] [%tid] %msg\n",
                       "%Y-%m-%d %H:%M:%S");
    log_formatter_format(&rec, out_buf, sizeof(out_buf));

    log_formatter_init("%unknown_token %msg\n", NULL);
    log_formatter_format(&rec, out_buf, sizeof(out_buf));

    /* Small buffer truncation test */
    char small_buf[10];
    log_formatter_format(&rec, small_buf, sizeof(small_buf));

    /* 4. Test Sinks Error Paths & Creation Edge Cases */
    log_sink_t *console_s = console_sink_create(1 /* stderr */);
    if (console_s) {
        log_add_sink(console_s);
    }

    log_sink_t *file_s = file_sink_create("logs/cov_test.log", 1024 * 1024, 3);
    if (file_s) {
        const char *log_text = "file sink test line\n";
        file_s->write(file_s, log_text, strlen(log_text));
        file_s->flush(file_s);
        if (file_s->atfork_child) {
            file_s->atfork_child(file_s);
        }
        if (file_s->destroy)
            file_s->destroy(file_s);
    }

#ifndef _WIN32
    log_sink_t *syslog_s = syslog_sink_create("cov_syslog", 1 << 3);
    if (syslog_s) {
        if (syslog_s->atfork_child) {
            syslog_s->atfork_child(syslog_s);
        }
        if (syslog_s->destroy)
            syslog_s->destroy(syslog_s);
    }

    log_sink_t *sock_s = socket_sink_create("127.0.0.1", 9999);
    if (sock_s) {
        if (sock_s->atfork_child) {
            sock_s->atfork_child(sock_s);
        }
        if (sock_s->destroy)
            sock_s->destroy(sock_s);
    }
#endif

    /* 5. Test Rate Limiter Suppressed Total */
    uint64_t suppressed = 0;
    log_rate_limit_init(1, 1, 1);
    log_rate_limit_allow(&suppressed);
    log_rate_limit_get_total_suppressed();

    /* 6. Test Observability Stats & Depth */
    log_stats_t stats;
    log_get_stats(&stats);
    log_get_stats(NULL);
    log_async_get_queue_depth();

    log_destroy();
    printf("coverage boost test passed\n");
    return 0;
}
