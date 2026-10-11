/**
 * @file socket_ring.c
 * @brief Lock-free async non-blocking socket writer with ring buffer and
 * exponential backoff.
 *
 * Implements the ring buffer for formatted log lines and a background writer
 * thread that drains the buffer over a TCP/TLS socket with non-blocking I/O.
 *
 * ## Lock-free Design
 *
 * The ring buffer uses atomic compare-exchange on `head` for producer slot
 * claiming — multiple producer threads may call @ref socket_ring_put concurrently
 * without any mutex. When the buffer is full, the new line is dropped (lossy)
 * and the `dropped` counter is incremented. The single consumer (writer thread)
 * blocks on a semaphore (`items_sem`) and reads from `tail`.
 */

#include "clog_port.h"
#include "socket_async.h"
#include <stdlib.h>
#include <string.h>

/* ── Ring Buffer Implementation ── */

static size_t sk_next_pow2(size_t n)
{
    if (n < 2) {
        return 2;
    }
    size_t p = 1;
    while (p < n) {
        p <<= 1;
    }
    return p;
}

socket_ring_buffer_t *socket_ring_create(size_t capacity)
{
    if (capacity == 0) {
        return NULL;
    }

    socket_ring_buffer_t *ring = calloc(1, sizeof(socket_ring_buffer_t));
    if (!ring) {
        return NULL;
    }

    size_t cap = sk_next_pow2(capacity);

    ring->slots = calloc(cap, sizeof(socket_ring_slot_t));
    if (!ring->slots) {
        free(ring);
        return NULL;
    }

    ring->capacity = cap;
    ring->mask     = cap - 1;

    ring->head    = 0;
    ring->tail    = 0;
    ring->count   = 0;
    ring->dropped = 0;
    ring->closed  = 0;

    /* Slot i expects `seq == i + 1` for the first line at absolute position i. */
    for (size_t i = 0; i < cap; i++) {
        ring->slots[i].seq = (uint64_t)i;
    }

    if (clog_sem_init(&ring->items_sem, 0) != 0) {
        free(ring->slots);
        free(ring);
        return NULL;
    }

    return ring;
}

int socket_ring_put(socket_ring_buffer_t *ring, const char *line, size_t len)
{
    if (!ring || !line || len == 0) {
        return -1;
    }

    /* Fast check: closed ring rejects puts. */
    if (clog_atomic_load_int(&ring->closed)) {
        return -1;
    }

    /*
     * Lock-free producer path: use a CAS loop to atomically claim a slot in
     * `head`. This is the hot path — no mutex is ever acquired.
     */
    for (;;) {
        size_t head = clog_atomic_load_sz(&ring->head);
        size_t tail = clog_atomic_load_sz(&ring->tail);

        if (head - tail >= ring->capacity) {
            /*
             * Ring is full — drop the new entry (lossy backpressure).
             * This is intentional for a logging ring buffer: blocking the
             * caller would be worse than dropping a message.
             */
            clog_atomic_inc64(&ring->dropped);
            return 0;
        }

        if (clog_atomic_cas_sz(&ring->head, &head, head + 1)) {
            /*
             * Successfully claimed slot `head`. Allocate a copy of the line
             * and write it, then publish with a release-store of seq (making
             * the write visible to the consumer) before signalling.
             */
            socket_ring_slot_t *slot = &ring->slots[head & ring->mask];
            if (len < SOCKET_RING_INLINE_LEN) {
                memcpy(slot->inline_buf, line, len);
                slot->inline_buf[len] = '\0';
                slot->line            = slot->inline_buf;
                slot->len             = len;
                slot->heap            = 0;
            } else {
                char *copy = malloc(len + 1);
                if (!copy) {
                    /*
                     * Allocation failed. The slot is already claimed — rolling
                     * head back would corrupt other producers' claims, so publish
                     * an empty slot instead and count it as dropped. The consumer
                     * frees NULL safely and skips zero-length lines.
                     */
                    slot->line = NULL;
                    slot->len  = 0;
                    slot->heap = 0;
                    clog_atomic_store_u64(&slot->seq, (uint64_t)head + 1);
                    clog_atomic_fetch_add_sz(&ring->count, 1);
                    clog_sem_post(&ring->items_sem);
                    clog_atomic_inc64(&ring->dropped);
                    return -1;
                }
                memcpy(copy, line, len);
                copy[len] = '\0';

                slot->line = copy;
                slot->len  = len;
                slot->heap = 1;
            }

            clog_atomic_store_u64(&slot->seq, (uint64_t)head + 1);
            clog_atomic_fetch_add_sz(&ring->count, 1);
            clog_sem_post(&ring->items_sem);
            return 0;
        }
        /* CAS failed — another producer claimed the slot; retry. */
    }
}

