/**
 * @file log.c
 * @brief Public logging entry points: init/destroy/reload/flush and write path.
 *
 * ## Architecture
 *
 * This file implements the singleton logger API (global `g_default_logger`)
 * and the multi-instance `logger_t` API. Both paths share the same internal
 * write path (`logger_writevprintf_internal`) and initialisation logic
 * (`logger_init_internal`).
 *
 * ## Write Path Flow
 *
 * ```
 * LOG_INFO(...)
 *   └─► log_writevprintf()
 *         └─► logger_writevprintf_internal()
 *               ├─ level filter (skip if below threshold)
 *               ├─ rate limit check (drop if suppressed)
 *               ├─ format message via vsnprintf
 *               ├─ populate log_record_t
 *               ├─ deep-copy into async queue  (if async mode)
 *               │     or
 *               └─ dispatch to all sinks      (if sync mode)
 * ```
 *
 * ## Thread Safety
 *
 * - `log_init` / `log_destroy`: serialised by `g_init_mutex`.
 * - `log_reload`: acquires `g_init_mutex` then uses atomic snapshot swap.
 * - `logger_writevprintf_internal`: safe for concurrent calls (no shared
 *   mutable state except atomics for Prometheus counters).
 * - Module name is protected by `module_mutex`.
 *
 * ## Fork Safety
 *
 * `pthread_atfork` handlers (POSIX only) acquire `g_init_mutex` and the
 * dispatcher lock before fork, then release them in child and parent
 * separately. The child re-creates the async worker thread.
 */
#include "log.h"
#include "clog_port.h"
#include "clogx_version.h"
#include "dispatcher.h"
#include "log_async.h"
#include "log_config.h"
#include "log_formatter.h"
#include "log_internal.h"
#include "log_limits.h"
#include "log_rate_limit.h"
#include "log_record.h"
#include "log_signal.h"
#include "plugin_loader.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

logger_t g_default_logger = {0};

static clog_mutex_t g_init_mutex                 = CLOG_MUTEX_INITIALIZER;
volatile uint64_t   g_prometheus_level_counts[6] = {0};

static clog_thread_local uint8_t g_thread_trace_id[16];
static clog_thread_local uint8_t g_thread_span_id[8];
static clog_thread_local bool    g_has_thread_trace_context = false;

const char *log_strerror(int err)
{
    switch (err) {
    case CLOG_OK:
        return "success";
    case CLOG_ERR_INVALID_ARG:
        return "invalid argument";
    case CLOG_ERR_INIT_REENTRANT:
        return "reentrant init without destroy";
    case CLOG_ERR_CONFIG_OPEN:
        return "failed to open config file";
    case CLOG_ERR_CONFIG_PARSE:
        return "config parse error";
    case CLOG_ERR_NO_SINKS:
        return "no sinks configured";
    case CLOG_ERR_FILE_OPEN:
        return "failed to open log file";
    case CLOG_ERR_FILE_WRITE:
        return "file write error";
    case CLOG_ERR_QUEUE_FULL:
        return "async queue full or closed";
    case CLOG_ERR_THREAD_CREATE:
        return "failed to create worker thread";
    case CLOG_ERR_SOCKET_CONNECT:
        return "socket connect failed";
    case CLOG_ERR_OOM:
        return "out of memory";
    case CLOG_ERR_RELOAD:
        return "reload failed";
    default:
        return "unknown error";
    }
}

void log_set_async_fallback_cb(void (*cb)(void))
{
    g_default_logger.async_fallback_cb = cb;
}

void (*log_get_async_fallback_cb(void))(void)
{
    return g_default_logger.async_fallback_cb;
}

static void logger_set_module_internal(logger_t *logger, const char *module)
{
    clog_mutex_lock(&logger->module_mutex);
    if (!module || !*module) {
        snprintf(logger->module, sizeof(logger->module), "%s", "main");
    } else {
        snprintf(logger->module, sizeof(logger->module), "%s", module);
    }
    clog_mutex_unlock(&logger->module_mutex);
}

void log_set_module(const char *module)
{
    logger_set_module_internal(&g_default_logger, module);
}

void log_get_module(char *buf, size_t n)
{
    if (!buf || n == 0) {
        return;
    }
    clog_mutex_lock(&g_default_logger.module_mutex);
    snprintf(buf, n, "%s", g_default_logger.module);
    clog_mutex_unlock(&g_default_logger.module_mutex);
}

