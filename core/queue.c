/**
 * @file queue.c
 * @brief Lock-free MPSC (Multi-Producer, Single-Consumer) ring buffer for async logging.
 *
 * ## Design
 *
 * This is a lock-free bounded queue backed by a dynamically-allocated ring
 * buffer of `log_record_t` values. The producer fast path (`try_put`) uses
 * a single `atomic_fetch_add` on `head` (wait-free claim, no retry) — no
 * mutex is ever acquired by producers. The single consumer drains via
 * `get_batch` and advances `tail`.
 *
 * Synchronization is handled by:
 * - **Atomic fetch_add** on `head` for slot claiming (producers); a
 *   post-claim full-queue check publishes a tombstone for race losers.
 * - **Per-slot sequence numbers**: a producer writes the record, then
 *   release-stores `seq = position + 1`; the consumer acquire-loads `seq`
 *   and only reads slots where it equals `position + 1`. This makes the
 *   record write visible to the consumer and prevents it from reading a
 *   half-written record in the window between the head fetch_add and
 *   the write.
 * - **Semaphore** `items_sem` wakes the consumer, posted only when the
 *   consumer is parked (coalesced; check-then-park with double-check).
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
 * - Producers: `clog_atomic_load_sz(tail)` (acquire) is the optimistic
 *   pre-check; the slot is claimed via fetch_add (acq-rel), re-checked
 *   against tail, then the record is written and the slot published with
 *   a release-store of `seq = position + 1`. `sem_post(items_sem)` fires
 *   only on a parked 1->0 claim.
 *
 * - Consumer: fast-path `clog_atomic_load_sz(head/tail)` (acquire) → if
 *   empty and open, set parked, double-check, then `sem_wait(items_sem)`
 *   → for each slot, acquire-load `seq` and only read records where
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

/* A record is inline iff its message points into its own inline_buf.
 * log_record_clone packs message+module+tag+KV strings all-or-nothing:
 * total <= CLOG_MAX_INLINE_SIZE goes fully inline, otherwise fully heap.
 * Empty records (message == NULL) and tombstones are never inline. */
static int log_record_is_inline(const log_record_t *r)
{
    return r->message != NULL && r->message >= r->inline_buf &&
           r->message < r->inline_buf + CLOG_MAX_INLINE_SIZE;
}

/* Repoint dst's string pointers into dst's own inline_buf at the same
 * offsets src uses. Pure pointer arithmetic, no memcpy (struct copy
 * already carried the bytes). Covers message/module/tag + KV keys/STR vals. */
