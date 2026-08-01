/**
 * @file test_queue_try_put.c
 * @brief Regression tests: MPSC queue lock-free try-put, full-put, get, and
 *        multi-producer concurrency.
 */

#include "clog_port.h"
#include "log_record.h"
#include "queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_PRODUCERS 8
#define LOGS_PER_PRODUCER 1000

static int basic_try_put_test(void)
{
    mpsc_queue_t *q = mpsc_queue_create(1);
    if (!q) {
        fprintf(stderr, "create failed\n");
        return 1;
    }

    log_record_t a;
    log_record_t b;
    log_record_t out;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.message = "first";
    b.message = "second";

    if (mpsc_queue_try_put(q, &a) != 0) {
        fprintf(stderr, "first try_put should succeed\n");
        mpsc_queue_destroy(q);
        return 1;
    }

    if (mpsc_queue_try_put(q, &b) == 0) {
        fprintf(stderr, "second try_put should fail when full\n");
        mpsc_queue_destroy(q);
        return 1;
    }

    if (mpsc_queue_get(q, &out) != 0 || strcmp(out.message, "first") != 0) {
        fprintf(stderr, "get mismatch\n");
        mpsc_queue_destroy(q);
        return 1;
    }

    if (mpsc_queue_try_put(q, &b) != 0) {
        fprintf(stderr, "try_put after drain should succeed\n");
        mpsc_queue_destroy(q);
        return 1;
    }

    mpsc_queue_close(q);
    if (mpsc_queue_try_put(q, &a) == 0) {
        fprintf(stderr, "try_put on closed queue should fail\n");
        mpsc_queue_destroy(q);
        return 1;
    }

    mpsc_queue_destroy(q);
    printf("queue try_put test passed\n");
    return 0;
}

typedef struct {
    mpsc_queue_t *q;
    int           id;
    int           errors;
} producer_ctx_t;

static void *producer_thread(void *arg)
{
    producer_ctx_t *ctx = (producer_ctx_t *)arg;

    for (int i = 0; i < LOGS_PER_PRODUCER; i++) {
        log_record_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.message = "msg";
        rec.tid     = (uint32_t)ctx->id;
        rec.level   = (uint8_t)i;

        /* try_put may fail when the queue is briefly full — retry. */
        int retries = 0;
        while (mpsc_queue_try_put(ctx->q, &rec) != 0) {
            if (__atomic_load_n(&ctx->q->closed, __ATOMIC_ACQUIRE)) {
                ctx->errors++;
                break;
            }
            if (retries < 10000) {
                clog_sleep_ms(0);
                retries++;
            } else {
                ctx->errors++;
                break;
            }
        }
    }

    return NULL;
}

typedef struct {
    mpsc_queue_t *q;
    int          *received;
    int          *errors;
    int           total;
    int           producer_count;
} consumer_ctx_t;

static void *consumer_thread(void *arg)
{
    consumer_ctx_t *ctx = (consumer_ctx_t *)arg;

    int per_producer[NUM_PRODUCERS];
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        per_producer[i] = 0;
    }

    int total = 0;
    while (total < ctx->total) {
        log_record_t batch[64];
        int          n = mpsc_queue_get_batch(ctx->q, batch, 64);
        if (n < 0) {
            break;
        }

        for (int i = 0; i < n; i++) {
            if (batch[i].tid < (uint32_t)ctx->producer_count) {
                per_producer[batch[i].tid]++;
            } else {
                ctx->errors[0]++;
            }
            total++;
        }
    }

    /* Verify each producer's records were all received. */
    for (int i = 0; i < ctx->producer_count; i++) {
        if (per_producer[i] != LOGS_PER_PRODUCER) {
            fprintf(stderr,
                    "consumer: producer %d delivered %d (expected %d)\n",
                    i,
                    per_producer[i],
                    LOGS_PER_PRODUCER);
            ctx->errors[0]++;
        }
    }

    ctx->received[0] = total;
    return NULL;
}

static int mpsc_concurrency_test(void)
{
    mpsc_queue_t *q = mpsc_queue_create(8192);
    if (!q) {
        fprintf(stderr, "create failed\n");
        return 1;
    }

    int expected = NUM_PRODUCERS * LOGS_PER_PRODUCER;

    /* Start consumer first so it can drain as producers write. */
    int            received = 0;
    int            errors   = 0;
    consumer_ctx_t consumer_ctx;
    consumer_ctx.q              = q;
    consumer_ctx.received       = &received;
    consumer_ctx.errors         = &errors;
    consumer_ctx.total          = expected;
    consumer_ctx.producer_count = NUM_PRODUCERS;

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
        ctxs[i].errors = 0;
        if (clog_thread_create(&threads[i], producer_thread, &ctxs[i]) != 0) {
            fprintf(stderr, "thread create failed for producer %d\n", i);
            mpsc_queue_close(q);
            mpsc_queue_destroy(q);
            return 1;
        }
    }

    /* Wait for all producers to finish. */
    int producer_errors = 0;
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        clog_thread_join(threads[i]);
        producer_errors += ctxs[i].errors;
    }

    /* Close the queue and wait for consumer to finish. */
    mpsc_queue_close(q);
    clog_thread_join(consumer_tid);
    mpsc_queue_destroy(q);

    if (errors > 0) {
        fprintf(stderr, "mpsc concurrency: %d consumer errors\n", errors);
        return 1;
    }

    if (received != expected) {
        fprintf(stderr, "mpsc concurrency: expected %d records, got %d\n", expected, received);
        return 1;
    }

    if (producer_errors > 0) {
        fprintf(stderr, "mpsc concurrency: %d producer errors (try_put failed)\n", producer_errors);
        return 1;
    }

    printf("mpsc concurrency: %d/%d records enqueued/dequeued OK\n", received, expected);
    printf("queue mpsc concurrency test passed\n");
    return 0;
}

int main(void)
{
    if (basic_try_put_test() != 0) {
        return 1;
    }
    if (mpsc_concurrency_test() != 0) {
        return 1;
    }
    return 0;
}
