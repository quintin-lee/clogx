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

// Returns non-zero if the async worker is active
int log_async_is_running(void);

// Write a log record asynchronously (blocks if queue is full)
int log_async_write(log_record_t *record);

#endif // LOG_ASYNC_H
