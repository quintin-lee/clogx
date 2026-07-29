/**
 * @file queue.c
 * @brief Mutex-protected bounded ring buffer used by the async logger.
 */
#include <stdlib.h>
#include <string.h>
#include "clog_port.h"
#include "queue.h"

mpsc_queue_t *mpsc_queue_create(size_t capacity) {
    mpsc_queue_t *q = malloc(sizeof(mpsc_queue_t));
    if (!q)
        return NULL;

    q->capacity = capacity;
    q->buffer = malloc(capacity * sizeof(log_record_t));
    if (!q->buffer) {
        free(q);
        return NULL;
    }

    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->closed = 0;

    clog_mutex_init(&q->mutex);
    clog_cond_init(&q->not_full);
    clog_cond_init(&q->not_empty);
    clog_cond_init(&q->drained);

    return q;
}

int mpsc_queue_put(mpsc_queue_t *restrict q, log_record_t *restrict record) {
    if (!q || !record)
        return -1;

    int ret = -1;
    CLOG_MUTEXGUARDED(&q->mutex, {
        while (q->count == q->capacity && !q->closed) {
            clog_cond_wait(&q->not_full, &q->mutex);
        }

        if (!q->closed) {
            q->buffer[q->head] = *record;
            q->head = (q->head + 1) % q->capacity;
            q->count++;
            clog_cond_signal(&q->not_empty);
            ret = 0;
        }
    });
    return ret;
}

int mpsc_queue_try_put(mpsc_queue_t *restrict q, log_record_t *restrict record) {
    if (!q || !record)
        return -1;

    int ret = -1;
    CLOG_MUTEXGUARDED(&q->mutex, {
        if (!q->closed && q->count < q->capacity) {
            q->buffer[q->head] = *record;
            q->head = (q->head + 1) % q->capacity;
            q->count++;
            clog_cond_signal(&q->not_empty);
            ret = 0;
        }
    });
    return ret;
}

int mpsc_queue_get(mpsc_queue_t *restrict q, log_record_t *restrict record) {
    return mpsc_queue_get_batch(q, record, 1) == 1 ? 0 : -1;
}

int mpsc_queue_get_batch(mpsc_queue_t *restrict q, log_record_t *restrict records,
                         size_t max_records) {
    if (!q || !records || max_records == 0)
        return -1;

    int count_to_get = -1;
    CLOG_MUTEXGUARDED(&q->mutex, {
        while (q->count == 0) {
            if (q->closed) {
                break;
            }
            clog_cond_wait(&q->not_empty, &q->mutex);
        }

        if (!q->closed || q->count > 0) {
            count_to_get = (int)(q->count < max_records ? q->count : max_records);
            for (int i = 0; i < count_to_get; i++) {
                records[i] = q->buffer[q->tail];
                q->tail = (q->tail + 1) % q->capacity;
                q->count--;
            }

            if (q->count == 0)
                clog_cond_broadcast(&q->drained);

            clog_cond_broadcast(&q->not_full);
        }
    });
    return count_to_get;
}

void mpsc_queue_close(mpsc_queue_t *q) {
    if (!q)
        return;

    CLOG_MUTEXGUARDED(&q->mutex, {
        q->closed = 1;
        clog_cond_broadcast(&q->not_empty);
        clog_cond_broadcast(&q->not_full);
        clog_cond_broadcast(&q->drained);
    });
}

void mpsc_queue_wait_empty(mpsc_queue_t *q) {
    if (!q)
        return;

    CLOG_MUTEXGUARDED(&q->mutex, {
        while (q->count > 0) {
            clog_cond_wait(&q->drained, &q->mutex);
        }
    });
}

void mpsc_queue_destroy(mpsc_queue_t *q) {
    if (!q)
        return;

    CLOG_MUTEXGUARDED(&q->mutex, {
        clog_cond_broadcast(&q->not_full);
        clog_cond_broadcast(&q->not_empty);
        clog_cond_broadcast(&q->drained);
    });
    clog_mutex_destroy(&q->mutex);
    clog_cond_destroy(&q->not_full);
    clog_cond_destroy(&q->not_empty);
    clog_cond_destroy(&q->drained);

    free(q->buffer);
    free(q);
}
