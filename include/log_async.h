#ifndef LOG_ASYNC_H
#define LOG_ASYNC_H

#include "log_record.h"

// Initialize async logger with specified queue size
// Returns 0 on success, -1 on error
int log_async_init(int queue_size);

// Shutdown async logger (drains queue and frees pending records)
void log_async_shutdown(void);

// Wait until the async queue is empty, then flush sinks
void log_async_flush(void);

// Write a log record asynchronously (blocks if queue is full)
int log_async_write(log_record_t *record);

// Set custom dispatch function (optional)
void log_async_set_dispatch(void (*func)(log_record_t *));

#endif // LOG_ASYNC_H
