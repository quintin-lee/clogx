#ifndef QUEUE_H
#define QUEUE_H

#include <stddef.h>
#include <pthread.h>
#include "log_record.h"

typedef struct mpsc_queue_t {
    log_record_t *buffer;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
} mpsc_queue_t;

mpsc_queue_t *mpsc_queue_create(size_t capacity);
int mpsc_queue_put(mpsc_queue_t *q, log_record_t *record);
int mpsc_queue_get(mpsc_queue_t *q, log_record_t *record);
void mpsc_queue_destroy(mpsc_queue_t *q);

#endif // QUEUE_H