int socket_ring_get_batch(
    socket_ring_buffer_t *ring, const char **lines, size_t *lengths, int *heaps, size_t max_lines)
{
    if (!ring || !lines || !lengths || !heaps || max_lines == 0) {
        return -1;
    }

    /*
     * Block until at least one item is available or the ring is closed.
     * The semaphore is posted by producers after each put, and by
     * socket_ring_close() / socket_ring_signal() to wake blocked consumers.
     *
     * A closed ring must never block: the closed flag is checked before
     * waiting (fast path for callers that come back after close) and again
     * after waking, so a close racing with the wait still terminates through
     * the wake-up post. Without the pre-wait check, a second get_batch on a
     * closed empty ring would consume the close's post and then block
     * forever (semaphore count already zero).
     */
    for (;;) {
        /*
         * Closed AND empty → terminate immediately. This keeps repeated
         * calls after close from blocking on the consumed wake-up post
         * (lost-wakeup deadlock). Closed-but-nonempty still returns the
         * remaining items below (the semaphore count can never be zero while
         * items exist), preserving drain-on-close semantics.
         */
        if (clog_atomic_load_int(&ring->closed) &&
            clog_atomic_load_sz(&ring->head) == clog_atomic_load_sz(&ring->tail)) {
            return -1;
        }
        clog_sem_wait(&ring->items_sem);

        /* Load the latest head (producer writes) and local tail (consumer reads). */
        size_t head = clog_atomic_load_sz(&ring->head);
        size_t tail = clog_atomic_load_sz(&ring->tail);

        size_t available = head - tail;

        if (available == 0) {
            /*
             * Woken by close/signal with no actual items to read.
             * If closed, signal shutdown; otherwise keep waiting.
             */
            if (clog_atomic_load_int(&ring->closed)) {
                return -1;
            }
            continue;
        }

        /*
         * Only read slots whose producer has published them (seq == pos + 1).
         * A producer that won the head CAS but was preempted before writing has
         * committed — it will finish and post items_sem, so a short yield and a
         * retry from the caller is safe.
         */
        size_t n = 0;
        while (n < available && n < max_lines) {
            size_t              pos  = tail + n;
            socket_ring_slot_t *slot = &ring->slots[pos & ring->mask];
            if (clog_atomic_load_u64(&slot->seq) != (uint64_t)pos + 1) {
                break;
            }
            lines[n]   = slot->line;
            lengths[n] = slot->len;
            heaps[n]   = slot->heap;
            slot->line = NULL;
            slot->len  = 0;
            slot->heap = 0;
            n++;
        }

        if (n == 0) {
            /* Claimed but unpublished slots — yield once and report empty; the
             * committed producer's post will wake the next get_batch. */
            clog_sleep_ms(0);
            continue;
        }

        /* Advance the consumer read position. */
        clog_atomic_store_sz(&ring->tail, tail + n);
        clog_atomic_fetch_sub_sz(&ring->count, n);

        return (int)n;
    }
}

void socket_ring_close(socket_ring_buffer_t *ring)
{
    if (!ring) {
        return;
    }

    clog_atomic_store_int(&ring->closed, 1);

    /*
     * Post to the semaphore so any blocked consumer wakes up and can
     * observe the closed state.
     */
    clog_sem_post(&ring->items_sem);
}

void socket_ring_signal(socket_ring_buffer_t *ring)
{
    if (!ring) {
        return;
    }

    /*
     * Wake the consumer thread. The consumer will check the actual head/tail
     * after waking.
     */
    clog_sem_post(&ring->items_sem);
}

void socket_ring_destroy(socket_ring_buffer_t *ring)
{
    if (!ring) {
        return;
    }

    /* Wake any blocked consumer so it can observe closed state. */
    clog_atomic_store_int(&ring->closed, 1);
    clog_sem_post(&ring->items_sem);

    /* Free any remaining lines. */
    size_t head      = clog_atomic_load_sz(&ring->head);
    size_t tail      = clog_atomic_load_sz(&ring->tail);
    size_t remaining = head - tail;
    for (size_t i = 0; i < remaining && i < ring->capacity; i++) {
        size_t idx = (tail + i) & ring->mask;
        if (ring->slots[idx].heap) {
            free(ring->slots[idx].line);
        }
        ring->slots[idx].line = NULL;
        ring->slots[idx].len  = 0;
        ring->slots[idx].heap = 0;
    }

    clog_sem_destroy(&ring->items_sem);
    free(ring->slots);
    free(ring);
}

size_t socket_ring_depth(const socket_ring_buffer_t *ring)
{
    if (!ring) {
        return 0;
    }
    return clog_atomic_load_sz(&ring->head) - clog_atomic_load_sz(&ring->tail);
}
