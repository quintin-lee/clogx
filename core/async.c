/**
 * @file async.c
 * @brief Async logging worker thread and record deep-copy logic.
 *
 * ## Design
 *
 * When `log_config_t.async_mode` is enabled, the write path does not
 * dispatch to sinks directly. Instead, it deep-copies the `log_record_t`
 * into the async queue and returns immediately. A dedicated background
 * thread (`async_worker_loop`) drains the queue and dispatches records
 * synchronously.
 *
 * ## Deep-Copy (`log_record_clone`)
 *
 * A `log_record_t` contains several heap-allocated strings (message,
 * file, function, formatted_message, plugin_json, extra_json). A naive
 * `memcpy` would create dangling pointers. `log_record_clone` allocates
 * a fresh `log_record_t`, duplicates every heap string via `strdup`, and
 * returns the independent copy.
 *
 * ## Worker Thread Lifecycle
 *
 * ```
 * log_start_async_worker()
 *   ├─ allocate queue (capacity from config)
 *   ├─ create worker thread (async_worker_loop)
 *   └─ thread enters drain loop
 *
 * log_stop_async_worker()
 *   ├─ set shutdown flag
 *   ├─ wake worker via condition variable
 *   ├─ pthread_join (wait for drain)
 *   └─ free queue
 * ```
 *
 * The worker loop blocks on a condition variable when the queue is empty.
 * When records are enqueued, the producer signals the condition variable
 * to wake the worker.
 */
#include "clog_port.h"
#include "dispatcher.h"
#include "log.h"
#include "log_async.h"
#include "log_config.h"
#include "log_internal.h"
#include "queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void log_record_free_owned(log_record_t *record)
{
    if (!record) {
        return;
    }
    const char *block =
        record->message ? record->message : (record->module ? record->module : record->tag);
    if (block) {
        free((void *)block);
    }
    record->message = NULL;
    record->file    = NULL;
    record->func    = NULL;
    record->module  = NULL;
    record->tag     = NULL;
}

/**
 * @brief Deep-copy a log record, allocating independent heap strings.
 *
 * A naive memcpy would create dangling pointers when the original
 * record's stack-allocated strings go out of scope. This function
 * allocates a single contiguous block for message + module + tag,
 * copies them, and points the destination fields into that block.
 * file and func are compile-time constants (no copy needed).
 *
 * @param[out] dst  Destination record (must not be NULL).
 * @param[in]  src  Source record to clone (must not be NULL).
 * @return 0 on success, -1 on NULL inputs or malloc failure.
 */
