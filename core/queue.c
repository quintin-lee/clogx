#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "queue.h"

mpsc_queue_t *mpsc_queue_create(size_t capacity) {
    mpsc_queue_t *q = malloc(sizeof(mpsc_queue_t));
    if (!q) return NULL;

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

    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_full, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->drained, NULL);

    return q;
}

int mpsc_queue_put(mpsc_queue_t *q, log_record_t *record) {
    if (!q || !record) return -1;

    pthread_mutex_lock(&q->mutex);

    while (q->count == q->capacity && !q->closed) {
        pthread_cond_wait(&q->not_full, &q->mutex);
    }

    if (q->closed) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    q->buffer[q->head] = *record;
    q->head = (q->head + 1) % q->capacity;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);

    return 0;
}

int mpsc_queue_get(mpsc_queue_t *q, log_record_t *record) {
    if (!q || !record) return -1;

    pthread_mutex_lock(&q->mutex);

    while (q->count == 0) {
        if (q->closed) {
            pthread_mutex_unlock(&q->mutex);
            return -1;
        }
        pthread_cond_wait(&q->not_empty, &q->mutex);
    }

    *record = q->buffer[q->tail];
    q->tail = (q->tail + 1) % q->capacity;
    q->count--;

    if (q->count == 0) {
        pthread_cond_broadcast(&q->drained);
    }

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);

    return 0;
}

void mpsc_queue_close(mpsc_queue_t *q) {
    if (!q) return;

    pthread_mutex_lock(&q->mutex);
    q->closed = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->not_full);
    pthread_cond_broadcast(&q->drained);
    pthread_mutex_unlock(&q->mutex);
}

void mpsc_queue_wait_empty(mpsc_queue_t *q) {
    if (!q) return;

    pthread_mutex_lock(&q->mutex);
    while (q->count > 0) {
        pthread_cond_wait(&q->drained, &q->mutex);
    }
    pthread_mutex_unlock(&q->mutex);
}

void mpsc_queue_destroy(mpsc_queue_t *q) {
    if (!q) return;

    pthread_mutex_lock(&q->mutex);
    pthread_cond_broadcast(&q->not_full);
    pthread_cond_broadcast(&q->not_empty);
    pthread_cond_broadcast(&q->drained);
    pthread_mutex_unlock(&q->mutex);

    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->drained);

    free(q->buffer);
    free(q);
}
