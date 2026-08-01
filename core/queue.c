/**
 * @file queue.c
 * @brief Lock-free MPSC (Multi-Producer, Single-Consumer) ring buffer for async logging.
 *
 * ## Design
 *
 * This is a lock-free bounded queue backed by a dynamically-allocated ring
 * buffer of `log_record_t` values. The producer fast path (`try_put`) uses
 * an atomic compare-exchange loop on `head` — no mutex is ever acquired by
 * producers. The single consumer drains via `get_batch` and advances `tail`.
 *
 * Synchronization is handled by:
 * - **Atomic CAS** on `head` for slot claiming (producers).
 * - **Per-slot sequence numbers**: a producer writes the record, then
 *   release-stores `seq = position + 1`; the consumer acquire-loads `seq`
 *   and only reads slots where it equals `position + 1`. This makes the
 *   record write visible to the consumer and prevents it from reading a
 *   half-written record in the window between the head CAS and the write.
 * - **Semaphore** `items_sem` signals the consumer when new items arrive.
 * - **Semaphore** `slots_sem` signals blocked producers when space frees up.
 * - A small `drain_mutex` + `drain_cond` pair is used *only* by
 *   `wait_empty` during shutdown — never in the hot path.
 *
 * ### Power-of-Two Capacity
 *
 * The capacity is rounded up to the next power of two so that the ring index
 * can be computed with a cheap bitwise AND (`pos & mask`) instead of modulo.
 *
 * ## Memory Ordering
 *
 * - Producers: `clog_atomic_load_sz(tail)` (acquire) is performed inside the
 *   CAS loop to check capacity. After claiming a slot via CAS, the record is
 *   written, then the slot is published with a release-store of
 *   `seq = position + 1`, and `sem_post(items_sem)` wakes the consumer.
 *
 * - Consumer: `sem_wait(items_sem)` (acquire) → `clog_atomic_load_sz(head)`
 *   (acquire) → for each slot, acquire-load `seq` and only read records where
 *   `seq == position + 1` → `clog_atomic_store_sz(tail, ...)` (release) →
 *   `sem_post(slots_sem)` to free slots for producers.
 *
 * @par State Diagram
 *
 * ```
 * [EMPTY] ──try_put()──► [PARTIAL] ──try_put()──► [FULL]
 *                      │               │
 *                 get_batch          get_batch
 *                      │               │
 *                      ▼               ▼
 * [EMPTY] ◄───────── [PARTIAL] ◄────────── (drained → FULL→PARTIAL)
 * ```
 */
#include "queue.h"
#include "clog_port.h"
#include <stdlib.h>
#include <string.h>

static size_t next_pow2(size_t n)
{
    if (n <= 1) {
        return 1;
    }
    size_t p = 2;
    while (p < n) {
        p <<= 1;
    }
    return p;
}

mpsc_queue_t *mpsc_queue_create(size_t capacity)
{
    mpsc_queue_t *q = malloc(sizeof(mpsc_queue_t));
    if (!q) {
        return NULL;
    }

    size_t cap = next_pow2(capacity);

    q->buffer = malloc(cap * sizeof(mpsc_slot_t));
    if (!q->buffer) {
        free(q);
        return NULL;
    }

    q->capacity = cap;
    q->mask     = cap - 1;

    q->head   = 0;
    q->tail   = 0;
    q->count  = 0;
    q->closed = 0;

    /* Initialise per-slot sequence numbers: slot i expects `seq == i + 1`
     * for the first record written at absolute position i. */
    for (size_t i = 0; i < cap; i++) {
        q->buffer[i].seq = (uint64_t)i;
    }

    if (clog_sem_init(&q->items_sem, 0) != 0) {
        free(q->buffer);
        free(q);
        return NULL;
    }
    if (clog_sem_init(&q->slots_sem, (long)cap) != 0) {
        clog_sem_destroy(&q->items_sem);
        free(q->buffer);
        free(q);
        return NULL;
    }
    clog_mutex_init(&q->drain_mutex);
    clog_cond_init(&q->drain_cond);

    return q;
}

