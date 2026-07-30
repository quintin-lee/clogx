/**
 * @file log.c
 * @brief Public logging entry points: init/destroy/reload/flush and write path.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "clog_port.h"
#include "log.h"
#include "log_config.h"
#include "log_formatter.h"
#include "log_limits.h"
#include "log_rate_limit.h"
#include "log_signal.h"
#include "dispatcher.h"
#include "log_async.h"
#include "log_record.h"
#include "plugin_loader.h"
#include "log_internal.h"

logger_t g_default_logger = {0};

static clog_mutex_t g_init_mutex = CLOG_MUTEX_INITIALIZER;
volatile uint64_t g_prometheus_level_counts[6] = {0};

static clog_thread_local uint8_t g_thread_trace_id[16];
static clog_thread_local uint8_t g_thread_span_id[8];
static clog_thread_local bool g_has_thread_trace_context = false;

const char *log_strerror(int err) {
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

void log_set_async_fallback_cb(void (*cb)(void)) {
    g_default_logger.async_fallback_cb = cb;
}

void (*log_get_async_fallback_cb(void))(void) {
    return g_default_logger.async_fallback_cb;
}

static void logger_set_module_internal(logger_t *logger, const char *module) {
    clog_mutex_lock(&logger->module_mutex);
    if (!module || !*module) {
        snprintf(logger->module, sizeof(logger->module), "%s", "main");
    } else {
        snprintf(logger->module, sizeof(logger->module), "%s", module);
    }
    clog_mutex_unlock(&logger->module_mutex);
}

void log_set_module(const char *module) {
    logger_set_module_internal(&g_default_logger, module);
}

void log_get_module(char *buf, size_t n) {
    if (!buf || n == 0)
        return;
    clog_mutex_lock(&g_default_logger.module_mutex);
    snprintf(buf, n, "%s", g_default_logger.module);
    clog_mutex_unlock(&g_default_logger.module_mutex);
}

int log_add_sink(log_sink_t *sink) {
    if (!sink)
        return CLOG_ERR_INVALID_ARG;

    clog_mutex_lock(&g_init_mutex);
    if (!g_default_logger.initialized) {
        clog_mutex_unlock(&g_init_mutex);
        return CLOG_ERR_RELOAD;
    }
    int ret = log_dispatcher_add_sink_for(&g_default_logger, sink);
    clog_mutex_unlock(&g_init_mutex);
    return ret == 0 ? CLOG_OK : CLOG_ERR_OOM;
}

int log_remove_sink(log_sink_t *sink) {
    if (!sink)
        return CLOG_ERR_INVALID_ARG;
    if (log_dispatcher_remove_sink_for(&g_default_logger, sink) != 0)
        return CLOG_ERR_INVALID_ARG;
    return CLOG_OK;
}

/** Wall-clock timestamp in microseconds since the Unix epoch. */
static inline uint64_t get_timestamp(void) {
#if defined(_WIN32) || defined(_WIN64)
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    /* Convert 100ns intervals since Jan 1, 1601 to microseconds since Jan 1, 1970 */
    return (t - 116444736000000000ULL) / 10;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
#endif
}

/** Truncated thread ID suitable for %thread formatting. */
static inline uint32_t get_thread_id(void) {
#if defined(_WIN32) || defined(_WIN64)
    return (uint32_t)GetCurrentThreadId();
#else
    pthread_t self = pthread_self();
    uint32_t h = (uint32_t)((uintptr_t)self >> 32);
    uint32_t l = (uint32_t)(uintptr_t)self;
    return (h ^ l ^ 0x9e3779b9u) + 1u;
#endif
}

static int logger_init_internal(logger_t *logger, const char *yaml_path) {
    clog_rwlock_init(&logger->config_rwlock);

    if (log_config_load_into(logger, yaml_path) != 0)
        return CLOG_ERR_CONFIG_OPEN;

    log_rate_limit_init_for(logger, logger->config.rate_limit_enable,
                            logger->config.rate_limit_max_per_sec, logger->config.rate_limit_burst);

    if (log_dispatcher_init_for(logger) != 0)
        return CLOG_ERR_NO_SINKS;

    if (logger->config.async) {
        if (log_async_init_for(logger, logger->config.queue_size) != 0) {
            log_dispatcher_destroy_for(logger);
            return CLOG_ERR_THREAD_CREATE;
        }
    }

    logger_set_module_internal(logger, "main");
    logger->initialized = true;
    return CLOG_OK;
}

