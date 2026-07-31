/**
 * @file queue.c
 * @brief Bounded ring buffer for async logging.
 *
 * ## Design
 *
 * This is a mutex-protected bounded queue backed by a dynamically-allocated
 * ring buffer (`log_record_t`). The implementation is intentionally simple:
 * lock → push/pop → unlock. This avoids lock-free complexity while still
 * delivering high throughput via the mutex hot path.
 *
 * ## State Diagram
 *
 * ```
 * [EMPTY] ──push()──► [PARTIAL] ──push()──► [FULL]
 *                        │                     │
 *                     pop()                 pop()
 *                        │                     │
 *                        ▼                     ▼
 * [EMPTY] ◄────────── [PARTIAL] ◄────────── [EMPTY]
 * ```
 *
 * - `head` is the index of the next slot to write to (producer).
 * - `tail` is the index of the next slot to read from (consumer).
 * - `head == tail` → empty; `next(head) == tail` → full.
 *
 * ## Thread Safety
 *
 * All operations are serialised by `queue->mutex`. In async logging mode,
 * only the producer thread (main thread calling `LOG_*`) pushes, while
 * the consumer thread (worker thread) pops.
 */
#include "queue.h"
#include "clog_port.h"
#include <stdlib.h>
#include <string.h>

/**
 * @brief Create a bounded ring buffer for async log records.
 *
 * Allocates the queue struct, the ring buffer, and initialises three
 * condition variables: not_full (producer waits), not_empty (consumer
 * waits), drained (flush waits).
 *
 * @param capacity  Maximum number of records the queue can hold.
 * @return Pointer to the new queue, or NULL on allocation failure.
 */
mpsc_queue_t *mpsc_queue_create(size_t capacity)
{
    mpsc_queue_t *q = malloc(sizeof(mpsc_queue_t));
    if (!q) {
        return NULL;
    }

    q->capacity = capacity;
    q->buffer   = malloc(capacity * sizeof(log_record_t));
    if (!q->buffer) {
        free(q);
        return NULL;
    }

    q->head   = 0;
    q->tail   = 0;
    q->count  = 0;
    q->closed = 0;

    clog_mutex_init(&q->mutex);
    clog_cond_init(&q->not_full);
    clog_cond_init(&q->not_empty);
    clog_cond_init(&q->drained);

    return q;
}

/**
 * @brief Blocking enqueue: wait for space if the queue is full.
 *
 * Blocks the calling thread on the not_full condition variable until
 * a slot is available or the queue is closed. This is the primary
 * enqueue path for the LOG_* macros in async mode.
 *
 * @param q        Queue instance (must not be NULL).
 * @param record   Log record to enqueue (deep-copied into the ring).
 * @return 0 on success, -1 if queue is NULL, closed, or record is NULL.
 */
int mpsc_queue_put(mpsc_queue_t *restrict q, log_record_t *restrict record)
{
    if (!q || !record) {
        return -1;
    }

    int ret = -1;
    CLOG_MUTEXGUARDED(&q->mutex, {
        while (q->count == q->capacity && !q->closed) {
            clog_cond_wait(&q->not_full, &q->mutex);
        }

        if (!q->closed) {
            q->buffer[q->head] = *record;
            q->head            = (q->head + 1) % q->capacity;
            q->count++;
            clog_cond_signal(&q->not_empty);
            ret = 0;
        }
    });
    return ret;
}

/**
 * @brief Non-blocking enqueue: return immediately if the queue is full.
 *
 * Unlike mpsc_queue_put(), never blocks. Returns -1 if no space is
 * available, which triggers the async fallback path (drop or sync
 * degradation depending on configuration).
 *
 * @param q        Queue instance (must not be NULL).
 * @param record   Log record to enqueue.
 * @return 0 on success, -1 if full, closed, or NULL inputs.
 */
