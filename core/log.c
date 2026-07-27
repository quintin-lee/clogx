#define _POSIX_C_SOURCE 200809L
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

static inline uint64_t get_timestamp(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static inline uint32_t get_thread_id(void) {
    pthread_t self = pthread_self();
    return (uint32_t)((uintptr_t)self % 0xFFFFFFFF);
}

void log_writevprintf(
    log_level_t level,
    const char *file,
    int line,
    const char *func,
    const char *fmt,
    ...) {

    log_config_t *cfg = log_config_get();
    if (level < cfg->level) {
        return;
    }

    // Use va_list to handle variable arguments
    va_list args;
    va_start(args, fmt);

    // Format the message using vsnprintf
    char message[1024];
    int ret = vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    if (ret < 0 || ret >= (int)sizeof(message)) {
        message[sizeof(message) - 1] = '\0';
    }

    // Create log record
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

    // Dispatch based on config
    if (cfg->async) {
        log_async_write(&record);
    } else {
        log_dispatcher_dispatch(&record);
    }
}

int log_init(const char *yaml_path) {
    if (!yaml_path) yaml_path = "";

    if (log_config_init(yaml_path) != 0) {
        return -1;
    }

    if (log_dispatcher_init() != 0) {
        return -1;
    }

    log_config_t *cfg = log_config_get();
    if (cfg->async) {
        if (log_async_init(cfg->queue_size) != 0) {
            log_destroy();
            return -1;
        }
    }

    return 0;
}

void log_destroy(void) {
    log_async_shutdown();
    log_dispatcher_destroy();
}

void log_flush(void) {
    log_dispatcher_flush();
}

int log_reload(void) {
    int ret = log_config_reload();
    if (ret == 0) {
        log_dispatcher_destroy();
        log_dispatcher_init();
    }
    return ret;
}