static int log_record_clone(log_record_t *restrict dst, const log_record_t *restrict src)
{
    if (!dst || !src) {
        return -1;
    }
    *dst      = *src;
    dst->file = src->file;
    dst->func = src->func;

    size_t msg_len     = src->message ? strlen(src->message) : 0;
    size_t mod_len     = src->module ? strlen(src->module) : 0;
    size_t tag_len     = src->tag ? strlen(src->tag) : 0;
    size_t total_bytes = (src->message ? msg_len + 1 : 0) + (src->module ? mod_len + 1 : 0) +
                         (src->tag ? tag_len + 1 : 0);

    if (total_bytes == 0) {
        dst->message = NULL;
        dst->module  = NULL;
        dst->tag     = NULL;
        return 0;
    }

    /* LCOV_EXCL_START - System malloc failure */
    char *block = malloc(total_bytes);
    if (!block) {
        return -1;
    }
    /* LCOV_EXCL_STOP */

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

/**
 * @brief Background worker thread: dequeue batches and dispatch to sinks.
 *
 * Runs in a loop calling mpsc_queue_get_batch() (up to 64 records
 * per batch). For each batch, dispatches every record via
 * log_dispatcher_dispatch_for(), frees owned strings, then flushes
 * the dispatcher once per batch (not per record) for throughput.
 * Exits when the queue is closed and drained.
 *
 * @param arg  Pointer to the logger_t instance.
 * @return NULL always.
 */
static void *async_worker_for(void *arg)
{
    logger_t    *logger = (logger_t *)arg;
    log_record_t batch[ASYNC_BATCH_SIZE];
    while (1) {
        int count = mpsc_queue_get_batch(logger->queue, batch, ASYNC_BATCH_SIZE);
        if (count <= 0) {
            break;
        }
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

/* ── Instance variants ── */

/**
 * @brief Start the async worker thread and create the queue.
 *
 * Creates a bounded queue of @p queue_size capacity, then spawns a
 * detached POSIX thread (or Windows thread) running async_worker_for().
 *
 * @param logger      Logger instance.
 * @param queue_size  Maximum number of records in the async queue.
 * @return 0 on success, -1 on queue/thread creation failure.
 */
int log_async_init_for(logger_t *logger, int queue_size)
{
    if (!logger || queue_size <= 0) {
        return -1;
    }
    if (logger->async_running) {
        return 0;
    }
    logger->queue = mpsc_queue_create((size_t)queue_size);
    if (!logger->queue) {
        return -1;
    }
    logger->async_running = 1;
    if (clog_thread_create(&logger->worker_thread, async_worker_for, logger) != 0) {
        mpsc_queue_destroy(logger->queue);
        logger->queue         = NULL;
        logger->async_running = 0;
        return -1;
    }
    return 0;
}

void log_async_shutdown_for(logger_t *logger)
{
    if (!logger || !logger->async_running || !logger->queue) {
        return;
    }
    mpsc_queue_close(logger->queue);
    logger->async_running = 0;
    clog_thread_join(logger->worker_thread);
    mpsc_queue_destroy(logger->queue);
    logger->queue = NULL;
}

void log_async_flush_for(logger_t *logger)
{
    if (!logger || !logger->queue) {
        return;
    }
    mpsc_queue_wait_empty(logger->queue);
    while (logger->async_processing) {
        clog_sleep_ms(1);
    }
    log_dispatcher_flush_for(logger);
}

int log_async_is_running_for(logger_t *logger)
{
    return logger && logger->async_running && logger->queue != NULL;
}

int log_async_write_for(logger_t *logger, log_record_t *restrict record)
{
    if (!logger || !record) {
        return -1;
    }
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

size_t log_async_get_queue_depth_for(logger_t *logger)
{
    if (!logger || !logger->queue) {
        return 0;
    }
    return clog_atomic_load_sz(&logger->queue->count);
}

void log_async_atfork_child_for(logger_t *logger)
{
    if (!logger || !logger->async_running || !logger->queue) {
        return;
    }
    mpsc_queue_t *q = logger->queue;
    /* The parent's thread does not survive fork; re-init sync primitives. */
    clog_sem_destroy(&q->items_sem);
    clog_sem_destroy(&q->slots_sem);
    clog_mutex_destroy(&q->drain_mutex);
    clog_cond_destroy(&q->drain_cond);

    q->head   = 0;
    q->tail   = 0;
    q->count  = 0;
    q->closed = 0;

    if (clog_sem_init(&q->items_sem, 0) != 0) {
        logger->async_running = 0;
        return;
    }
    if (clog_sem_init(&q->slots_sem, (long)q->capacity) != 0) {
        clog_sem_destroy(&q->items_sem);
        logger->async_running = 0;
        return;
    }
    clog_mutex_init(&q->drain_mutex);
    clog_cond_init(&q->drain_cond);

    logger->async_running = 1;
    if (clog_thread_create(&logger->worker_thread, async_worker_for, logger) != 0) {
        logger->async_running = 0;
    }
}

/* ── Singleton wrappers ── */

int log_async_init(int queue_size)
{
    return log_async_init_for(&g_default_logger, queue_size);
}
void log_async_shutdown(void)
{
    log_async_shutdown_for(&g_default_logger);
}
void log_async_flush(void)
{
    log_async_flush_for(&g_default_logger);
}
int log_async_is_running(void)
{
    return log_async_is_running_for(&g_default_logger);
}
int log_async_write(log_record_t *restrict record)
{
    return log_async_write_for(&g_default_logger, record);
}
size_t log_async_get_queue_depth(void)
{
    return log_async_get_queue_depth_for(&g_default_logger);
}
void log_async_atfork_child(void)
{
    log_async_atfork_child_for(&g_default_logger);
}
