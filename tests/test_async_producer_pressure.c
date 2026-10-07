/**
 * @file test_async_producer_pressure.c
 * @brief Pressure test for the producer fast-path: 8 producers x 100k records
 *        through a small queue. Verifies accounting identity
 *        (real + tombstones + fails == attempts), final depth 0, and that
 *        inline records survive the queue round trip with rebased pointers.
 *
 *        The multi-producer part uses string literals (static storage, safe
 *        without clone); inline-rebase is covered deterministically by
 *        inline_rebase_test() below.
 */

#include "clog_port.h"
#include "log_record.h"
#include "queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_PRODUCERS 8
#define LOGS_PER_PRODUCER 100000
#define QUEUE_CAP 1024

typedef struct {
    mpsc_queue_t *q;
    int           id;
    long          ok;
    long          failed;
} producer_ctx_t;

static void *producer_thread(void *arg)
{
    producer_ctx_t *ctx = (producer_ctx_t *)arg;

    for (int i = 0; i < LOGS_PER_PRODUCER; i++) {
        log_record_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.message = "pressure-msg";
        rec.module  = "mod";
        rec.tid     = (uint32_t)ctx->id;
        rec.level   = (uint8_t)LOG_LEVEL_INFO;

        /* No retry: full-queue -1 is an expected outcome under pressure. */
        if (mpsc_queue_try_put(ctx->q, &rec) == 0) {
            ctx->ok++;
        } else {
            ctx->failed++;
        }
    }

    return NULL;
}

typedef struct {
    mpsc_queue_t *q;
    long          real;
    long          tombstones;
} consumer_ctx_t;

static void *consumer_thread(void *arg)
{
    consumer_ctx_t *ctx = (consumer_ctx_t *)arg;

    for (;;) {
        log_record_t batch[64];
        int          n = mpsc_queue_get_batch(ctx->q, batch, 64);
        if (n < 0) {
            break; /* Closed and drained. */
        }

        for (int i = 0; i < n; i++) {
            if (batch[i].is_tombstone) {
                ctx->tombstones++;
            } else {
                if (strcmp(batch[i].message, "pressure-msg") != 0 ||
                    strcmp(batch[i].module, "mod") != 0) {
                    fprintf(stderr, "content mismatch\n");
                    exit(1);
                }
                ctx->real++;
            }
        }
    }

    return NULL;
}

static int pressure_test(void)
{
    mpsc_queue_t *q = mpsc_queue_create(QUEUE_CAP);
    if (!q) {
        fprintf(stderr, "create failed\n");
        return 1;
    }

    long expected = (long)NUM_PRODUCERS * LOGS_PER_PRODUCER;

    consumer_ctx_t consumer_ctx;
    consumer_ctx.q          = q;
    consumer_ctx.real       = 0;
    consumer_ctx.tombstones = 0;

    clog_thread_t consumer_tid;
    if (clog_thread_create(&consumer_tid, consumer_thread, &consumer_ctx) != 0) {
        fprintf(stderr, "consumer thread create failed\n");
        mpsc_queue_destroy(q);
        return 1;
    }

    clog_thread_t  threads[NUM_PRODUCERS];
    producer_ctx_t ctxs[NUM_PRODUCERS];

    for (int i = 0; i < NUM_PRODUCERS; i++) {
        ctxs[i].q      = q;
        ctxs[i].id     = i;
        ctxs[i].ok     = 0;
        ctxs[i].failed = 0;
        if (clog_thread_create(&threads[i], producer_thread, &ctxs[i]) != 0) {
            fprintf(stderr, "thread create failed for producer %d\n", i);
            mpsc_queue_close(q);
            mpsc_queue_destroy(q);
            return 1;
        }
    }

    long ok     = 0;
    long failed = 0;
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        clog_thread_join(threads[i]);
        ok += ctxs[i].ok;
        failed += ctxs[i].failed;
    }

    mpsc_queue_close(q);
    clog_thread_join(consumer_tid);

    if (ok + failed != expected) {
        fprintf(stderr, "attempt accounting: ok %ld + failed %ld != %ld\n", ok, failed, expected);
        mpsc_queue_destroy(q);
        return 1;
    }

    if (consumer_ctx.real != ok || consumer_ctx.tombstones > failed) {
        fprintf(stderr,
                "drain accounting: real %ld (ok %ld) tombstones %ld (failed %ld)\n",
                consumer_ctx.real,
                ok,
                consumer_ctx.tombstones,
                failed);
        mpsc_queue_destroy(q);
        return 1;
    }

    if (consumer_ctx.real == 0) {
        fprintf(stderr, "no real records drained\n");
        mpsc_queue_destroy(q);
        return 1;
    }

    /* Depth must read 0: head == tail snapshot. */
    if (clog_atomic_load_sz(&q->head) != clog_atomic_load_sz(&q->tail)) {
        fprintf(stderr, "depth not zero after drain\n");
        mpsc_queue_destroy(q);
        return 1;
    }

    printf("pressure: %ld ok (%ld real + %ld tombstones), %ld failed, depth 0\n",
           ok,
           consumer_ctx.real,
           consumer_ctx.tombstones,
           failed);

    mpsc_queue_destroy(q);
    return 0;
}

/* Deterministic queue-layer rebase check: hand-build an inline record
 * (pointers into its own inline_buf), round-trip it, and verify the
 * dequeued copy points into its own inline_buf with intact content. */
static int inline_rebase_test(void)
{
    mpsc_queue_t *q = mpsc_queue_create(16);
    if (!q) {
        fprintf(stderr, "create failed\n");
        return 1;
    }

    log_record_t rec;
    memset(&rec, 0, sizeof(rec));
    memcpy(rec.inline_buf, "abc", 4);
    rec.message = rec.inline_buf;

    if (mpsc_queue_try_put(q, &rec) != 0) {
        fprintf(stderr, "inline try_put should succeed\n");
        mpsc_queue_destroy(q);
        return 1;
    }

    log_record_t out;
    memset(&out, 0, sizeof(out));
    if (mpsc_queue_get(q, &out) != 0) {
        fprintf(stderr, "inline get should succeed\n");
        mpsc_queue_destroy(q);
        return 1;
    }

    if (strcmp(out.message, "abc") != 0) {
        fprintf(stderr, "inline content mismatch\n");
        mpsc_queue_destroy(q);
        return 1;
    }

    if (out.message < out.inline_buf || out.message >= out.inline_buf + CLOG_MAX_INLINE_SIZE) {
        fprintf(stderr, "message not rebased into dequeued inline_buf\n");
        mpsc_queue_destroy(q);
        return 1;
    }

    mpsc_queue_destroy(q);
    printf("inline rebase test passed\n");
    return 0;
}

int main(void)
{
    if (inline_rebase_test() != 0) {
        return 1;
    }
    if (pressure_test() != 0) {
        return 1;
    }
    return 0;
}
