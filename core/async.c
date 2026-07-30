/**
 * @file async.c
 * @brief Async logger: single-allocation record cloning into a queue consumed by one worker.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "clog_port.h"
#include "log_async.h"
#include "queue.h"
#include "dispatcher.h"
#include "log_config.h"
#include "log.h"
#include "log_internal.h"

/* Legacy singleton state — kept until Task 1.8 removal pass. */
typedef struct {
    mpsc_queue_t *queue;
    clog_thread_t worker_thread;
    volatile int running;
    volatile int processing;
} async_logger_t;

static async_logger_t g_async_logger = {.queue = NULL, .running = 0, .processing = 0};

static void log_record_free_owned(log_record_t *record) {
    if (!record)
        return;
    const char *block =
        record->message ? record->message : (record->module ? record->module : record->tag);
    if (block)
        free((void *)block);
    record->message = NULL;
    record->file = NULL;
    record->func = NULL;
    record->module = NULL;
    record->tag = NULL;
}

static int log_record_clone(log_record_t *restrict dst, const log_record_t *restrict src) {
    if (!dst || !src)
        return -1;
    *dst = *src;
    dst->file = src->file;
    dst->func = src->func;

    size_t msg_len = src->message ? strlen(src->message) : 0;
    size_t mod_len = src->module ? strlen(src->module) : 0;
    size_t tag_len = src->tag ? strlen(src->tag) : 0;
    size_t total_bytes = (src->message ? msg_len + 1 : 0) + (src->module ? mod_len + 1 : 0) +
                         (src->tag ? tag_len + 1 : 0);

    if (total_bytes == 0) {
        dst->message = NULL;
        dst->module = NULL;
        dst->tag = NULL;
        return 0;
    }

    char *block = malloc(total_bytes);
    if (!block)
        return -1;

    char *p = block;
    if (src->message) {
        memcpy(p, src->message, msg_len + 1);
        dst->message = p;
        p += msg_len + 1;
    } else {
        dst->message = NULL;
    }
    if (src->module) {
        memcpy(p, src->module, mod_len + 1);
        dst->module = p;
        p += mod_len + 1;
    } else {
        dst->module = NULL;
    }
    if (src->tag) {
        memcpy(p, src->tag, tag_len + 1);
        dst->tag = p;
    } else {
        dst->tag = NULL;
    }
    return 0;
}

#define ASYNC_BATCH_SIZE 64

/* Worker that dispatches to a logger_t instance (used by _for variants). */
static void *async_worker_for(void *arg) {
    logger_t *logger = (logger_t *)arg;
    log_record_t batch[ASYNC_BATCH_SIZE];
    while (1) {
        int count = mpsc_queue_get_batch(logger->queue, batch, ASYNC_BATCH_SIZE);
        if (count <= 0)
            break;
        logger->async_processing = 1;
        for (int i = 0; i < count; i++) {
            log_dispatcher_dispatch_for(logger, &batch[i]);
            log_record_free_owned(&batch[i]);
        }
        log_dispatcher_flush_for(logger);
        logger->async_processing = 0;
    }
    return NULL;
}

/* Legacy worker using g_async_logger (unchanged). */
static void *async_worker(void *arg) {
    async_logger_t *al = (async_logger_t *)arg;
    log_record_t batch[ASYNC_BATCH_SIZE];
    while (1) {
        int count = mpsc_queue_get_batch(al->queue, batch, ASYNC_BATCH_SIZE);
        if (count <= 0)
            break;
        al->processing = 1;
        for (int i = 0; i < count; i++) {
            log_dispatcher_dispatch(&batch[i]);
            log_record_free_owned(&batch[i]);
        }
        log_dispatcher_flush();
        al->processing = 0;
    }
    return NULL;
}

/* ── Instance variants ── */

int log_async_init_for(logger_t *logger, int queue_size) {
    if (queue_size <= 0)
        return -1;
    if (logger->async_running)
        return 0;
    logger->queue = mpsc_queue_create((size_t)queue_size);
    if (!logger->queue)
        return -1;
    logger->async_running = 1;
    if (clog_thread_create(&logger->worker_thread, async_worker_for, logger) != 0) {
        mpsc_queue_destroy(logger->queue);
        logger->queue = NULL;
        logger->async_running = 0;
        return -1;
    }
    return 0;
}

void log_async_shutdown_for(logger_t *logger) {
    if (!logger->async_running || !logger->queue)
        return;
    mpsc_queue_close(logger->queue);
    logger->async_running = 0;
    clog_thread_join(logger->worker_thread);
    mpsc_queue_destroy(logger->queue);
    logger->queue = NULL;
}

void log_async_flush_for(logger_t *logger) {
    if (!logger->queue)
        return;
    mpsc_queue_wait_empty(logger->queue);
    while (logger->async_processing)
        clog_sleep_ms(1);
    log_dispatcher_flush_for(logger);
}

int log_async_is_running_for(logger_t *logger) {
    return logger->async_running && logger->queue != NULL;
}

int log_async_write_for(logger_t *logger, log_record_t *restrict record) {
    if (!logger->queue) {
        log_dispatcher_dispatch_for(logger, record);
        return CLOG_ERR_QUEUE_FULL;
    }
    log_record_t owned;
    if (log_record_clone(&owned, record) != 0) {
        log_dispatcher_dispatch_for(logger, record);
        return CLOG_ERR_OOM;
    }
    int ret = mpsc_queue_try_put(logger->queue, &owned);
    if (ret != 0) {
        log_record_free_owned(&owned);
        log_dispatcher_dispatch_for(logger, record);
        return CLOG_ERR_QUEUE_FULL;
    }
    return CLOG_OK;
}

size_t log_async_get_queue_depth_for(logger_t *logger) {
    if (!logger->queue)
        return 0;
    clog_mutex_lock(&logger->queue->mutex);
    size_t depth = logger->queue->count;
    clog_mutex_unlock(&logger->queue->mutex);
    return depth;
}

void log_async_atfork_child_for(logger_t *logger) {
    if (!logger->async_running || !logger->queue)
        return;
    mpsc_queue_t *q = logger->queue;
    clog_mutex_init(&q->mutex);
    clog_cond_init(&q->not_full);
    clog_cond_init(&q->not_empty);
    clog_cond_init(&q->drained);
    q->closed = 0;
    logger->async_running = 1;
    if (clog_thread_create(&logger->worker_thread, async_worker_for, logger) != 0)
        logger->async_running = 0;
}

/* ── Singleton wrappers ── */

int log_async_init(int queue_size) {
    return log_async_init_for(&g_default_logger, queue_size);
}
void log_async_shutdown(void) {
    log_async_shutdown_for(&g_default_logger);
}
void log_async_flush(void) {
    log_async_flush_for(&g_default_logger);
}
int log_async_is_running(void) {
    return log_async_is_running_for(&g_default_logger);
}
int log_async_write(log_record_t *restrict record) {
    return log_async_write_for(&g_default_logger, record);
}
size_t log_async_get_queue_depth(void) {
    return log_async_get_queue_depth_for(&g_default_logger);
}
void log_async_atfork_child(void) {
    log_async_atfork_child_for(&g_default_logger);
}
