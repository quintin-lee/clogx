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

/**
 * @struct logger_t
 * @brief Complete internal representation of a logger instance.
 *
 * Holds configuration, sink dispatcher, optional async worker, rate limiter,
 * formatter state, and runtime statistics. The global singleton
 * @ref g_default_logger is the default instance; multi-instance logers are
 * created via @ref logger_create.
 *
 * Thread Safety: Field access is protected by their respective mutexes.
 * The @c config_rwlock allows concurrent reads with exclusive writes.
 */
typedef struct logger_t {
    /* ── Configuration ── */
    log_config_t  config;               /**< Current configuration snapshot. */
    char          config_path[512];      /**< YAML config path for hot-reload. */
    clog_rwlock_t config_rwlock;         /**< rwlock protecting config reads/writes. */

    /* ── Sink dispatcher ── */
    log_sink_t **sinks;                  /**< Array of registered sink pointers. */
    int          sink_count;             /**< Number of active sinks. */
    clog_mutex_t dispatcher_mutex;       /**< Mutex protecting sink array. */

    /* ── Async worker (optional) ── */
    mpsc_queue_t *queue;                 /**< MPSC record queue (NULL if sync mode). */
    clog_thread_t worker_thread;         /**< Background consumer thread handle. */
    volatile int  async_running;         /**< 1 while the worker thread is alive. */
    volatile int  async_processing;      /**< 1 while draining a batch. */

    /* ── Rate limiter (optional) ── */
    bool         rl_enabled;             /**< Token-bucket rate limiting active. */
    double       rl_tokens;              /**< Current available tokens. */
    double       rl_max_tokens;          /**< Maximum burst capacity. */
    double       rl_fill_rate;           /**< Tokens added per second. */
    uint64_t     rl_last_update_ms;      /**< Last refill timestamp (ms). */
    uint64_t     rl_suppressed_count;    /**< Suppressed count in current window. */
    uint64_t     rl_total_suppressed;    /**< Lifetime suppressed count. */
    clog_mutex_t rl_mutex;               /**< Mutex for token-bucket operations. */

    /* ── Formatter ── */
    char         format_str[CLOG_MAX_FORMAT_SIZE];   /**< Format template buffer. */
    char         time_format_str[64];                 /**< strftime time format. */
    clog_mutex_t fmt_mutex;              /**< Mutex for format string access. */

    /* ── Module name ── */
    char         module[64];             /**< Module/tag name for %module token. */
    clog_mutex_t module_mutex;           /**< Mutex for module name access. */

    /* ── Runtime statistics ── */
    uint64_t total_logged;               /**< Total log records dispatched. */
    uint64_t dropped_queue_full;         /**< Records dropped (queue full). */

    /* ── Lifecycle ── */
    bool initialized;

    /* ── Callbacks ── */
    void (*async_fallback_cb)(void);
} logger_t;

extern logger_t g_default_logger;

#endif /* LOG_INTERNAL_H */