void log_writevprintf(log_level_t level, const char *file, int line, const char *func,
                      const char *fmt, ...) {
    if (log_get_pending_signal() != 0) {
        log_process_pending_signals();
    }

    if (level < g_default_logger.config.level) {
        return;
    }

    g_default_logger.total_logged++;
    if ((int)level >= 0 && (int)level < 6) {
        g_prometheus_level_counts[(int)level]++;
    }

    char message[CLOG_MAX_MESSAGE_SIZE];

    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(message, sizeof(message), fmt,
                        args); /* NOLINT(clang-analyzer-valist.Uninitialized) */
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
    clog_mutex_lock(&g_default_logger.module_mutex);
    snprintf(module_buf, sizeof(module_buf), "%s", g_default_logger.module);
    clog_mutex_unlock(&g_default_logger.module_mutex);

    log_record_t record;
    record.level = level;
    record.timestamp = get_timestamp();
    record.tid = get_thread_id();
    record.pid = clog_getpid();
    record.file = file;
    record.func = func;
    record.line = line;
    record.module = module_buf;
    record.tag = NULL;
    record.message = message;
    if (g_has_thread_trace_context) {
        memcpy(record.trace_id, g_thread_trace_id, 16);
        memcpy(record.span_id, g_thread_span_id, 8);
    } else {
        memset(record.trace_id, 0, 16);
        memset(record.span_id, 0, 8);
    }

    uint64_t suppressed = 0;
    if (!log_rate_limit_allow_for(&g_default_logger, &suppressed)) {
        return;
    }

    if (suppressed > 0) {
        char supp_msg[128];
        snprintf(supp_msg, sizeof(supp_msg),
                 "[clogx] Suppressed %llu log messages due to rate limit",
                 (unsigned long long)suppressed);
        log_record_t supp_rec = record;
        supp_rec.level = LOG_LEVEL_WARN;
        supp_rec.message = supp_msg;
        if (g_default_logger.config.async) {
            if (log_async_write_for(&g_default_logger, &supp_rec) != 0) {
                void (*cb)(void) = g_default_logger.async_fallback_cb;
                if (cb)
                    cb();
            }
        } else {
            log_dispatcher_dispatch_for(&g_default_logger, &supp_rec);
        }
    }

    if (g_default_logger.config.async) {
        int ar = log_async_write_for(&g_default_logger, &record);
        if (ar != 0) {
            g_default_logger.dropped_queue_full++;
            void (*cb)(void) = g_default_logger.async_fallback_cb;
            if (cb)
                cb();
        }
    } else {
        log_dispatcher_dispatch_for(&g_default_logger, &record);
    }
}

void log_get_stats(log_stats_t *stats) {
    if (!stats)
        return;
    stats->total_logged_count = g_default_logger.total_logged;
    stats->dropped_queue_full_count = g_default_logger.dropped_queue_full;
    stats->suppressed_rate_count = log_rate_limit_get_total_suppressed_for(&g_default_logger);
    stats->current_queue_depth = log_async_get_queue_depth_for(&g_default_logger);
}

#define MAX_THREAD_CONTEXT_PAIRS 16

typedef struct {
    char key[32];
    char value[128];
} thread_context_pair_t;

static clog_thread_local thread_context_pair_t g_thread_context[MAX_THREAD_CONTEXT_PAIRS];
static clog_thread_local size_t g_thread_context_count = 0;

clogx_errno_t log_set_thread_context(const char *key, const char *value) {
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
             sizeof(g_thread_context[g_thread_context_count].key), "%s", key);
    snprintf(g_thread_context[g_thread_context_count].value,
             sizeof(g_thread_context[g_thread_context_count].value), "%s", value);
    g_thread_context_count++;
    return CLOG_OK;
}

