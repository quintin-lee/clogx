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

typedef struct {
    mpsc_queue_t *queue;
    clog_thread_t worker_thread;
    volatile int running;
    volatile int processing;
} async_logger_t;

static async_logger_t g_async_logger = {.queue = NULL, .running = 0, .processing = 0};

/** @brief Free string fields allocated as a single block by @ref log_record_clone. */
static void log_record_free_owned(log_record_t *record) {
    if (!record)
        return;

    const char *block =
        record->message ? record->message : (record->module ? record->module : record->tag);
    if (block) {
        free((void *)block);
    }

    record->message = NULL;
    record->file = NULL;
    record->func = NULL;
    record->module = NULL;
    record->tag = NULL;
}

/**
 * @brief Deep-copy dynamic string fields into a single block so the record outlives the producer
 * stack frame.
 * @param[out] dst Destination record (owned strings in a contiguous block on success).
 * @param[in]  src Source record (may reference stack storage).
 * @return 0 on success, -1 on allocation failure.
 */
static int log_record_clone(log_record_t *restrict dst, const log_record_t *restrict src) {
    if (!dst || !src)
        return -1;

    *dst = *src;
    /* Static string literals (__FILE__, __func__) do not need deep copy */
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

/**
 * @brief Worker loop; exits when @ref mpsc_queue_get returns -1 (closed & empty).
 * @param[in] arg Pointer to async_logger_t.
 * @return Always NULL.
 */
#define ASYNC_BATCH_SIZE 64

static void *async_worker(void *arg) {
    async_logger_t *logger = (async_logger_t *)arg;
    log_record_t batch[ASYNC_BATCH_SIZE];

    while (1) {
        int count = mpsc_queue_get_batch(logger->queue, batch, ASYNC_BATCH_SIZE);
        if (count <= 0) {
            break;
        }

        logger->processing = 1;
        for (int i = 0; i < count; i++) {
            log_dispatcher_dispatch(&batch[i]);
            log_record_free_owned(&batch[i]);
        }
        log_dispatcher_flush();
        logger->processing = 0;
    }

    return NULL;
}

int log_async_init(int queue_size) {
    if (queue_size <= 0)
        return -1;
    if (g_async_logger.running)
        return 0;

    g_async_logger.queue = mpsc_queue_create((size_t)queue_size);
    if (!g_async_logger.queue)
        return -1;

    g_async_logger.running = 1;

    if (clog_thread_create(&g_async_logger.worker_thread, async_worker, &g_async_logger) != 0) {
        mpsc_queue_destroy(g_async_logger.queue);
        g_async_logger.queue = NULL;
        g_async_logger.running = 0;
        return -1;
    }

    return 0;
}

void log_async_shutdown(void) {
    if (!g_async_logger.running || !g_async_logger.queue)
        return;

    mpsc_queue_close(g_async_logger.queue);
    g_async_logger.running = 0;
    clog_thread_join(g_async_logger.worker_thread);
    mpsc_queue_destroy(g_async_logger.queue);
    g_async_logger.queue = NULL;
}

void log_async_flush(void) {
    if (!g_async_logger.queue)
        return;

    mpsc_queue_wait_empty(g_async_logger.queue);
    while (g_async_logger.processing) {
        clog_sleep_ms(1);
    }
    log_dispatcher_flush();
}

int log_async_is_running(void) {
    return g_async_logger.running && g_async_logger.queue != NULL;
}

int log_async_write(log_record_t *restrict record) {
    if (!g_async_logger.queue) {
        log_dispatcher_dispatch(record);
        return CLOG_ERR_QUEUE_FULL;
    }

    log_record_t owned;
    if (log_record_clone(&owned, record) != 0) {
        log_dispatcher_dispatch(record);
        return CLOG_ERR_OOM;
    }

    int ret = mpsc_queue_try_put(g_async_logger.queue, &owned);
    if (ret != 0) {
        log_record_free_owned(&owned);
        log_dispatcher_dispatch(record);
        /* Fallback notification is owned by log_writevprintf. */
        return CLOG_ERR_QUEUE_FULL;
    }

    return CLOG_OK;
}

void log_async_atfork_child(void) {
    if (!g_async_logger.running || !g_async_logger.queue) {
        return;
    }

    mpsc_queue_t *q = g_async_logger.queue;
    clog_mutex_init(&q->mutex);
    clog_cond_init(&q->not_full);
    clog_cond_init(&q->not_empty);
    clog_cond_init(&q->drained);
    q->closed = 0;

    g_async_logger.running = 1;
    if (clog_thread_create(&g_async_logger.worker_thread, async_worker, &g_async_logger) != 0) {
        g_async_logger.running = 0;
    }
}

size_t log_async_get_queue_depth(void) {
    if (!g_async_logger.queue) {
        return 0;
    }
    mpsc_queue_t *q = g_async_logger.queue;
    clog_mutex_lock(&q->mutex);
    size_t depth = q->count;
    clog_mutex_unlock(&q->mutex);
    return depth;
}
