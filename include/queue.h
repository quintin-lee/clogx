/**
 * @file queue.h
 * @brief Bounded ring buffer used by the async logger.
 *
 * @details Historically named `mpsc_queue`. The implementation uses a mutex +
 * condition-variable pair for synchronization (not a lock-free design) and
 * supports multiple producers with a single consumer.
 *
 * ## Architecture
 *
 * The queue is a circular buffer of @ref log_record_t values. Producers
 * (any thread calling `log_write`) enqueue via @ref mpsc_queue_put or
 * @ref mpsc_queue_try_put. The single consumer (the async worker thread)
 * dequeues via @ref mpsc_queue_get or @ref mpsc_queue_get_batch.
 *
 * ## Ownership & Lifecycle
 *
 * @ref mpsc_queue_put stores a shallow copy of @ref log_record_t by value.
 * If the caller passes a record with heap-owned string fields (message,
 * file, func, module, tag), those strings remain owned by the caller until
 * the consumer dequeues the record. The async worker deep-copies these
 * fields before enqueue so the caller can free them immediately after
 * returning from the log macro.
 *
 * @par Thread Safety
 * - **Multiple producers**: safe (mutex-protected).
 * - **Single consumer**: assumed; concurrent `get`/`get_batch` from
 *   multiple threads is not supported.
 * - **close** is thread-safe.
 * - **destroy** must be called only after all producers and the consumer
 *   have stopped.
 *
 * @par State Diagram
 * ```
 * Created ──► Running (put / get) ──► Closed (put fails, get drains) ──► Destroyed
 * ```
 *
 * @see log_async.h for the async worker that drives the consumer side.
 */

#ifndef QUEUE_H
#define QUEUE_H

#include "clog_port.h"
#include "log_record.h"
#include <stddef.h>

/**
 * @struct mpsc_queue_t
 * @brief Mutex-protected circular buffer of @ref log_record_t values.
 *
 * The buffer is a fixed-size ring: when @ref count equals @ref capacity,
 * blocking variants wait on @ref not_full. When @ref count is zero,
 * consumers wait on @ref not_empty. The @ref closed flag acts as a
 * termination signal — once set, producers cannot enqueue any more
 * records, and consumers drain the remaining buffer before returning -1.
 *
 * @note The queue is NOT lock-free. All operations acquire @ref mutex.
 *       This is acceptable for a logging library because the critical
 *       section is short (a record copy) and contention is low.
 */
typedef struct mpsc_queue_t {
    log_record_t *buffer;    /**< Backing storage (owned, @ref capacity × sizeof(log_record_t)). */
    size_t        capacity;  /**< Maximum number of records before blocking producers. */
    size_t        head;      /**< Next write index (producer side). */
    size_t        tail;      /**< Next read index (consumer side). */
    size_t        count;     /**< Current number of records in the buffer. */
    int           closed;    /**< Non-zero once @ref mpsc_queue_close has been called. */
    clog_mutex_t  mutex;     /**< Serializes all queue field accesses. */
    clog_cond_t   not_full;  /**< Producers wait here when @ref count == @ref capacity. */
    clog_cond_t   not_empty; /**< Consumer waits here when @ref count == 0. */
    clog_cond_t   drained;   /**< Signaled when @ref count reaches 0 after close (for @ref
                                mpsc_queue_wait_empty). */
} mpsc_queue_t;

/**
 * @brief Allocate and initialise a ring buffer queue.
 *
 * The buffer for @ref capacity records is allocated internally. The caller
 * must call @ref mpsc_queue_destroy to free it.
 *
 * @param[in] capacity  Maximum number of @ref log_record_t entries.
 *                      Must be > 0; values > 1M are impractical but not
 *                      rejected.
 * @retval non-NULL     New queue ready for use.
 * @retval NULL         Allocation failure (errno = ENOMEM).
 */
mpsc_queue_t *mpsc_queue_create(size_t capacity);

