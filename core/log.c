#define _POSIX_C_SOURCE 200809L
/**
 * @file log.c
 * @brief Public logging entry points: init/destroy/reload/flush and write path.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include "log.h"
#include "log_config.h"
#include "log_formatter.h"
#include "dispatcher.h"
#include "log_async.h"
#include "log_record.h"

static pthread_mutex_t g_init_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_initialized = 0;

/** Wall-clock timestamp in microseconds since the Unix epoch. */
static inline uint64_t get_timestamp(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

/** Truncated pthread_t suitable for %thread formatting. */
static inline uint32_t get_thread_id(void) {
    pthread_t self = pthread_self();
    uint32_t h = (uint32_t)((uintptr_t)self >> 32);
    uint32_t l = (uint32_t)(uintptr_t)self;
    return (h ^ l ^ 0x9e3779b9u) + 1u;
}

void log_writevprintf(
    log_level_t level,
    const char *file,
    int line,
    const char *func,
    const char *fmt,
    ...) {

    if (level < log_get_level()) {
        return;
    }

    log_config_t *cfg = log_config_get();

    va_list args;
    va_start(args, fmt);

    char message[1024];
    int ret = vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    if (ret < 0 || ret >= (int)sizeof(message)) {
        message[sizeof(message) - 1] = '\0';
    }

    /*
     * record.message / file / func point at caller stack or static storage.
     * Async mode must deep-copy before the caller returns (see log_async_write).
     */
    log_record_t record;
    record.level = level;
    record.timestamp = get_timestamp();
    record.tid = get_thread_id();
    record.pid = (uint32_t)getpid();
    record.file = file;
    record.func = func;
    record.line = line;
    record.module = "main";
    record.tag = NULL;
    record.message = message;

    if (cfg->async) {
        log_async_write(&record);
    } else {
        log_dispatcher_dispatch(&record);
    }
}

int log_init(const char *yaml_path) {
    pthread_mutex_lock(&g_init_mutex);
    if (g_initialized) {
        pthread_mutex_unlock(&g_init_mutex);
        fprintf(stderr, "log_init called without log_destroy\n");
        return -1;
    }

    if (!yaml_path) yaml_path = "";

    if (log_config_init(yaml_path) != 0) {
        pthread_mutex_unlock(&g_init_mutex);
        return -1;
    }

    log_config_t *cfg = log_config_get();
    log_formatter_init(cfg->format);

    if (log_dispatcher_init() != 0) {
        pthread_mutex_unlock(&g_init_mutex);
        return -1;
    }

    if (cfg->async) {
        if (log_async_init(cfg->queue_size) != 0) {
            log_destroy();
            pthread_mutex_unlock(&g_init_mutex);
            return -1;
        }
    }

    g_initialized = 1;
    pthread_mutex_unlock(&g_init_mutex);
    return 0;
}

void log_destroy(void) {
    pthread_mutex_lock(&g_init_mutex);
    g_initialized = 0;
    pthread_mutex_unlock(&g_init_mutex);

    log_async_shutdown();
    log_dispatcher_destroy();
}

void log_flush(void) {
    log_config_t *cfg = log_config_get();
    if (cfg->async) {
        log_async_flush();
    } else {
        log_dispatcher_flush();
    }
}

int log_reload(void) {
    /* Stop async worker before rebuilding sinks to avoid use-after-free. */
    log_async_shutdown();

    int ret = log_config_reload();
    if (ret != 0) {
        return ret;
    }

    log_config_t *cfg = log_config_get();
    log_formatter_init(cfg->format);
    log_dispatcher_destroy();
    if (log_dispatcher_init() != 0) {
        return -1;
    }

    if (cfg->async) {
        if (log_async_init(cfg->queue_size) != 0) {
            return -1;
        }
    }

    return 0;
}