int mpsc_queue_try_put(mpsc_queue_t *restrict q, log_record_t *restrict record)
{
    if (!q || !record) {
        return -1;
    }

    int ret = -1;
    CLOG_MUTEXGUARDED(&q->mutex, {
        if (!q->closed && q->count < q->capacity) {
            q->buffer[q->head] = *record;
            q->head            = (q->head + 1) % q->capacity;
            q->count++;
            clog_cond_signal(&q->not_empty);
            ret = 0;
        }
    });
    return ret;
}

int mpsc_queue_get(mpsc_queue_t *restrict q, log_record_t *restrict record)
{
    return mpsc_queue_get_batch(q, record, 1) == 1 ? 0 : -1;
}

/**
 * @brief Batch dequeue: drain up to @p max_records in a single lock acquisition.
 *
 * The async worker calls this to dequeue multiple records per iteration,
 * reducing mutex contention. Blocks on the not_empty condition when the
 * queue is empty, unless the queue is closed (returns remaining records).
 *
 * @param q            Queue instance (must not be NULL).
 * @param records      Output array for dequeued records.
 * @param max_records  Maximum records to dequeue in this call.
 * @return Number of records actually dequeued (0 if closed and empty),
 *         or -1 on NULL inputs.
 */
int mpsc_queue_get_batch(mpsc_queue_t *restrict q,
                         log_record_t *restrict records,
                         size_t max_records)
{
    if (!q || !records || max_records == 0) {
        return -1;
    }

    int count_to_get = -1;
    CLOG_MUTEXGUARDED(&q->mutex, {
        while (q->count == 0) {
            if (q->closed) {
                break;
            }
            clog_cond_wait(&q->not_empty, &q->mutex);
        }

        if (!q->closed || q->count > 0) {
            count_to_get = (int)(q->count < max_records ? q->count : max_records);
            for (int i = 0; i < count_to_get; i++) {
                records[i] = q->buffer[q->tail];
                q->tail    = (q->tail + 1) % q->capacity;
                q->count--;
            }

            if (q->count == 0) {
                clog_cond_broadcast(&q->drained);
            }

            clog_cond_broadcast(&q->not_full);
        }
    });
    return count_to_get;
}

/**
 * @brief Close the queue, waking all blocked producers and consumers.
 *
 * After closing, mpsc_queue_put() returns -1 immediately and
 * mpsc_queue_get_batch() drains remaining records then returns 0.
 * Used during logger shutdown to signal the async worker to exit.
 */
void mpsc_queue_close(mpsc_queue_t *q)
{
    if (!q) {
        return;
    }

    CLOG_MUTEXGUARDED(&q->mutex, {
        q->closed = 1;
        clog_cond_broadcast(&q->not_empty);
        clog_cond_broadcast(&q->not_full);
        clog_cond_broadcast(&q->drained);
    });
}

/**
 * @brief Block until the queue is empty (all records drained by consumer).
 *
 * Used by log_flush() to wait for the async worker to finish processing
 * all queued records. Blocks on the drained condition variable.
 */
void mpsc_queue_wait_empty(mpsc_queue_t *q)
{
    if (!q) {
        return;
    }

    CLOG_MUTEXGUARDED(&q->mutex, {
        while (q->count > 0) {
            clog_cond_wait(&q->drained, &q->mutex);
        }
    });
}

/**
 * @brief Free all resources associated with the queue.
 *
 * Broadcasts all condition variables to wake any blocked threads,
 * destroys mutex and conditions, then frees the ring buffer and
 * queue struct. Caller must ensure no other thread is accessing
 * the queue (typically after mpsc_queue_close() + thread join).
 */
void mpsc_queue_destroy(mpsc_queue_t *q)
{
    if (!q) {
        return;
    }

    CLOG_MUTEXGUARDED(&q->mutex, {
        clog_cond_broadcast(&q->not_full);
        clog_cond_broadcast(&q->not_empty);
        clog_cond_broadcast(&q->drained);
    });
    clog_mutex_destroy(&q->mutex);
    clog_cond_destroy(&q->not_full);
    clog_cond_destroy(&q->not_empty);
    clog_cond_destroy(&q->drained);

    free(q->buffer);
    free(q);
}
