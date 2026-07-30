/**
 * @file log_async.h
 * @brief Asynchronous logging worker and queue integration.
 *
 * Producers enqueue deep-copied @ref log_record_t values; one worker thread
 * dequeues and calls @ref log_dispatcher_dispatch.
 */

#ifndef LOG_ASYNC_H
#define LOG_ASYNC_H

#include "log_record.h"

typedef struct logger_t logger_t;

/* ── Instance API (_for variants) ── */

int log_async_init_for(logger_t *logger, int queue_size);
void log_async_shutdown_for(logger_t *logger);
void log_async_flush_for(logger_t *logger);
int log_async_is_running_for(logger_t *logger);
int log_async_write_for(logger_t *logger, log_record_t *restrict record);
size_t log_async_get_queue_depth_for(logger_t *logger);
void log_async_atfork_child_for(logger_t *logger);

/* ── Singleton API (default logger) ── */

int log_async_init(int queue_size);
void log_async_shutdown(void);
void log_async_flush(void);
int log_async_is_running(void);
int log_async_write(log_record_t *restrict record);
size_t log_async_get_queue_depth(void);
void log_async_atfork_child(void);

#endif /* LOG_ASYNC_H */
