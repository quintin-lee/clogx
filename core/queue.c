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

    clog_mutex_lock(&q->mutex);

    while (q->count == q->capacity && !q->closed) {
        clog_cond_wait(&q->not_full, &q->mutex);
    }

    if (q->closed) {
        clog_mutex_unlock(&q->mutex);
        return -1;
    }

    q->buffer[q->head] = *record;
    q->head = (q->head + 1) % q->capacity;
    q->count++;

    clog_cond_signal(&q->not_empty);
    clog_mutex_unlock(&q->mutex);

    return 0;
}

int mpsc_queue_try_put(mpsc_queue_t *restrict q, log_record_t *restrict record) {
    if (!q || !record)
        return -1;

    clog_mutex_lock(&q->mutex);

    if (q->closed || q->count == q->capacity) {
        clog_mutex_unlock(&q->mutex);
        return -1;
    }

    q->buffer[q->head] = *record;
    q->head = (q->head + 1) % q->capacity;
    q->count++;

    clog_cond_signal(&q->not_empty);
    clog_mutex_unlock(&q->mutex);

    return 0;
}

int mpsc_queue_get(mpsc_queue_t *restrict q, log_record_t *restrict record) {
    if (!q || !record)
        return -1;

    clog_mutex_lock(&q->mutex);

    while (q->count == 0) {
        if (q->closed) {
            clog_mutex_unlock(&q->mutex);
            return -1;
        }
        clog_cond_wait(&q->not_empty, &q->mutex);
    }

    *record = q->buffer[q->tail];
    q->tail = (q->tail + 1) % q->capacity;
    q->count--;

    if (q->count == 0) {
        clog_cond_broadcast(&q->drained);
    }

    clog_cond_signal(&q->not_full);
    clog_mutex_unlock(&q->mutex);

    return 0;
}

void mpsc_queue_close(mpsc_queue_t *q) {
    if (!q)
        return;

    clog_mutex_lock(&q->mutex);
    q->closed = 1;
    clog_cond_broadcast(&q->not_empty);
    clog_cond_broadcast(&q->not_full);
    clog_cond_broadcast(&q->drained);
    clog_mutex_unlock(&q->mutex);
}

void mpsc_queue_wait_empty(mpsc_queue_t *q) {
    if (!q)
        return;

    clog_mutex_lock(&q->mutex);
    while (q->count > 0) {
        clog_cond_wait(&q->drained, &q->mutex);
    }
    clog_mutex_unlock(&q->mutex);
}

void mpsc_queue_destroy(mpsc_queue_t *q) {
    if (!q)
        return;

    clog_mutex_lock(&q->mutex);
    clog_cond_broadcast(&q->not_full);
    clog_cond_broadcast(&q->not_empty);
    clog_cond_broadcast(&q->drained);
    clog_mutex_unlock(&q->mutex);

    clog_mutex_destroy(&q->mutex);
    clog_cond_destroy(&q->not_full);
    clog_cond_destroy(&q->not_empty);
    clog_cond_destroy(&q->drained);

    free(q->buffer);
    free(q);
}
