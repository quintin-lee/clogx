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

static clog_mutex_t g_init_mutex = CLOG_MUTEX_INITIALIZER;
static int g_initialized = 0;
static void (*g_async_fallback_cb)(void);
static clog_mutex_t g_module_mutex = CLOG_MUTEX_INITIALIZER;
static char g_module[64] = "main";

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
    g_async_fallback_cb = cb;
}

void (*log_get_async_fallback_cb(void))(void) {
    return g_async_fallback_cb;
}

void log_set_module(const char *module) {
    clog_mutex_lock(&g_module_mutex);
    if (!module || !*module) {
        snprintf(g_module, sizeof(g_module), "%s", "main");
    } else {
        snprintf(g_module, sizeof(g_module), "%s", module);
    }
    clog_mutex_unlock(&g_module_mutex);
}

void log_get_module(char *buf, size_t n) {
    if (!buf || n == 0)
        return;
    clog_mutex_lock(&g_module_mutex);
    snprintf(buf, n, "%s", g_module);
    clog_mutex_unlock(&g_module_mutex);
}

int log_add_sink(log_sink_t *sink) {
    if (!sink)
        return CLOG_ERR_INVALID_ARG;

    clog_mutex_lock(&g_init_mutex);
    if (!g_initialized) {
        clog_mutex_unlock(&g_init_mutex);
        return CLOG_ERR_RELOAD;
    }
    int ret = log_dispatcher_add_sink(sink);
    clog_mutex_unlock(&g_init_mutex);
    return ret == 0 ? CLOG_OK : CLOG_ERR_OOM;
}

int log_remove_sink(log_sink_t *sink) {
    if (!sink)
        return CLOG_ERR_INVALID_ARG;
    if (log_dispatcher_remove_sink(sink) != 0)
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

void log_writevprintf(log_level_t level, const char *file, int line, const char *func,
                      const char *fmt, ...) {
    if (log_get_pending_signal() != 0) {
        log_process_pending_signals();
    }

    if (level < log_get_level()) {
        return;
    }

    va_list args;
    va_start(args, fmt);

    char message[CLOG_MAX_MESSAGE_SIZE];
    int ret = vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    if (ret < 0) {
        message[0] = '\0';
    } else if (ret >= (int)sizeof(message)) {
        /* Mark truncated messages so callers can detect overflow. */
        if (sizeof(message) >= 4) {
            memcpy(message + sizeof(message) - 4, "...", 4);
        } else {
            message[sizeof(message) - 1] = '\0';
        }
    }

    char module_buf[64];
    log_get_module(module_buf, sizeof(module_buf));

    /*
     * record.message / file / func / module point at caller stack or static storage.
     * Async mode must deep-copy before the caller returns (see log_async_write).
     */
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

    uint64_t suppressed = 0;
    if (!log_rate_limit_allow(&suppressed)) {
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
        if (log_config_is_async()) {
            if (log_async_write(&supp_rec) != 0) {
                void (*cb)(void) = log_get_async_fallback_cb();
                if (cb)
                    cb();
            }
        } else {
            log_dispatcher_dispatch(&supp_rec);
        }
    }

    if (log_config_is_async()) {
        int ar = log_async_write(&record);
        if (ar != 0) {
            void (*cb)(void) = log_get_async_fallback_cb();
            if (cb)
                cb();
        }
    } else {
        log_dispatcher_dispatch(&record);
    }
}

#ifndef _WIN32
static pthread_once_t g_atfork_once = PTHREAD_ONCE_INIT;

static void log_atfork_prepare(void) {
    clog_mutex_lock(&g_init_mutex);
    clog_mutex_lock(&g_module_mutex);
    log_dispatcher_atfork_prepare();
}

static void log_atfork_parent(void) {
    log_dispatcher_atfork_parent();
    clog_mutex_unlock(&g_module_mutex);
    clog_mutex_unlock(&g_init_mutex);
}

static void log_atfork_child(void) {
    log_dispatcher_atfork_child();
    clog_mutex_unlock(&g_module_mutex);
    clog_mutex_unlock(&g_init_mutex);
    log_async_atfork_child();
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
    if (g_initialized) {
        clog_mutex_unlock(&g_init_mutex);
        return CLOG_ERR_INIT_REENTRANT;
    }

    if (!yaml_path)
        yaml_path = "";

    if (log_config_init(yaml_path) != 0) {
        clog_mutex_unlock(&g_init_mutex);
        return CLOG_ERR_CONFIG_OPEN;
    }

    log_config_t *cfg = log_config_get();
    log_formatter_init(cfg->format, cfg->time_format);
    log_rate_limit_init(cfg->rate_limit_enable, cfg->rate_limit_max_per_sec, cfg->rate_limit_burst);

    if (log_dispatcher_init() != 0) {
        clog_mutex_unlock(&g_init_mutex);
        return CLOG_ERR_NO_SINKS;
    }

    if (cfg->async) {
        if (log_async_init(cfg->queue_size) != 0) {
            log_destroy();
            clog_mutex_unlock(&g_init_mutex);
            return CLOG_ERR_THREAD_CREATE;
        }
    }

    if (cfg->catch_signals) {
        log_install_signal_handlers();
    }

    g_initialized = 1;
    clog_mutex_unlock(&g_init_mutex);
    return CLOG_OK;
}

void log_destroy(void) {
    clog_mutex_lock(&g_init_mutex);
    g_initialized = 0;
    clog_mutex_unlock(&g_init_mutex);

    log_restore_signal_handlers();
    log_rate_limit_reset();
    log_async_shutdown();
    log_dispatcher_destroy();
}

void log_flush(void) {
    if (log_config_is_async()) {
        log_async_flush();
    } else {
        log_dispatcher_flush();
    }
}

int log_reload(void) {
    clog_mutex_lock(&g_init_mutex);
    if (!g_initialized) {
        clog_mutex_unlock(&g_init_mutex);
        return CLOG_ERR_RELOAD;
    }
    clog_mutex_unlock(&g_init_mutex);

    int ret = log_config_reload();
    if (ret != 0) {
        return CLOG_ERR_CONFIG_OPEN;
    }

    log_config_t *cfg = log_config_get();
    log_formatter_init(cfg->format, cfg->time_format);
    log_rate_limit_init(cfg->rate_limit_enable, cfg->rate_limit_max_per_sec, cfg->rate_limit_burst);

    log_dispatcher_snapshot_t snap = {0};
    ret = log_dispatcher_build_snapshot(cfg, &snap);
    if (ret != 0) {
        return CLOG_ERR_NO_SINKS;
    }

    log_async_shutdown();
    log_dispatcher_commit_snapshot(&snap);
    log_dispatcher_destroy_snapshot(&snap);

    if (cfg->async) {
        if (log_async_init(cfg->queue_size) != 0) {
            return CLOG_ERR_THREAD_CREATE;
        }
    }

    return CLOG_OK;
}
