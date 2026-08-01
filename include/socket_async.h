/**
 * @file socket_async.h
 * @brief Async non-blocking socket writer with lock-free ring buffer and
 * exponential backoff.
 *
 * @details Provides a background writer thread that drains a ring buffer of
 * formatted log lines and sends them over a TCP/TLS socket. When the remote
 * endpoint is unavailable, the writer sleeps with exponential backoff before
 * retrying, preventing tight reconnect loops and protecting the caller from
 * blocking on slow or downed receivers.
 *
 * ## Architecture
 *
 * @dot "Async Socket Writer"
 * digraph async_socket {
 *     rankdir=LR;
 *     node [shape=box, style=filled, fontname="Helvetica", fontsize=10];
 *     edge [color="#666666", fontname="Helvetica", fontsize=9];
 *
 *     caller [label="Caller Thread\n(socket_write)" fillcolor="#E3F2FD"];
 *     ring [label="Ring Buffer\n(char* slots)\nlock-free CAS head" fillcolor="#FFF9C4"
 * shape=cylinder]; writer [label="Writer Thread\nnon-blocking send\nexponential backoff"
 * fillcolor="#F3E5F5"]; socket [label="TCP/TLS Socket\n(non-blocking)" fillcolor="#E8F5E9"]; remote
 * [label="Remote\n(Logstash/OTel/Vector)" fillcolor="#E0F2F1"];
 *
 *     caller -> ring [label="put\n(lock-free CAS)"];
 *     ring -> writer [label="get\n(semaphore wait)"];
 *     writer -> socket [label="send\n(EAGAIN → retry)"];
 *     socket -> remote;
 * }
 * @enddot
 *
 * ## Lock-free Producer Path
 *
 * The ring buffer uses atomic compare-exchange on `head` for slot claiming.
 * Multiple producer threads may call @ref socket_ring_put concurrently without
 * any mutex. When the buffer is full, the oldest entry is overwritten — the
 * producer atomically advances `tail` and frees the displaced line. A
 * per-slot sequence counter prevents read/write races between the producer's
 * overwrite and the consumer's dequeue.
 *
 * ## Exponential Backoff
 *
 * When a send or connect fails:
 *   1. The current backoff delay is @ref backoff_min_ms.
 *   2. After each failure, delay *= 2 (capped at @ref backoff_max_ms).
 *   3. A small random jitter (±10%) is added to prevent thundering herd.
 *   4. On successful send, backoff resets to @ref backoff_min_ms.
 *
 * ## Thread Safety
 *
 * - @ref socket_ring_put is safe for multiple concurrent callers (lock-free).
 * - @ref socket_writer_start/stop must be called from a single thread.
 * - @ref socket_ring_destroy must be called after all producers and the
 *   writer thread have stopped.
 */

#ifndef SOCKET_ASYNC_H
#define SOCKET_ASYNC_H

#include "clog_port.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @struct socket_ring_slot_t
 * @brief A single slot in the ring buffer holding a formatted log line.
 *
 * `seq` starts at the slot index (0..capacity-1) and is set to
 * `absolute position + 1` by the producer with a release-store after the
 * line pointer is fully written. The consumer acquire-loads `seq` and only
 * reads the slot when it equals `position + 1` — closing the window between
 * the head CAS and the actual write.
 */
typedef struct {
    char             *line; /**< Heap-allocated log line (ownership transferred to ring). */
    size_t            len;  /**< Length of @ref line (excluding NUL). */
    volatile uint64_t seq;  /**< Publish counter — release-stored by producer,
                                 acquire-loaded by consumer. */
} socket_ring_slot_t;

/**
 * @struct socket_ring_buffer_t
 * @brief Lock-free MPSC circular buffer for formatted log lines.
 *
 * Producers call @ref socket_ring_put to enqueue a line. The background
 * writer thread calls @ref socket_ring_get_batch to dequeue in bulk.
 * When the buffer is full, the oldest entry is dropped (lossy design
 * — logging must never block the application).
 *
 * Internally, each slot carries a sequence counter (`seq`) so that producers
 * and the single consumer can operate without a mutex. Producers claim slots
 * by atomically advancing `head` (via CAS); only the consumer advances `tail`.
 */
typedef struct {
    socket_ring_slot_t *slots;     /**< Ring buffer slots. */
    size_t              capacity;  /**< Maximum number of slots (power-of-2). */
    size_t              mask;      /**< capacity - 1 (for fast bitwise modulo). */
    volatile size_t     head;      /**< Next write index (producer-side, atomic CAS). */
    volatile size_t     tail;      /**< Next read index (consumer-side, atomic). */
    volatile size_t     count;     /**< Approximate current entry count. */
    volatile uint64_t   dropped;   /**< Total lines dropped (buffer full). */
    volatile int        closed;    /**< Non-zero after @ref socket_ring_close. */
    clog_sem_t          items_sem; /**< Semaphore: items available for consumer. */
} socket_ring_buffer_t;

