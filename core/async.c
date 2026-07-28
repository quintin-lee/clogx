/**
 * @file async.c
 * @brief Async logger: deep-copy records into a queue consumed by one worker.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "log_async.h"
#include "queue.h"
#include "dispatcher.h"
#include "log_config.h"
#include "log.h"

typedef struct {
    mpsc_queue_t *queue;
    pthread_t worker_thread;
    volatile int running;
} async_logger_t;

static async_logger_t g_async_logger = {.queue = NULL, .worker_thread = 0, .running = 0};

static char *dup_field(const char *s) {
    if (!s)
        return NULL;
    return strdup(s);
}

/** @brief Free string fields previously allocated by @ref log_record_clone. */
static void log_record_free_owned(log_record_t *record) {
    if (!record)
        return;

    free((void *)record->message);
    free((void *)record->file);
    free((void *)record->func);
    free((void *)record->module);
    free((void *)record->tag);

    record->message = NULL;
    record->file = NULL;
    record->func = NULL;
    record->module = NULL;
    record->tag = NULL;
}

/**
 * @brief Deep-copy string fields so the record outlives the producer stack frame.
 * @param[out] dst Destination record (owned strings on success).
 * @param[in]  src Source record (may reference stack storage).
 * @return 0 on success, -1 on allocation failure (dst fields freed).
 */
static int log_record_clone(log_record_t *restrict dst, const log_record_t *restrict src) {
    if (!dst || !src)
        return -1;

    *dst = *src;
    dst->message = dup_field(src->message);
    dst->file = dup_field(src->file);
    dst->func = dup_field(src->func);
    dst->module = dup_field(src->module);
    dst->tag = dup_field(src->tag);

    if ((src->message && !dst->message) || (src->file && !dst->file) || (src->func && !dst->func) ||
        (src->module && !dst->module) || (src->tag && !dst->tag)) {
        log_record_free_owned(dst);
        return -1;
    }

    return 0;
}

/**
 * @brief Worker loop; exits when @ref mpsc_queue_get returns -1 (closed & empty).
 * @param[in] arg Pointer to async_logger_t.
 * @return Always NULL.
 */
static void *async_worker(void *arg) {
    async_logger_t *logger = (async_logger_t *)arg;
    log_record_t record;

    while (1) {
        int ret = mpsc_queue_get(logger->queue, &record);
        if (ret != 0) {
            break;
        }

        log_dispatcher_dispatch(&record);
        log_record_free_owned(&record);
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

    if (pthread_create(&g_async_logger.worker_thread, NULL, async_worker, &g_async_logger) != 0) {
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
    pthread_join(g_async_logger.worker_thread, NULL);
    mpsc_queue_destroy(g_async_logger.queue);
    g_async_logger.queue = NULL;
}

void log_async_flush(void) {
    if (!g_async_logger.queue)
        return;

    mpsc_queue_wait_empty(g_async_logger.queue);
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

    size_t cap = g_async_logger.queue->capacity;
    mpsc_queue_destroy(g_async_logger.queue);
    g_async_logger.queue = mpsc_queue_create(cap);
    if (!g_async_logger.queue) {
        g_async_logger.running = 0;
        return;
    }

    g_async_logger.running = 1;
    if (pthread_create(&g_async_logger.worker_thread, NULL, async_worker, &g_async_logger) != 0) {
        mpsc_queue_destroy(g_async_logger.queue);
        g_async_logger.queue = NULL;
        g_async_logger.running = 0;
    }
}
