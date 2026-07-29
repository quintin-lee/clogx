# Design Spec: Async Queue Batching & I/O Optimization

**Date**: 2026-07-29  
**Topic**: Batch Dequeue and Flush Optimization for Async Worker Thread  
**Status**: Approved  

---

## 1. Overview

Currently, `async_worker()` in `core/async.c` pops log records one by one (`mpsc_queue_get`), clones/dispatches them individually through `log_dispatcher_dispatch()`, and flushes sinks per call or upon empty queue. Under high log velocity, individual popping and dispatching incurs high mutex lock/unlock and I/O syscall overhead.

This optimization introduces **Batch Dequeue**:
- The worker thread pops up to `CLOG_ASYNC_BATCH_SIZE` (default 32) records in a single batch.
- All records in the batch are dispatched consecutively.
- Sinks are flushed once at the end of each batch, drastically reducing mutex lock contention and file `fflush` / `write` syscall frequency.

---

## 2. Component Design

### 2.1 Queue Batch Popping (`include/queue.h` & `core/queue.c`)

Add `mpsc_queue_get_batch()` to `queue.h` / `queue.c`:

```c
/**
 * @brief Dequeue up to @p max_items into @p records array in a single lock acquisition.
 * @param[in]  queue     Target queue.
 * @param[out] records   Buffer array of log_record_t to store dequeued items.
 * @param[in]  max_items Maximum number of items to dequeue in one batch.
 * @return Number of items dequeued (0 if closed and empty, or timed out).
 */
size_t mpsc_queue_get_batch(mpsc_queue_t *queue, log_record_t *records, size_t max_items);
```

### 2.2 Worker Thread Batch Processing (`core/async.c`)

In `async_worker()`:

```c
#ifndef CLOG_ASYNC_BATCH_SIZE
#define CLOG_ASYNC_BATCH_SIZE 32
#endif

static void *async_worker(void *arg) {
    async_logger_t *logger = (async_logger_t *)arg;
    log_record_t batch[CLOG_ASYNC_BATCH_SIZE];

    while (logger->running) {
        size_t count = mpsc_queue_get_batch(logger->queue, batch, CLOG_ASYNC_BATCH_SIZE);
        if (count == 0) {
            break; /* Queue closed and empty */
        }

        logger->processing = 1;
        for (size_t i = 0; i < count; i++) {
            log_dispatcher_dispatch(&batch[i]);
            log_record_free_owned(&batch[i]);
        }
        log_dispatcher_flush();
        logger->processing = 0;
    }

    return NULL;
}
```

---

## 3. Test & Verification Plan

1. **Unit Test Updates**:
   - `test_queue_try_put.c`: Add `mpsc_queue_get_batch` unit test.
   - `test_async_lifecycle.c`: Verify batching behavior under multi-threaded logging.
2. **Build Suite Verification**:
   - `make check`
   - `make test-asan`
   - `make test-ubsan`