/**
 * @brief Get the current number of entries in the ring buffer (approximate).
 *
 * @param ring  Ring buffer instance.
 * @return Approximate count of buffered entries, or 0 if ring is NULL.
 */
size_t socket_ring_depth(const socket_ring_buffer_t *ring);

/**
 * @struct socket_writer_config_t
 * @brief Configuration for the async socket writer.
 */
typedef struct {
    const char *host;           /**< Remote host. */
    int         port;           /**< Remote port. */
    bool        use_tls;        /**< Enable TLS (requires CLOG_USE_TLS). */
    const char *ca_file;        /**< CA certificate path for TLS, or NULL. */
    bool        skip_verify;    /**< Skip TLS certificate verification. */
    size_t      ring_capacity;  /**< Ring buffer capacity (number of lines). 0 = 8192. */
    uint32_t    backoff_min_ms; /**< Initial backoff delay in ms. */
    uint32_t    backoff_max_ms; /**< Maximum backoff delay in ms. */
} socket_writer_config_t;

/**
 * @struct socket_writer_t
 * @brief Background writer thread that drains a ring buffer over TCP/TLS.
 *
 * Created by @ref socket_writer_start. The writer runs until
 * @ref socket_writer_stop is called, then drains remaining entries
 * before exiting.
 */
typedef struct socket_writer_t socket_writer_t;

/* ── Ring Buffer API ── */

/**
 * @brief Allocate a lock-free ring buffer for formatted log lines.
 *
 * @param capacity  Maximum number of lines. Rounded up to a power of 2.
 * @return New ring buffer, or NULL on allocation failure.
 */
socket_ring_buffer_t *socket_ring_create(size_t capacity);

/**
 * @brief Enqueue a formatted log line (non-blocking, lossy).
 *
 * Copies the line into the ring. If the ring is full, the oldest entry
 * is dropped and @ref socket_ring_buffer_t::dropped is incremented.
 *
 * This function is **lock-free** — multiple producer threads may call it
 * concurrently without contention.
 *
 * @param ring  Ring buffer instance.
 * @param line  NUL-terminated log line (copied internally).
 * @param len   Length of @p line (excluding NUL).
 * @retval 0   Success.
 * @retval -1  Ring is NULL, closed, or line is NULL.
 */
int socket_ring_put(socket_ring_buffer_t *ring, const char *line, size_t len);

/**
 * @brief Dequeue up to @p max_lines entries in one call.
 *
 * Blocks on a semaphore if the ring is empty (unless closed).
 *
 * @param ring       Ring buffer instance.
 * @param lines      Output array of const char* (pointers into ring-owned storage).
 * @param lengths    Output array of line lengths.
 * @param max_lines  Capacity of output arrays.
 * @retval >0  Number of lines dequeued.
 * @retval 0   Ring is empty (should not happen under normal usage).
 * @retval -1  Ring is closed and drained.
 */
int socket_ring_get_batch(socket_ring_buffer_t *ring,
                          const char          **lines,
                          size_t               *lengths,
                          size_t                max_lines);

/**
 * @brief Signal the ring buffer to close (no more puts accepted).
 *
 * After this call, @ref socket_ring_put returns -1. The writer thread
 * drains remaining entries before exiting.
 *
 * @param ring  Ring buffer instance.
 */
void socket_ring_close(socket_ring_buffer_t *ring);

/**
 * @brief Wake the writer thread (e.g. after close or to force a flush).
 *
 * @param ring  Ring buffer instance.
 */
void socket_ring_signal(socket_ring_buffer_t *ring);

/**
 * @brief Destroy the ring buffer and free all owned memory.
 *
 * @param ring  Ring buffer instance (NULL-safe).
 */
void socket_ring_destroy(socket_ring_buffer_t *ring);

/* ── Writer Thread API ── */

/**
 * @brief Start the background writer thread.
 *
 * Creates a non-blocking TCP/TLS connection to the configured endpoint
 * and begins draining the ring buffer. Reconnection uses exponential
 * backoff with jitter.
 *
 * @param config  Writer configuration (host, port, TLS, backoff params).
 * @return Writer instance, or NULL on error.
 */
socket_writer_t *socket_writer_start(const socket_writer_config_t *config);

/**
 * @brief Get the ring buffer from a writer (for enqueuing lines).
 *
 * @param writer  Writer instance.
 * @return Ring buffer (valid until @ref socket_writer_stop).
 */
socket_ring_buffer_t *socket_writer_ring(socket_writer_t *writer);

/**
 * @brief Stop the writer thread, drain remaining lines, and destroy the ring.
 *
 * Signals the ring to close, waits for the writer to drain, joins the thread,
 * then destroys the ring buffer. After this call the writer must not be reused.
 *
 * @param writer  Writer instance (NULL-safe).
 */
void socket_writer_stop(socket_writer_t *writer);

/**
 * @brief Get the number of lines dropped by the ring buffer.
 *
 * @param writer  Writer instance.
 * @return Total dropped count (lossy backpressure).
 */
uint64_t socket_writer_dropped(const socket_writer_t *writer);

#endif /* SOCKET_ASYNC_H */