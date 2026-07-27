/**
 * @file queue.h
 * @brief Bounded ring buffer used by the async logger.
 *
 * @details Historically named `mpsc_queue`. The implementation is a mutex +
 * condition-variable queue (not lock-free) and supports multiple producers
 * with a single consumer.
 *
 * @par Ownership
 * @ref mpsc_queue_put stores a shallow copy of @ref log_record_t. Callers
 * that pass heap-owned string fields must free them only after a matching
 * @ref mpsc_queue_get (see async deep-copy helpers).
 */

#ifndef QUEUE_H
#define QUEUE_H

#include <stddef.h>
#include <pthread.h>
#include "log_record.h"

/**
 * @struct mpsc_queue_t
 * @brief Mutex-protected circular buffer of @ref log_record_t.
 */
typedef struct mpsc_queue_t {
    log_record_t *buffer;     /**< Backing storage. */
    size_t capacity;          /**< Maximum number of records. */
    size_t head;              /**< Next write index. */
    size_t tail;              /**< Next read index. */
    size_t count;             /**< Current number of records. */
    int closed;               /**< After close, put fails; get drains then exits. */
    pthread_mutex_t mutex;    /**< Protects all queue fields. */
    pthread_cond_t not_full;  /**< Signaled when space is available. */
    pthread_cond_t not_empty; /**< Signaled when data is available. */
    pthread_cond_t drained;   /**< Signaled when count reaches 0. */
} mpsc_queue_t;

/**
 * @brief Create a queue with the given capacity.
 * @param[in] capacity Maximum number of records.
 * @return New queue, or NULL on allocation failure.
 */
mpsc_queue_t *mpsc_queue_create(size_t capacity);

/**
 * @brief Enqueue a record (blocks while full).
 * @param[in,out] q      Queue instance.
 * @param[in]     record Record to copy into the buffer.
 * @return 0 on success, -1 if @p q is closed or arguments are invalid.
 */
int mpsc_queue_put(mpsc_queue_t *q, log_record_t *record);

/**
 * @brief Dequeue a record (blocks while empty).
 * @param[in,out] q      Queue instance.
 * @param[out]    record Receives the dequeued record.
 * @return 0 on success, -1 when the queue is closed and empty.
 */
int mpsc_queue_get(mpsc_queue_t *q, log_record_t *record);

/**
 * @brief Mark the queue closed and wake all waiters.
 *
 * Further puts fail. Gets continue until the buffer is empty, then return -1.
 *
 * @param[in,out] q Queue instance.
 */
void mpsc_queue_close(mpsc_queue_t *q);

/**
 * @brief Block until the queue is empty.
 * @param[in,out] q Queue instance.
 */
void mpsc_queue_wait_empty(mpsc_queue_t *q);

/**
 * @brief Destroy a queue and free its resources.
 * @param[in,out] q Queue instance (may be NULL).
 *
 * @warning Does not free string fields inside remaining records.
 */
void mpsc_queue_destroy(mpsc_queue_t *q);

#endif /* QUEUE_H */
