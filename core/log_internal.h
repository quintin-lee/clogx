/**
 * @file log_internal.h
 * @brief Internal shared header for multi-instance logger_t support.
 *
 * All core modules (dispatcher, formatter, async, config, rate_limit)
 * include this header to access the full logger_t struct definition
 * and the default global instance. Public callers only see the opaque
 * forward declaration in include/log.h.
 */
#ifndef LOG_INTERNAL_H
#define LOG_INTERNAL_H

#include "clog_port.h"
#include "log_config.h"
#include "log_limits.h"
#include "log_sink.h"
#include "queue.h"

/* ── Full logger_t struct definition ── */

typedef struct logger_t {
    /* ── Configuration ── */
    log_config_t  config;
    char          config_path[512]; /* YAML path for reload */
    clog_rwlock_t config_rwlock;    /* rwlock for config reads/writes */

    /* ── Sink dispatcher ── */
    log_sink_t **sinks;
    int          sink_count;
    clog_mutex_t dispatcher_mutex;

    /* ── Async worker (optional) ── */
    mpsc_queue_t *queue;
    clog_thread_t worker_thread;
    volatile int  async_running;
    volatile int  async_processing;

    /* ── Rate limiter (optional) ── */
    bool         rl_enabled;
    double       rl_tokens;
    double       rl_max_tokens;
    double       rl_fill_rate;
    uint64_t     rl_last_update_ms;
    uint64_t     rl_suppressed_count;
    uint64_t     rl_total_suppressed;
    clog_mutex_t rl_mutex;

    /* ── Formatter ── */
    char         format_str[CLOG_MAX_FORMAT_SIZE];
    char         time_format_str[64];
    clog_mutex_t fmt_mutex;

    /* ── Module name ── */
    char         module[64];
    clog_mutex_t module_mutex;

    /* ── Runtime statistics ── */
    uint64_t total_logged;
    uint64_t dropped_queue_full;

    /* ── Lifecycle ── */
    bool initialized;

    /* ── Callbacks ── */
    void (*async_fallback_cb)(void);
} logger_t;

extern logger_t g_default_logger;

#endif /* LOG_INTERNAL_H */
