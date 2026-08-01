/**
 * @file queue.h
 * @brief Lock-free MPSC (Multi-Producer, Single-Consumer) ring buffer used by
 * the async logger.
 *
 * @details The queue uses C11-style atomic operations (via compiler builtins)
 * for the producer fast path — multiple threads may call @ref mpsc_queue_try_put
 * concurrently without any mutex contention. A single dedicated consumer thread
 * blocks on @ref mpsc_queue_wait_for_items and drains via
 * @ref mpsc_queue_get_batch_try (or the combined @ref mpsc_queue_get_batch).
 *
 * ## Architecture
 *
 * The queue is a circular buffer of @ref log_record_t values. Producers
 * (any thread calling `log_write`) enqueue via @ref mpsc_queue_put or
 * @ref mpsc_queue_try_put. The single consumer (the async worker thread)
 * dequeues via @ref mpsc_queue_get or @ref mpsc_queue_get_batch.
 *
 * ### Lock-free Mechanism
 *
 * - **head** (atomic, write-only from producer side): Each producer atomically
 *   claims a slot via compare-exchange loop on `head`. The CAS ensures only one
 *   producer wins each slot.
 * - **tail** (atomic, write-only from consumer side): Only the consumer thread
 *   advances `tail`. Producers read it (atomically) to check capacity.
 * - **Per-slot sequence numbers** (`mpsc_slot_t::seq`): `head` is advanced by
 *   the CAS *before* the record is written, so a consumer must not trust
 *   `head` alone — it would read half-written records. After writing the
 *   record, the producer publishes it with a release-store of
 *   `seq = position + 1`; the consumer only reads a slot whose `seq` it
 *   observes (acquire-load) to equal `position + 1`. Slots are initialised to
 *   `seq = i` at creation, so the invariant holds across ring wraparound.
 * - **Semaphores**: `items_sem` counts available items (consumer waits on it);
 *   `slots_sem` counts free slots (blocking `put` waits on it). These replace
 *   the mutex+condvar pair for thread notification.
 * - **Drain mutex/condvar**: A lightweight mutex is kept solely for
 *   @ref mpsc_queue_wait_empty — used only during shutdown, not in the hot path.
 *
 * ## Ownership & Lifecycle
 *
 * @ref mpsc_queue_try_put stores a shallow copy of @ref log_record_t by value.
 * If the caller passes a record with heap-owned string fields (message, file,
 * func, module, tag), those strings remain owned by the caller until the
 * consumer dequeues the record. The async worker deep-copies these
 * fields before enqueue so the caller can free them immediately after
 * returning from the log macro.
 *
 * @par Thread Safety
 * - **Multiple producers**: safe — lock-free via atomic CAS on `head`.
 * - **Single consumer**: the single consumer thread calls `get`/`get_batch`.
 *   No other thread may call these dequeue functions concurrently.
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
 * @struct mpsc_slot_t
 * @brief A single ring-buffer slot: the record plus its publish sequence.
 *
 * `seq` starts at the slot index (0..capacity-1) and is set to
 * `absolute position + 1` by the producer with a release-store after the
 * record is fully written. The consumer acquire-loads `seq` and only reads
 * the record when it equals `position + 1`, which guarantees the record
 * write is visible. This closes the window between the head CAS (which
 * advances `head` before the record is written) and the actual write.
 */
typedef struct {
    log_record_t      rec; /**< The record (written before seq is published). */
    volatile uint64_t seq; /**< Publish counter — release-stored by producer,
                                acquire-loaded by consumer. */
} mpsc_slot_t;

/**
 * @struct mpsc_queue_t
 * @brief Lock-free MPSC circular buffer of @ref log_record_t values.
 *
 * The buffer is a fixed-size ring of @ref mpsc_slot_t. Producers claim slots
 * atomically via compare-exchange on @ref head, write the record, then
 * publish the slot with a release-store of its sequence number. The single
 * consumer only dequeues published slots. A semaphore (`items_sem`) wakes
 * the consumer; another (`slots_sem`) wakes blocked producers. A small mutex
 * protects only the drain-wait path.
 *
 * @note The producer fast path (try_put) is **lock-free** — no mutex is
 *       acquired. Contention is on the CAS instruction, which is far cheaper
 *       than a syscall-backed mutex.
 */
