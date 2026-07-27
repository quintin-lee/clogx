#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "log_async.h"
#include "queue.h"
#include "dispatcher.h"
#include "log_config.h"

typedef struct {
    mpsc_queue_t *queue;
    pthread_t worker_thread;
    volatile int running;
} async_logger_t;

static async_logger_t g_async_logger = {
    .queue = NULL,
    .worker_thread = 0,
    .running = 0
};

void* async_worker(void *arg) {
    async_logger_t *logger = (async_logger_t *)arg;
    log_record_t record;

    while (logger->running) {
        int ret = mpsc_queue_get(logger->queue, &record);
        if (ret == 0 && logger->running) {
            log_dispatcher_dispatch(&record);
        }
    }

    return NULL;
}

int log_async_init(int queue_size) {
    if (queue_size <= 0) return -1;

    g_async_logger.queue = mpsc_queue_create(queue_size);
    if (!g_async_logger.queue) return -1;

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
    if (!g_async_logger.running) return;

    g_async_logger.running = 0;
    pthread_cond_broadcast(&g_async_logger.queue->not_full);
    pthread_cond_broadcast(&g_async_logger.queue->not_empty);
    pthread_join(g_async_logger.worker_thread, NULL);
    mpsc_queue_destroy(g_async_logger.queue);
    g_async_logger.queue = NULL;
}

int log_async_write(log_record_t *record) {
    if (!g_async_logger.queue) {
        // Async not initialized, fall back to direct dispatch
        log_dispatcher_dispatch(record);
        return 0;
    }

    return mpsc_queue_put(g_async_logger.queue, record);
}