int mpsc_queue_try_put(mpsc_queue_t *restrict q, log_record_t *restrict record)
{
    if (!q || !record) {
        return -1;
    }

    /* Fast check: if the queue has been closed, give up immediately. */
    if (clog_atomic_load_int(&q->closed)) {
        return -1;
    }

    /*
     * CAS loop: each producer atomically claims a write slot by advancing
     * `head`. If another producer races ahead, we retry with the updated
     * value. This is the lock-free fast path — no mutex is acquired.
     */
    for (;;) {
        size_t head = clog_atomic_load_sz(&q->head);
        size_t tail = clog_atomic_load_sz(&q->tail);

        if (head - tail >= q->capacity) {
            return -1; /* Queue is full. */
        }

        if (clog_atomic_cas_sz(&q->head, &head, head + 1)) {
            /*
             * Successfully claimed slot `head` (the old value). Write the
             * record, publish it with a release-store of the sequence number
             * (making the write visible to the consumer), then signal.
             */
            mpsc_slot_t *slot = &q->buffer[head & q->mask];
            slot->rec         = *record;
            clog_atomic_store_u64(&slot->seq, (uint64_t)head + 1);
            clog_atomic_fetch_add_sz(&q->count, 1);
            clog_sem_post(&q->items_sem);
            return 0;
        }
        /* CAS failed — head was advanced by another producer; retry. */
    }
}

int mpsc_queue_put(mpsc_queue_t *restrict q, log_record_t *restrict record)
{
    if (!q || !record) {
        return -1;
    }

    for (;;) {
        /* Try a non-blocking enqueue first. */
        if (mpsc_queue_try_put(q, record) == 0) {
            return 0;
        }

        /*
         * try_put failed. If the queue is closed, propagate the failure.
         * Otherwise it was full — wait for a free slot, then retry.
         */
        if (clog_atomic_load_int(&q->closed)) {
            return -1;
        }

        clog_sem_wait(&q->slots_sem);
    }
}

int mpsc_queue_get(mpsc_queue_t *restrict q, log_record_t *restrict record)
{
    return mpsc_queue_get_batch(q, record, 1) == 1 ? 0 : -1;
}

int mpsc_queue_wait_for_items(mpsc_queue_t *q)
{
    if (!q) {
        return -1;
    }

    /*
     * The semaphore may be posted by close() even when the queue is empty.
     * Loop until we either find items to consume or confirm the queue is
     * closed and drained.
     */
    for (;;) {
        clog_sem_wait(&q->items_sem);

        /* Load the latest head (producer writes) and tail (consumer reads). */
        size_t head = clog_atomic_load_sz(&q->head);
        size_t tail = clog_atomic_load_sz(&q->tail); /* single consumer,
                                                       wait_empty may read */

        if (head - tail > 0) {
            return 0;
        }
        if (clog_atomic_load_int(&q->closed)) {
            return -1;
        }
        /* Spurious wake-up — re-block. */
    }
}