typedef struct mpsc_queue_t {
    mpsc_slot_t    *buffer;      /**< Backing storage (capacity × sizeof(mpsc_slot_t)). */
    size_t          capacity;    /**< Maximum records (always rounded up to power-of-2). */
    size_t          mask;        /**< capacity - 1 (for fast modulo: pos & mask). */
    volatile size_t head;        /**< Next write index (producer-side, atomic via CAS). */
    volatile size_t tail;        /**< Next read index (consumer-side, atomic load/store). */
    volatile size_t count;       /**< Approximate current count (for stats/depth queries). */
    volatile int    closed;      /**< Non-zero once @ref mpsc_queue_close has been called. */
    clog_sem_t      items_sem;   /**< Semaphore: count of items available for the consumer. */
    clog_sem_t      slots_sem;   /**< Semaphore: count of free slots (for blocking put). */
    clog_mutex_t    drain_mutex; /**< Mutex for wait_empty condvar (shutdown path only). */
    clog_cond_t     drain_cond;  /**< Signaled when count reaches 0 after close (for wait_empty). */
} mpsc_queue_t;

/**
 * @brief Allocate and initialise a lock-free ring buffer queue.
 *
 * The buffer for `capacity` records is allocated internally. `capacity` is
 * rounded up to the next power of two (minimum 2) to enable fast bitwise
 * modulo. The caller must call @ref mpsc_queue_destroy to free it.
 *
 * @param[in] capacity  Desired maximum number of @ref log_record_t entries.
 *                      Values of 0 or 1 are clamped to 2.
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
 * @note Blocks on @ref mpsc_queue_t::slots_sem when the buffer is full.
 */
int mpsc_queue_put(mpsc_queue_t *restrict q, log_record_t *restrict record);

/**
 * @brief Try to enqueue a record without blocking.
 *
 * Unlike @ref mpsc_queue_put but returns -1 immediately if the queue is full
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
 * Waits on @ref mpsc_queue_t::items_sem when the buffer has no data.
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
 * @brief Block until the queue has at least one published record.
 *
 * Returns as soon as records are available to dequeue. Spurious wake-ups
 * (e.g. @ref mpsc_queue_close posting items_sem on an empty queue) are
 * retried internally.
 *
 * @param[in,out] q  Queue instance.
 * @retval 0   Records are available to dequeue.
 * @retval -1  Queue is closed and drained.
 */
int mpsc_queue_wait_for_items(mpsc_queue_t *q);

/**
 * @brief Non-blocking batch dequeue.
 *
 * Same dequeue semantics as @ref mpsc_queue_get_batch, but never blocks:
 * returns 0 immediately when no published records are available. The async
 * worker uses this after @ref mpsc_queue_wait_for_items so it can set its
 * in-flight flag before draining (closing the flush visibility window).
 *
 * @param[in,out] q            Queue instance.
 * @param[out]    records      Destination array (must hold at least
 *                             @p max_records entries).
 * @param[in]     max_records  Capacity of @p records array.
 * @retval >0  Number of records dequeued (guaranteed ≤ @p max_records).
 * @retval 0   No published records available.
 * @retval -1  Invalid arguments.
 */
int mpsc_queue_get_batch_try(mpsc_queue_t *restrict q,
                             log_record_t *restrict records,
                             size_t max_records);

/**
 * @brief Dequeue up to @p max_records in one batch.
 *
 * Performs a single dequeue of as many records as available
 * (up to @p max_records) into the caller's array. This amortises
 * semaphore overhead across multiple records and is the primary dequeue
 * path used by the async worker (batch sizes up to 64).
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
 * All waiters (on items_sem, slots_sem, and drain_cond) are woken.
 *
 * @param[in,out] q  Queue instance.
 */
void mpsc_queue_close(mpsc_queue_t *q);

/**
 * @brief Block the caller until the queue is empty and closed.
 *
 * Used during shutdown to ensure all queued records have been consumed
 * before destroying the queue. Waits on @ref mpsc_queue_t::drain_cond,
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
 * Wakes all blocked threads, destroys semaphores and mutex/condvar,
 * then frees the ring buffer and queue struct.
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