const char *log_get_thread_context(const char *key) {
    if (!key)
        return NULL;
    for (size_t i = 0; i < g_thread_context_count; i++) {
        if (strcmp(g_thread_context[i].key, key) == 0) {
            return g_thread_context[i].value;
        }
    }
    return NULL;
}

void log_clear_thread_context(void) {
    g_thread_context_count = 0;
}

void clog_set_trace_context(const uint8_t trace_id[16], const uint8_t span_id[8]) {
    if (trace_id && span_id) {
        memcpy(g_thread_trace_id, trace_id, 16);
        memcpy(g_thread_span_id, span_id, 8);
        g_has_thread_trace_context = true;
    } else {
        clog_clear_trace_context();
    }
}

void clog_get_trace_context(uint8_t trace_id[16], uint8_t span_id[8]) {
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

void clog_clear_trace_context(void) {
    memset(g_thread_trace_id, 0, 16);
    memset(g_thread_span_id, 0, 8);
    g_has_thread_trace_context = false;
}

static int parse_hex_nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

clogx_errno_t clog_set_trace_context_hex(const char *trace_id_hex, const char *span_id_hex) {
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
        if (hi < 0 || lo < 0)
            return CLOG_ERR_INVALID_ARG;
        tid[i] = (uint8_t)((hi << 4) | lo);
    }
    for (int i = 0; i < 8; i++) {
        int hi = parse_hex_nibble(span_id_hex[(size_t)i * 2]);
        int lo = parse_hex_nibble(span_id_hex[(size_t)i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return CLOG_ERR_INVALID_ARG;
        sid[i] = (uint8_t)((hi << 4) | lo);
    }

    clog_set_trace_context(tid, sid);
    return CLOG_OK;
}

#ifndef _WIN32
static pthread_once_t g_atfork_once = PTHREAD_ONCE_INIT;

static void log_atfork_prepare(void) {
    clog_mutex_lock(&g_init_mutex);
    clog_mutex_lock(&g_default_logger.module_mutex);
    log_dispatcher_atfork_prepare_for(&g_default_logger);
}

static void log_atfork_parent(void) {
    log_dispatcher_atfork_parent_for(&g_default_logger);
    clog_mutex_unlock(&g_default_logger.module_mutex);
    clog_mutex_unlock(&g_init_mutex);
}

static void log_atfork_child(void) {
    log_dispatcher_atfork_child_for(&g_default_logger);
    clog_mutex_unlock(&g_default_logger.module_mutex);
    clog_mutex_unlock(&g_init_mutex);
    log_async_atfork_child_for(&g_default_logger);
}

static void register_atfork(void) {
    pthread_atfork(log_atfork_prepare, log_atfork_parent, log_atfork_child);
}
#endif

int log_init(const char *yaml_path) {
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

void log_destroy(void) {
    bool was_initialized = false;

    clog_mutex_lock(&g_init_mutex);
    if (g_default_logger.initialized) {
        g_default_logger.initialized = false;
        was_initialized = true;
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

void log_flush(void) {
    if (g_default_logger.config.async) {
        log_async_flush_for(&g_default_logger);
    } else {
        log_dispatcher_flush_for(&g_default_logger);
    }
}

int log_reload(void) {
    clog_mutex_lock(&g_init_mutex);
    if (!g_default_logger.initialized) {
        clog_mutex_unlock(&g_init_mutex);
        return CLOG_ERR_RELOAD;
    }
    clog_mutex_unlock(&g_init_mutex);

    int ret = log_config_load_into(&g_default_logger, g_default_logger.config_path);
    if (ret != 0)
        return CLOG_ERR_CONFIG_OPEN;

    log_rate_limit_init_for(&g_default_logger, g_default_logger.config.rate_limit_enable,
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
        if (log_async_init_for(&g_default_logger, g_default_logger.config.queue_size) != 0)
            return CLOG_ERR_THREAD_CREATE;
    }

    return CLOG_OK;
}