int mpsc_queue_get_batch_try(mpsc_queue_t *restrict q,
                             log_record_t *restrict records,
                             size_t max_records)
{
    if (!q || !records || max_records == 0) {
        return -1;
    }

    /* Load the latest head (producer writes) and tail (consumer reads). */
    size_t head = clog_atomic_load_sz(&q->head);
    size_t tail = clog_atomic_load_sz(&q->tail); /* single consumer,
                                                   wait_empty may read */
    size_t available = head - tail;
    if (available == 0) {
        return 0;
    }

    size_t n = 0;
    while (n < available && n < max_records) {
        size_t       pos  = tail + n;
        mpsc_slot_t *slot = &q->buffer[pos & q->mask];
        /*
         * Only read the slot once its producer has published it
         * (release-stored seq == pos + 1). A producer that won the head
         * CAS but was preempted before writing has *committed* — it will
         * finish writing and post items_sem, so waiting is safe.
         */
        if (clog_atomic_load_u64(&slot->seq) != (uint64_t)pos + 1) {
            break;
        }
        records[n] = slot->rec;
        n++;
    }
    if (n == 0) {
        return 0;
    }

    /* Advance our consumer read position. */
    clog_atomic_store_sz(&q->tail, tail + n);
    clog_atomic_fetch_sub_sz(&q->count, n);

    /* Free up slots for blocked producers. */
    for (size_t i = 0; i < n; i++) {
        clog_sem_post(&q->slots_sem);
    }

    /*
     * If the queue is now empty, notify any threads waiting in
     * mpsc_queue_wait_empty(). This must happen even when the queue is
     * not closed, because mpsc_queue_wait_empty() is called by
     * log_async_flush_for() before close to ensure all records have been
     * dispatched.
     */
    size_t rem = clog_atomic_load_sz(&q->head) - clog_atomic_load_sz(&q->tail);
    if (rem == 0) {
        clog_mutex_lock(&q->drain_mutex);
        clog_cond_broadcast(&q->drain_cond);
        clog_mutex_unlock(&q->drain_mutex);
    }

    return (int)n;
}

int mpsc_queue_get_batch(mpsc_queue_t *restrict q,
                         log_record_t *restrict records,
                         size_t max_records)
{
    if (!q || !records || max_records == 0) {
        return -1;
    }

    /*
     * Blocking batch dequeue: wait for at least one published record, then
     * drain without blocking again. If all claimed slots are still
     * unpublished (producers preempted between CAS and write), the committed
     * producers are guaranteed to publish and post items_sem, so re-wait.
     */
    for (;;) {
        if (mpsc_queue_wait_for_items(q) != 0) {
            return -1;
        }
        int n = mpsc_queue_get_batch_try(q, records, max_records);
        if (n != 0) {
            return n;
        }
    }
}

void mpsc_queue_close(mpsc_queue_t *q)
{
    if (!q) {
        return;
    }

    clog_atomic_store_int(&q->closed, 1);

    /*
     * Wake at least one consumer and unblock all producers. We post to
     * items_sem once for the consumer and post capacity times to slots_sem
     * (worst case: every slot is occupied by a blocked producer).
     */
    clog_sem_post(&q->items_sem);
    for (size_t i = 0; i < q->capacity; i++) {
        clog_sem_post(&q->slots_sem);
    }

    /* Also wake any wait_empty callers. */
    clog_mutex_lock(&q->drain_mutex);
    clog_cond_broadcast(&q->drain_cond);
    clog_mutex_unlock(&q->drain_mutex);
}

void mpsc_queue_wait_empty(mpsc_queue_t *q)
{
    if (!q) {
        return;
    }

    clog_mutex_lock(&q->drain_mutex);
    for (;;) {
        size_t head = clog_atomic_load_sz(&q->head);
        size_t tail = clog_atomic_load_sz(&q->tail);
        if (head - tail == 0) {
            break;
        }
        clog_cond_wait(&q->drain_cond, &q->drain_mutex);
    }
    clog_mutex_unlock(&q->drain_mutex);
}

void mpsc_queue_destroy(mpsc_queue_t *q)
{
    if (!q) {
        return;
    }

    /*
     * Wake any threads that might still be blocked on the semaphores so
     * they can observe the closed state and exit.
     */
    clog_atomic_store_int(&q->closed, 1);
    for (size_t i = 0; i < q->capacity + 2; i++) {
        clog_sem_post(&q->items_sem);
        clog_sem_post(&q->slots_sem);
    }
    clog_mutex_lock(&q->drain_mutex);
    clog_cond_broadcast(&q->drain_cond);
    clog_mutex_unlock(&q->drain_mutex);

    clog_sem_destroy(&q->items_sem);
    clog_sem_destroy(&q->slots_sem);
    clog_mutex_destroy(&q->drain_mutex);
    clog_cond_destroy(&q->drain_cond);

    free(q->buffer);
    free(q);
}