/**
 * @brief Enqueue a record, blocking if the queue is full.
 *
 * Stores a shallow (byte-wise) copy of @p record into the ring buffer.
 * If @p q is closed, return immediately with -1.
 *
 * @param[in,out] q       Queue instance.
 * @param[in]     record  Record whose contents are copied into the buffer.
 *                        Not modified by this call.
 * @retval 0   Success.
 * @retval -1  @p q is NULL, @p record is NULL, or @p q has been closed.
 *
 * @note @ref log_record_t contains **no heap-owning pointers by default**.
 *       The async path deep-copies string fields before enqueue, so the
 *       caller's stack-allocated record is safe to leave scope.
 * @note Blocks on @ref mpsc_queue_t::not_full when the buffer is full.
 * @note Spurious wakeups are handled by re-checking @ref mpsc_queue_t::count.
 */
int mpsc_queue_put(mpsc_queue_t *restrict q, log_record_t *restrict record);

/**
 * @brief Try to enqueue a record without blocking.
 *
 * Like @ref mpsc_queue_put but returns -1 immediately if the queue is full
 * instead of waiting. Useful for callers that cannot tolerate latency, such
 * as signal handlers or real-time threads.
 *
 * @param[in,out] q       Queue instance.
 * @param[in]     record  Record to copy into the buffer.
 * @retval 0   Success.
 * @retval -1  Queue is full, closed, or arguments are invalid.
 */
int mpsc_queue_try_put(mpsc_queue_t *restrict q, log_record_t *restrict record);

/**
 * @brief Dequeue a single record, blocking while empty.
 *
 * Waits on @ref mpsc_queue_t::not_empty when the buffer has no data.
 * When @p q is closed AND the buffer is empty, returns -1 to signal
 * the consumer to shut down.
 *
 * @param[in,out] q       Queue instance.
 * @param[out]    record  Receives the dequeued record (byte-wise copy).
 * @retval 0   Success.
 * @retval -1  Queue is closed and drained (consumer should exit).
 */
int mpsc_queue_get(mpsc_queue_t *restrict q, log_record_t *restrict record);

/**
 * @brief Dequeue up to @p max_records in one critical section.
 *
 * Performs a single lock acquire and copies as many records as available
 * (up to @p max_records) into the caller's array. This amortises mutex
 * overhead across multiple records and is the primary dequeue path used
 * by the async worker (batch sizes up to 64).
 *
 * Blocks if the queue is empty (same as @ref mpsc_queue_get).
 *
 * @param[in,out] q            Queue instance.
 * @param[out]    records      Destination array (must hold at least
 *                             @p max_records entries).
 * @param[in]     max_records  Capacity of @p records array.
 * @retval >0  Number of records dequeued (guaranteed ≤ @p max_records).
 * @retval 0   Queue was empty and not closed (should not happen under
 *             normal usage; consumer should retry).
 * @retval -1  Queue is closed and drained.
 */
int mpsc_queue_get_batch(mpsc_queue_t *restrict q,
                         log_record_t *restrict records,
                         size_t max_records);

/**
 * @brief Shut down the queue and wake all blocked threads.
 *
 * After this call:
 * - @ref mpsc_queue_put and @ref mpsc_queue_try_put return -1.
 * - @ref mpsc_queue_get and @ref mpsc_queue_get_batch continue to
 *   dequeue existing records until the buffer is empty, then return -1.
 *
 * All condition variables are broadcast so blocked threads can observe
 * the closed state.
 *
 * @param[in,out] q  Queue instance.
 */
void mpsc_queue_close(mpsc_queue_t *q);

/**
 * @brief Block the caller until the queue is empty and closed.
 *
 * Used during shutdown to ensure all queued records have been consumed
 * before destroying the queue. Waits on @ref mpsc_queue_t::drained,
 * which is signaled when @ref count reaches 0 after @ref mpsc_queue_close.
 *
 * @param[in,out] q  Queue instance.
 *
 * @pre @ref mpsc_queue_close must have been called beforehand (otherwise
 *      producers could keep adding records and this function never returns).
 */
void mpsc_queue_wait_empty(mpsc_queue_t *q);

/**
 * @brief Destroy a queue and release its backing buffer.
 *
 * @param[in,out] q  Queue instance (NULL-safe).
 *
 * @warning This function does NOT free the string fields (@ref log_record_t
 *          does not own heap memory for its strings in the default shallow-
 *          copy path). The async worker deep-copies strings before enqueue
 *          so remaining records after close can be discarded safely.
 */
void mpsc_queue_destroy(mpsc_queue_t *q);

#endif /* QUEUE_H */