static void log_record_rebase_inline(log_record_t *dst, const log_record_t *src)
{
    ptrdiff_t off;
    if (src->message) {
        off          = src->message - src->inline_buf;
        dst->message = dst->inline_buf + off;
    }
    if (src->module) {
        off         = src->module - src->inline_buf;
        dst->module = dst->inline_buf + off;
    }
    if (src->tag) {
        off      = src->tag - src->inline_buf;
        dst->tag = dst->inline_buf + off;
    }
    for (size_t i = 0; i < src->kv_count; i++) {
        if (src->kv[i].key) {
            off            = src->kv[i].key - src->inline_buf;
            dst->kv[i].key = dst->inline_buf + off;
        }
        if (src->kv[i].type == CLOG_KV_TYPE_STR && src->kv[i].val.str) {
            off                = src->kv[i].val.str - src->inline_buf;
            dst->kv[i].val.str = dst->inline_buf + off;
        }
    }
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

    q->head            = 0;
    q->tail            = 0;
    q->closed          = 0;
    q->consumer_parked = 0;

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

/* Wake the consumer only if it is parked (CAS 1->0 claims the post).
 * When the consumer is awake and batch-draining, producers pay zero syscall. */
static void queue_signal_consumer(mpsc_queue_t *q)
{
    int expected = 1;
    if (clog_atomic_cas_int(&q->consumer_parked, &expected, 0)) {
        clog_sem_post(&q->items_sem);
    }
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

    /* Optimistic pre-check (same drop semantics as before). */
    size_t tail_snap = clog_atomic_load_sz(&q->tail);
    size_t head_snap = clog_atomic_load_sz(&q->head);
    if (head_snap - tail_snap >= q->capacity) {
        return -1; /* Queue is full. */
    }

    /*
     * Wait-free claim: one atomic op, no CAS retry storm under N producers.
     * head is monotonic — never rolled back (would race concurrent claims).
     */
    size_t       pos  = clog_atomic_fetch_add_sz_ar(&q->head, 1);
    mpsc_slot_t *slot = &q->buffer[pos & q->mask];

    /* Post-claim re-check: losers of the full-queue race must publish a
     * tombstone (valid seq + is_tombstone, no owned memory) — silent
     * abandon would stall the consumer, which breaks at the first
     * unpublished slot. But the ring slot currently holds the UNREAD
     * record from lap pos-capacity (tail <= pos-capacity proves it):
     * publishing now would destroy it. Wait until the consumer has eaten
     * past it (progress is guaranteed: everything before pos is published,
     * so the consumer advances to the hole), then publish. If the consumer
     * makes zero progress for the whole bound it is effectively dead —
     * abandon the claim (the record is already fallback-dispatched
     * write-side; the queue degrades to sync mode instead of hanging). */
    tail_snap = clog_atomic_load_sz(&q->tail);
    if (pos - tail_snap >= q->capacity) {
        if (pos >= q->capacity) {
            size_t        last_tail = tail_snap;
            unsigned long stalls    = 0;
            for (;;) {
                tail_snap = clog_atomic_load_sz(&q->tail);
                if (tail_snap > pos - q->capacity) {
                    break; /* Old content consumed — overwrite is safe. */
                }
                if (tail_snap != last_tail) {
                    last_tail = tail_snap;
                    stalls    = 0;
                } else if (++stalls > 1000000) {
                    return -1; /* Consumer dead/stalled — abandon, no publish. */
                }
                clog_sleep_ms(0);
            }
        }
        log_record_t tomb;
        memset(&tomb, 0, sizeof(tomb));
        tomb.is_tombstone = true;
        slot->rec         = tomb;
        clog_atomic_store_u64(&slot->seq, (uint64_t)pos + 1);
        queue_signal_consumer(q);
        return -1;
    }

    slot->rec = *record;
    if (log_record_is_inline(record)) {
        /* Pointers still reference the producer's stack copy — rebase into the slot. */
        log_record_rebase_inline(&slot->rec, record);
    }
    clog_atomic_store_u64(&slot->seq, (uint64_t)pos + 1);
    queue_signal_consumer(q);
    return 0;
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
    for (;;) {
        /* Fast path: items already visible — never touch parked/sem. */
        size_t head = clog_atomic_load_sz(&q->head);
        size_t tail = clog_atomic_load_sz(&q->tail);
        if (head - tail > 0) {
            return 0;
        }
        if (clog_atomic_load_int(&q->closed)) {
            return -1;
        }
        /* Park, then re-check (check-then-park): a producer that posted
         * between our first check and park would otherwise be lost. */
        clog_atomic_store_int(&q->consumer_parked, 1);
        head = clog_atomic_load_sz(&q->head);
        tail = clog_atomic_load_sz(&q->tail);
        if (head - tail > 0) {
            clog_atomic_store_int(&q->consumer_parked, 0);
            return 0;
        }
        if (clog_atomic_load_int(&q->closed)) {
            clog_atomic_store_int(&q->consumer_parked, 0);
            return -1;
        }
        clog_sem_wait(&q->items_sem);
        clog_atomic_store_int(&q->consumer_parked, 0);
        /* Loop: spurious wake-ups (incl. close's empty post) re-block here. */
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
        if (log_record_is_inline(&slot->rec)) {
            /* Pointers reference the ring slot — rebase into our own batch copy. */
            log_record_rebase_inline(&records[n], &slot->rec);
        }
        n++;
    }
    if (n == 0) {
        return 0;
    }

    /* Advance our consumer read position. */
    clog_atomic_store_sz(&q->tail, tail + n);

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
     * unpublished (producers preempted between fetch_add and write), the
     * committed producers are guaranteed to publish and wake us, so re-wait.
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