int log_add_sink(log_sink_t *sink)
{
    if (!sink) {
        return CLOG_ERR_INVALID_ARG;
    }

    clog_mutex_lock(&g_init_mutex);
    if (!g_default_logger.initialized) {
        clog_mutex_unlock(&g_init_mutex);
        return CLOG_ERR_RELOAD;
    }
    int ret = log_dispatcher_add_sink_for(&g_default_logger, sink);
    clog_mutex_unlock(&g_init_mutex);
    return ret == 0 ? CLOG_OK : CLOG_ERR_OOM;
}

int log_remove_sink(log_sink_t *sink)
{
    if (!sink) {
        return CLOG_ERR_INVALID_ARG;
    }
    if (log_dispatcher_remove_sink_for(&g_default_logger, sink) != 0) {
        return CLOG_ERR_INVALID_ARG;
    }
    return CLOG_OK;
}

/**
 * @brief Internal initialisation shared by global and multi-instance loggers.
 *
 * Loads config from YAML, initialises rate limiter, dispatcher (creates
 * sinks from config), and optionally starts the async worker thread.
 * On failure, partially-initialised resources are cleaned up by the caller.
 *
 * @param logger  Logger instance to initialise (must be zero-initialized).
 * @param yaml_path  Path to YAML config file, or NULL for defaults.
 * @return CLOG_OK on success, or a negative clogx_errno_t code.
 *
 * @pre  Caller holds no locks on @p logger.
 * @post On success, `logger->initialized == true`.
 */
