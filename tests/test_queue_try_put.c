/**
 * @file test_queue_try_put.c
 * @brief Regression test: MPSC queue try-put behavior under contention.
 */

#include "log_record.h"
#include "queue.h"
#include <stdio.h>
#include <string.h>

int main(void)
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