static int logger_init_internal(logger_t *logger, const char *yaml_path)
{
    clog_rwlock_init(&logger->config_rwlock);

    if (log_config_load_into(logger, yaml_path) != 0) {
        return CLOG_ERR_CONFIG_OPEN;
    }

    log_rate_limit_init_for(logger,
                            logger->config.rate_limit_enable,
                            logger->config.rate_limit_max_per_sec,
                            logger->config.rate_limit_burst);

    if (log_dispatcher_init_for(logger) != 0) {
        return CLOG_ERR_NO_SINKS;
    }

    if (logger->config.async) {
        if (log_async_init_for(logger, logger->config.queue_size) != 0) {
            log_dispatcher_destroy_for(logger);
            return CLOG_ERR_THREAD_CREATE;
        }
    }

    logger_set_module_internal(logger, "main");
    logger->initialized = true;

    fprintf(stderr, "[clogx] version " CLOGX_VERSION_STRING "\n");
    return CLOG_OK;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Internal write path (shared by log_writevprintf / logger_writevprintf)
 * ════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Core write path: format message, apply rate limits, dispatch or enqueue.
 *
 * This is the hot path for every LOG_* macro. It runs on the caller's thread
 * and must be as fast as possible. Steps:
 *
 * 1. Check pending signals (default logger only).
 * 2. Level filter — skip if below threshold.
 * 3. Increment Prometheus counters (atomic).
 * 4. Format message via `vsnprintf` into stack buffer.
 * 5. Copy module name under lock.
 * 6. Populate `log_record_t` with all metadata (timestamp, TID, PID, etc.).
 * 7. Rate limit check — suppress if bucket exhausted.
 * 8. Deep-copy into async queue (async mode) or dispatch to sinks (sync mode).
 *
 * @param logger  Logger instance.
 * @param level   Log level of this message.
 * @param file    Source file (__FILE__).
 * @param line    Source line (__LINE__).
 * @param func    Function name (__func__).
 * @param fmt     printf-style format string.
 * @param args_orig  Variadic argument list.
 *
 * @note Thread-safe: no locks held across the entire path. Module name is
 *       briefly locked; Prometheus counters use atomic increments.
 */
static void logger_writevprintf_internal(logger_t   *logger,
                                         log_level_t level,
                                         const char *file,
                                         int         line,
                                         const char *func,
                                         const char *fmt,
                                         va_list     args_orig)
{
    if (!logger || !logger->initialized) {
        return;
    }

    /* Signal check for the default logger only — instance users manage signals themselves. */
    if (logger == &g_default_logger && log_get_pending_signal() != 0) {
        log_process_pending_signals();
    }

    if (level < logger->config.level) {
        return;
    }

    clog_atomic_inc64(&logger->total_logged);
    if ((int)level >= 0 && (int)level < 6) {
        clog_atomic_inc64(&g_prometheus_level_counts[(int)level]);
    }

    char message[CLOG_MAX_MESSAGE_SIZE];

    va_list args;
    va_copy(args, args_orig);
    int ret = vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    if (ret < 0) {
        message[0] = '\0';
    } else if (ret >= (int)sizeof(message)) {
        if (sizeof(message) >= 4) {
            memcpy(message + sizeof(message) - 4, "...", 4);
        } else {
            message[sizeof(message) - 1] = '\0';
        }
    }

    char module_buf[64];
    clog_mutex_lock(&logger->module_mutex);
    snprintf(module_buf, sizeof(module_buf), "%s", logger->module);
    clog_mutex_unlock(&logger->module_mutex);

    log_record_t record;
    record.level     = level;
    record.timestamp = clog_get_timestamp_us();
    record.tid       = clog_get_thread_id();
    record.pid       = clog_getpid();
    record.file      = file;
    record.func      = func;
    record.line      = line;
    record.module    = module_buf;
    record.tag       = NULL;
    record.message   = message;
    if (g_has_thread_trace_context) {
        memcpy(record.trace_id, g_thread_trace_id, 16);
        memcpy(record.span_id, g_thread_span_id, 8);
    } else {
        memset(record.trace_id, 0, 16);
        memset(record.span_id, 0, 8);
    }

    uint64_t suppressed = 0;
    if (!log_rate_limit_allow_for(logger, &suppressed)) {
        return;
    }

    if (suppressed > 0) {
        char supp_msg[128];
        snprintf(supp_msg,
                 sizeof(supp_msg),
                 "[clogx] Suppressed %llu log messages due to rate limit",
                 (unsigned long long)suppressed);
        log_record_t supp_rec = record;
        supp_rec.level        = LOG_LEVEL_WARN;
        supp_rec.message      = supp_msg;
        if (logger->config.async) {
            if (log_async_write_for(logger, &supp_rec) != 0) {
                void (*cb)(void) = logger->async_fallback_cb;
                if (cb) {
                    cb();
                }
            }
        } else {
            log_dispatcher_dispatch_for(logger, &supp_rec);
        }
    }

    if (logger->config.async) {
        int ar = log_async_write_for(logger, &record);
        if (ar != 0) {
            clog_atomic_inc64(&logger->dropped_queue_full);
            void (*cb)(void) = logger->async_fallback_cb;
            if (cb) {
                cb();
            }
        }
    } else {
        log_dispatcher_dispatch_for(logger, &record);
    }
}

void log_writevprintf(
    log_level_t level, const char *file, int line, const char *func, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    logger_writevprintf_internal(&g_default_logger, level, file, line, func, fmt, args);
    va_end(args);
}

void logger_writevprintf(logger_t   *logger,
                         log_level_t level,
                         const char *file,
                         int         line,
                         const char *func,
                         const char *fmt,
                         ...)
{
    va_list args;
    va_start(args, fmt);
    logger_writevprintf_internal(logger, level, file, line, func, fmt, args);
    va_end(args);
}

static void logger_write_kv_internal(logger_t        *logger,
                                     log_level_t      level,
                                     const char      *file,
                                     int              line,
                                     const char      *func,
                                     const char      *msg,
                                     const clog_kv_t *kvs,
                                     size_t           kv_count)
{
    if (!logger || !logger->initialized) {
        return;
    }

    if (logger == &g_default_logger && log_get_pending_signal() != 0) {
        log_process_pending_signals();
    }

    if (level < logger->config.level) {
        return;
    }

    clog_atomic_inc64(&logger->total_logged);
    if ((int)level >= 0 && (int)level < 6) {
        clog_atomic_inc64(&g_prometheus_level_counts[(int)level]);
    }

    char module_buf[64];
    clog_mutex_lock(&logger->module_mutex);
    snprintf(module_buf, sizeof(module_buf), "%s", logger->module);
    clog_mutex_unlock(&logger->module_mutex);

    log_record_t record;
    memset(&record, 0, sizeof(record));
    record.level     = level;
    record.timestamp = clog_get_timestamp_us();
    record.tid       = clog_get_thread_id();
    record.pid       = clog_getpid();
    record.file      = file;
    record.func      = func;
    record.line      = line;
    record.module    = module_buf;
    record.tag       = NULL;
    record.message   = msg ? msg : "";

    size_t count    = kv_count > CLOG_MAX_KV ? CLOG_MAX_KV : kv_count;
    record.kv_count = count;
    for (size_t i = 0; i < count; i++) {
        record.kv[i] = kvs[i];
    }

    if (g_has_thread_trace_context) {
        memcpy(record.trace_id, g_thread_trace_id, 16);
        memcpy(record.span_id, g_thread_span_id, 8);
    } else {
        memset(record.trace_id, 0, 16);
        memset(record.span_id, 0, 8);
    }

    uint64_t suppressed = 0;
    if (!log_rate_limit_allow_for(logger, &suppressed)) {
        return;
    }

    if (suppressed > 0) {
        char supp_msg[128];
        snprintf(supp_msg,
                 sizeof(supp_msg),
                 "[clogx] Suppressed %llu log messages due to rate limit",
                 (unsigned long long)suppressed);
        log_record_t supp_rec = record;
        supp_rec.level        = LOG_LEVEL_WARN;
        supp_rec.message      = supp_msg;
        supp_rec.kv_count     = 0;
        if (logger->config.async) {
            if (log_async_write_for(logger, &supp_rec) != 0) {
                void (*cb)(void) = logger->async_fallback_cb;
                if (cb) {
                    cb();
                }
            }
        } else {
            log_dispatcher_dispatch_for(logger, &supp_rec);
        }
    }

    if (logger->config.async) {
        int ar = log_async_write_for(logger, &record);
        if (ar != 0) {
            clog_atomic_inc64(&logger->dropped_queue_full);
            void (*cb)(void) = logger->async_fallback_cb;
            if (cb) {
                cb();
            }
        }
    } else {
        log_dispatcher_dispatch_for(logger, &record);
    }
}

void log_write_kv(log_level_t      level,
                  const char      *file,
                  int              line,
                  const char      *func,
                  const char      *msg,
                  const clog_kv_t *kvs,
                  size_t           kv_count)
{
    logger_write_kv_internal(&g_default_logger, level, file, line, func, msg, kvs, kv_count);
}

void logger_write_kv(logger_t        *logger,
                     log_level_t      level,
                     const char      *file,
                     int              line,
                     const char      *func,
                     const char      *msg,
                     const clog_kv_t *kvs,
                     size_t           kv_count)
{
    logger_write_kv_internal(logger, level, file, line, func, msg, kvs, kv_count);
}

void log_get_stats(log_stats_t *stats)
{
    if (!stats) {
        return;
    }
    stats->total_logged_count       = clog_atomic_get64(&g_default_logger.total_logged);
    stats->dropped_queue_full_count = clog_atomic_get64(&g_default_logger.dropped_queue_full);
    stats->suppressed_rate_count    = log_rate_limit_get_total_suppressed_for(&g_default_logger);
    stats->current_queue_depth      = log_async_get_queue_depth_for(&g_default_logger);
}

#define MAX_THREAD_CONTEXT_PAIRS 16

typedef struct {
    char key[32];
    char value[128];
} thread_context_pair_t;

static clog_thread_local thread_context_pair_t g_thread_context[MAX_THREAD_CONTEXT_PAIRS];
static clog_thread_local size_t                g_thread_context_count = 0;

/**
 * @brief Set a key-value pair in the calling thread's MDC context.
 *
 * Mapped Diagnostic Context (MDC) allows attaching structured metadata
 * (e.g. request_id, user_id) to all log lines from the current thread
 * without passing them through every function call. The context is
 * thread-local and survives across log calls.
 *
 * @param key    Context key (must not be empty; max 31 chars).
 * @param value  Context value (empty string or NULL removes the key).
 * @return CLOG_OK on success, CLOG_ERR_INVALID_ARG on bad input or
 *         if the context is full (max 16 pairs).
 *
 * @note Thread-local: each thread has its own independent context.
 */
clogx_errno_t log_set_thread_context(const char *key, const char *value)
{
    if (!key || strlen(key) == 0) {
        return CLOG_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < g_thread_context_count; i++) {
        if (strcmp(g_thread_context[i].key, key) == 0) {
            if (!value || strlen(value) == 0) {
                g_thread_context[i] = g_thread_context[g_thread_context_count - 1];
                g_thread_context_count--;
            } else {
                snprintf(g_thread_context[i].value, sizeof(g_thread_context[i].value), "%s", value);
            }
            return CLOG_OK;
        }
    }

    if (!value || strlen(value) == 0) {
        return CLOG_OK;
    }

    if (g_thread_context_count >= MAX_THREAD_CONTEXT_PAIRS) {
        return CLOG_ERR_INVALID_ARG;
    }

    snprintf(g_thread_context[g_thread_context_count].key,
             sizeof(g_thread_context[g_thread_context_count].key),
             "%s",
             key);
    snprintf(g_thread_context[g_thread_context_count].value,
             sizeof(g_thread_context[g_thread_context_count].value),
             "%s",
             value);
    g_thread_context_count++;
    return CLOG_OK;
}

const char *log_get_thread_context(const char *key)
{
    if (!key) {
        return NULL;
    }
    for (size_t i = 0; i < g_thread_context_count; i++) {
        if (strcmp(g_thread_context[i].key, key) == 0) {
            return g_thread_context[i].value;
        }
    }
    return NULL;
}

void log_clear_thread_context(void)
{
    g_thread_context_count = 0;
}

void clog_set_trace_context(const uint8_t trace_id[16], const uint8_t span_id[8])
{
    if (trace_id && span_id) {
        memcpy(g_thread_trace_id, trace_id, 16);
        memcpy(g_thread_span_id, span_id, 8);
        g_has_thread_trace_context = true;
    } else {
        clog_clear_trace_context();
    }
}

void clog_get_trace_context(uint8_t trace_id[16], uint8_t span_id[8])
{
    if (trace_id) {
        if (g_has_thread_trace_context) {
            memcpy(trace_id, g_thread_trace_id, 16);
        } else {
            memset(trace_id, 0, 16);
        }
    }
    if (span_id) {
        if (g_has_thread_trace_context) {
            memcpy(span_id, g_thread_span_id, 8);
        } else {
            memset(span_id, 0, 8);
        }
    }
}

void clog_clear_trace_context(void)
{
    memset(g_thread_trace_id, 0, 16);
    memset(g_thread_span_id, 0, 8);
    g_has_thread_trace_context = false;
}

/**
 * @brief Convert a hex character to its 4-bit numeric value.
 *
 * @param c  Hex character ('0'-'9', 'a'-'f', or 'A'-'F').
 * @return 0-15 on valid input, -1 otherwise.
 */
static int parse_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/**
 * @brief Set W3C Trace Context identifiers from hex-encoded strings.
 *
 * Parses a 32-char lowercase hex trace_id and 16-char lowercase hex span_id
 * (as used in the `traceparent` header: `00-{trace_id}-{span_id}-{flags}`).
 * Invalid hex characters or short strings return CLOG_ERR_INVALID_ARG.
 *
 * @param trace_id_hex  32-character hex string (W3C trace-id).
 * @param span_id_hex   16-character hex string (W3C parent-id).
 * @return CLOG_OK on success or if both strings are empty (clears context),
 *         CLOG_ERR_INVALID_ARG on invalid input.
 */
clogx_errno_t clog_set_trace_context_hex(const char *trace_id_hex, const char *span_id_hex)
{
    if (!trace_id_hex || !span_id_hex) {
        clog_clear_trace_context();
        return CLOG_OK;
    }
    if (strlen(trace_id_hex) == 0 && strlen(span_id_hex) == 0) {
        clog_clear_trace_context();
        return CLOG_OK;
    }
    if (strlen(trace_id_hex) < 32 || strlen(span_id_hex) < 16) {
        return CLOG_ERR_INVALID_ARG;
    }

    uint8_t tid[16];
    uint8_t sid[8];
    for (int i = 0; i < 16; i++) {
        int hi = parse_hex_nibble(trace_id_hex[(size_t)i * 2]);
        int lo = parse_hex_nibble(trace_id_hex[(size_t)i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return CLOG_ERR_INVALID_ARG;
        }
        tid[i] = (uint8_t)((hi << 4) | lo);
    }
    for (int i = 0; i < 8; i++) {
        int hi = parse_hex_nibble(span_id_hex[(size_t)i * 2]);
        int lo = parse_hex_nibble(span_id_hex[(size_t)i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return CLOG_ERR_INVALID_ARG;
        }
        sid[i] = (uint8_t)((hi << 4) | lo);
    }

    clog_set_trace_context(tid, sid);
    return CLOG_OK;
}

#ifndef _WIN32
static pthread_once_t g_atfork_once = PTHREAD_ONCE_INIT;

/**
 * @brief POSIX atfork prepare handler: acquire all locks before fork().
 *
 * Called in the parent process before fork(). Acquires g_init_mutex,
 * module_mutex, and dispatcher lock in a consistent order to prevent
 * deadlock if the child inherits locked mutexes.
 */
static void log_atfork_prepare(void)
{
    clog_mutex_lock(&g_init_mutex);
    clog_mutex_lock(&g_default_logger.module_mutex);
    log_dispatcher_atfork_prepare_for(&g_default_logger);
}

/**
 * @brief POSIX atfork parent handler: release locks after fork() in parent.
 *
 * Called in the parent process after fork(). Releases dispatcher lock,
 * module_mutex, and g_init_mutex in reverse order of acquisition.
 */
static void log_atfork_parent(void)
{
    log_dispatcher_atfork_parent_for(&g_default_logger);
    clog_mutex_unlock(&g_default_logger.module_mutex);
    clog_mutex_unlock(&g_init_mutex);
}

/**
 * @brief POSIX atfork child handler: release locks and restart async worker.
 *
 * Called in the child process after fork(). Releases all locks held by
 * the parent at fork time, then restarts the async worker thread which
 * was not inherited across fork().
 */
static void log_atfork_child(void)
{
    log_dispatcher_atfork_child_for(&g_default_logger);
    clog_mutex_unlock(&g_default_logger.module_mutex);
    clog_mutex_unlock(&g_init_mutex);
    log_async_atfork_child_for(&g_default_logger);
}

/**
 * @brief Register pthread_atfork handlers for fork safety.
 *
 * Ensures the async worker, mutexes, and thread-local trace context
 * are properly cleaned up after fork.  Called once at first init.
 */
static void register_atfork(void)
{
    pthread_atfork(log_atfork_prepare, log_atfork_parent, log_atfork_child);
}
#endif

/**
 * @brief Initialise the global singleton logger from a YAML config file.
 *
 * Must be called before any LOG_* macros. Calling a second time without
 * intervening log_destroy() returns CLOG_ERR_INIT_REENTRANT.
 *
 * On POSIX, registers pthread_atfork handlers to maintain lock consistency
 * across fork() and restart the async worker in the child process.
 *
 * @param yaml_path  Path to YAML config file. Pass NULL for built-in defaults
 *                   (console sink, INFO level, sync mode).
 * @return CLOG_OK on success, or a negative clogx_errno_t code.
 *
 * @pre  No other thread is calling LOG_* yet.
 * @post Logger is fully initialised; log_destroy() must be called before exit.
 */
int log_init(const char *yaml_path)
{
#ifndef _WIN32
    pthread_once(&g_atfork_once, register_atfork);
#endif

    clog_mutex_lock(&g_init_mutex);
    if (g_default_logger.initialized) {
        clog_mutex_unlock(&g_init_mutex);
        return CLOG_ERR_INIT_REENTRANT;
    }

    int ret = logger_init_internal(&g_default_logger, yaml_path);
    if (ret != CLOG_OK) {
        clog_mutex_unlock(&g_init_mutex);
        return ret;
    }

    if (g_default_logger.config.catch_signals) {
        log_install_signal_handlers();
    }

    if (g_default_logger.config.prometheus_enable) {
        clog_prometheus_exporter_start(g_default_logger.config.prometheus_port);
    }
    clog_mutex_unlock(&g_init_mutex);
    return CLOG_OK;
}

/**
 * @brief Tear down the global singleton logger and release all resources.
 *
 * Flushes the async worker (if running), destroys all sinks, resets the
 * rate limiter, and unloads plugin handles. Safe to call even if the
 * logger was never initialised (no-op in that case).
 *
 * @note After log_destroy(), log_init() may be called again to reinitialise.
 *       Not thread-safe with concurrent LOG_* calls — stop all logging first.
 */
void log_destroy(void)
{
    bool was_initialized = false;

    clog_mutex_lock(&g_init_mutex);
    if (g_default_logger.initialized) {
        g_default_logger.initialized = false;
        was_initialized              = true;
    }
    clog_mutex_unlock(&g_init_mutex);

    if (was_initialized) {
        clog_prometheus_exporter_stop();
        log_restore_signal_handlers();
        log_async_shutdown_for(&g_default_logger);
        log_dispatcher_destroy_for(&g_default_logger);
        log_rate_limit_reset_for(&g_default_logger);
        log_plugin_shutdown_all();
    }
}

/**
 * @brief Flush all pending log output to sinks.
 *
 * In async mode, blocks until the async worker has drained all queued
 * records. In sync mode, flushes each sink's underlying FILE* handle.
 *
 * @note Safe to call from signal handler context (POSIX) as it delegates
 *       to the signal-safe flush path. Timeout: 2000 ms.
 */
void log_flush(void)
{
    if (g_default_logger.config.async) {
        log_async_flush_for(&g_default_logger);
    } else {
        log_dispatcher_flush_for(&g_default_logger);
    }
}

/**
 * @brief Hot-reload the logger configuration without full restart.
 *
 * Re-reads the YAML config file, rebuilds the rate limiter, takes a
 * new sink snapshot, shuts down the old async worker, commits the new
 * snapshot, and restarts the async worker if needed. Existing log calls
 * in-flight during reload use the old sink set until the commit.
 *
 * @return CLOG_OK on success, or CLOG_ERR_RELOAD if the logger is not
 *         initialised, or CLOG_ERR_NO_SINKS if the new config has no sinks.
 */
int log_reload(void)
{
    clog_mutex_lock(&g_init_mutex);
    if (!g_default_logger.initialized) {
        clog_mutex_unlock(&g_init_mutex);
        return CLOG_ERR_RELOAD;
    }
    clog_mutex_unlock(&g_init_mutex);

    int ret = log_config_load_into(&g_default_logger, g_default_logger.config_path);
    if (ret != 0) {
        return CLOG_ERR_CONFIG_OPEN;
    }

    log_rate_limit_init_for(&g_default_logger,
                            g_default_logger.config.rate_limit_enable,
                            g_default_logger.config.rate_limit_max_per_sec,
                            g_default_logger.config.rate_limit_burst);

    log_dispatcher_snapshot_t snap = {0};
    ret = log_dispatcher_build_snapshot_for(&g_default_logger, &g_default_logger.config, &snap);
    if (ret != 0) {
        return CLOG_ERR_NO_SINKS;
    }

    log_async_shutdown_for(&g_default_logger);
    log_dispatcher_commit_snapshot_for(&g_default_logger, &snap);
    log_dispatcher_destroy_snapshot(&snap);

    if (g_default_logger.config.async) {
        if (log_async_init_for(&g_default_logger, g_default_logger.config.queue_size) != 0) {
            return CLOG_ERR_THREAD_CREATE;
        }
    }

    return CLOG_OK;
}

/* ════════════════════════════════════════════════════════════════════════
 *  Multi-instance API (Phase 2)
 * ════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Create an independent logger instance from a YAML config file.
 *
 * Allocates and initialises a new `logger_t` that operates independently
 * of the global singleton. Useful for multi-component applications where
 * different subsystems need separate log levels, sinks, or formats.
 *
 * @param yaml_path  Path to YAML config file (must not be NULL).
 * @return Pointer to the new logger, or NULL on allocation/init failure.
 *
 * @note The returned logger must be freed with logger_destroy().
 */
logger_t *logger_create(const char *yaml_path)
{
    logger_t *logger = (logger_t *)calloc(1, sizeof(logger_t));
    if (!logger) {
        return NULL;
    }

    clog_mutex_init(&logger->dispatcher_mutex);
    clog_mutex_init(&logger->rl_mutex);
    clog_mutex_init(&logger->fmt_mutex);
    clog_mutex_init(&logger->module_mutex);

    if (logger_init_internal(logger, yaml_path) != CLOG_OK) {
        clog_mutex_destroy(&logger->dispatcher_mutex);
        clog_mutex_destroy(&logger->rl_mutex);
        clog_mutex_destroy(&logger->fmt_mutex);
        clog_mutex_destroy(&logger->module_mutex);
        free(logger);
        return NULL;
    }

    return logger;
}

/**
 * @brief Create an independent logger instance from an in-memory config.
 *
 * Unlike logger_create(), this does not touch the filesystem — the
 * config is provided directly as a struct. Format and time_format strings
 * are copied into the logger's internal buffers to ensure lifetime safety.
 *
 * @param cfg  Non-NULL config struct. Internal pointers (format, time_format)
 *             are copied; the caller retains ownership.
 * @return Pointer to the new logger, or NULL on failure.
 */
logger_t *logger_create_from_config(const log_config_t *cfg)
{
    if (!cfg) {
        return NULL;
    }

    logger_t *logger = (logger_t *)calloc(1, sizeof(logger_t));
    if (!logger) {
        return NULL;
    }

    clog_mutex_init(&logger->dispatcher_mutex);
    clog_mutex_init(&logger->rl_mutex);
    clog_mutex_init(&logger->fmt_mutex);
    clog_mutex_init(&logger->module_mutex);
    clog_rwlock_init(&logger->config_rwlock);

    logger->config = *cfg;

    if (cfg->format) {
        snprintf(logger->format_str, sizeof(logger->format_str), "%s", cfg->format);
        logger->config.format = logger->format_str;
    }
    if (cfg->time_format) {
        snprintf(logger->time_format_str, sizeof(logger->time_format_str), "%s", cfg->time_format);
        logger->config.time_format = logger->time_format_str;
    }

    log_formatter_init_for(logger, cfg->format, cfg->time_format);
    log_rate_limit_init_for(
        logger, cfg->rate_limit_enable, cfg->rate_limit_max_per_sec, cfg->rate_limit_burst);

    if (log_dispatcher_init_for(logger) != 0) {
        clog_mutex_destroy(&logger->dispatcher_mutex);
        clog_mutex_destroy(&logger->rl_mutex);
        clog_mutex_destroy(&logger->fmt_mutex);
        clog_mutex_destroy(&logger->module_mutex);
        clog_rwlock_destroy(&logger->config_rwlock);
        free(logger);
        return NULL;
    }

    if (cfg->async) {
        if (log_async_init_for(logger, cfg->queue_size) != 0) {
            log_dispatcher_destroy_for(logger);
            clog_mutex_destroy(&logger->dispatcher_mutex);
            clog_mutex_destroy(&logger->rl_mutex);
            clog_mutex_destroy(&logger->fmt_mutex);
            clog_mutex_destroy(&logger->module_mutex);
            clog_rwlock_destroy(&logger->config_rwlock);
            free(logger);
            return NULL;
        }
    }

    logger_set_module_internal(logger, "main");
    logger->initialized = true;
    return logger;
}

void logger_destroy(logger_t *logger)
{
    if (!logger) {
        return;
    }

    log_async_shutdown_for(logger);
    log_dispatcher_destroy_for(logger);
    log_rate_limit_reset_for(logger);

    clog_mutex_destroy(&logger->dispatcher_mutex);
    clog_mutex_destroy(&logger->rl_mutex);
    clog_mutex_destroy(&logger->fmt_mutex);
    clog_mutex_destroy(&logger->module_mutex);

    free(logger);
}

void logger_flush(logger_t *logger)
{
    if (!logger) {
        return;
    }
    if (logger->config.async) {
        log_async_flush_for(logger);
    } else {
        log_dispatcher_flush_for(logger);
    }
}

int logger_reload(logger_t *logger)
{
    if (!logger || !logger->initialized) {
        return CLOG_ERR_RELOAD;
    }

    int ret = log_config_load_into(logger, logger->config_path);
    if (ret != 0) {
        return CLOG_ERR_CONFIG_OPEN;
    }

    log_rate_limit_init_for(logger,
                            logger->config.rate_limit_enable,
                            logger->config.rate_limit_max_per_sec,
                            logger->config.rate_limit_burst);

    log_dispatcher_snapshot_t snap = {0};
    ret = log_dispatcher_build_snapshot_for(logger, &logger->config, &snap);
    if (ret != 0) {
        return CLOG_ERR_NO_SINKS;
    }

    log_async_shutdown_for(logger);
    log_dispatcher_commit_snapshot_for(logger, &snap);
    log_dispatcher_destroy_snapshot(&snap);

    if (logger->config.async) {
        if (log_async_init_for(logger, logger->config.queue_size) != 0) {
            return CLOG_ERR_THREAD_CREATE;
        }
    }

    return CLOG_OK;
}

int logger_add_sink(logger_t *logger, log_sink_t *sink)
{
    if (!logger || !sink) {
        return CLOG_ERR_INVALID_ARG;
    }
    if (!logger->initialized) {
        return CLOG_ERR_RELOAD;
    }
    return log_dispatcher_add_sink_for(logger, sink) == 0 ? CLOG_OK : CLOG_ERR_OOM;
}

int logger_remove_sink(logger_t *logger, log_sink_t *sink)
{
    if (!logger || !sink) {
        return CLOG_ERR_INVALID_ARG;
    }
    return log_dispatcher_remove_sink_for(logger, sink) == 0 ? CLOG_OK : CLOG_ERR_INVALID_ARG;
}

int logger_set_level(logger_t *logger, log_level_t level)
{
    if (!logger) {
        return -1;
    }
    logger->config.level = level;
    return 0;
}

log_level_t logger_get_level(const logger_t *logger)
{
    return logger ? logger->config.level : LOG_LEVEL_INFO;
}

void logger_set_module(logger_t *logger, const char *module)
{
    if (logger) {
        logger_set_module_internal(logger, module);
    }
}

void logger_get_module(const logger_t *logger, char *buf, size_t n)
{
    if (!logger || !buf || n == 0) {
        return;
    }
    clog_mutex_lock(&((logger_t *)logger)->module_mutex);
    snprintf(buf, n, "%s", logger->module);
    clog_mutex_unlock(&((logger_t *)logger)->module_mutex);
}

void logger_get_stats(const logger_t *logger, log_stats_t *stats)
{
    if (!logger || !stats) {
        return;
    }
    stats->total_logged_count       = logger->total_logged;
    stats->dropped_queue_full_count = logger->dropped_queue_full;
    stats->suppressed_rate_count    = log_rate_limit_get_total_suppressed_for((logger_t *)logger);
    stats->current_queue_depth      = log_async_get_queue_depth_for((logger_t *)logger);
}

int logger_config_set(logger_t *logger, const log_config_t *cfg)
{
    if (!logger || !cfg) {
        return -1;
    }
    logger->config = *cfg;
    if (cfg->format) {
        snprintf(logger->format_str, sizeof(logger->format_str), "%s", cfg->format);
        logger->config.format = logger->format_str;
    }
    if (cfg->time_format) {
        snprintf(logger->time_format_str, sizeof(logger->time_format_str), "%s", cfg->time_format);
        logger->config.time_format = logger->time_format_str;
    }
    return 0;
}

log_config_t *logger_config_get(const logger_t *logger)
{
    return logger ? (log_config_t *)&logger->config : NULL;
}
