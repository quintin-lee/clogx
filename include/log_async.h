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

/**
 * @brief Start the async worker thread.
 *
 * @param[in] queue_size Capacity of the bounded queue (must be > 0).
 * @return 0 on success (or already running), -1 on error.
 */
int log_async_init(int queue_size);

/**
 * @brief Stop the async worker.
 *
 * Closes the queue, lets the worker drain remaining records, joins the
 * thread, and frees queue resources.
 */
void log_async_shutdown(void);

/**
 * @brief Wait until the async queue is empty, then flush sinks.
 */
void log_async_flush(void);

/**
 * @brief Query whether the async worker is active.
 * @return Non-zero if running.
 */
int log_async_is_running(void);

/**
 * @brief Enqueue a deep copy of @p record for asynchronous dispatch.
 *
 * Falls back to synchronous @ref log_dispatcher_dispatch if async is not
 * running, cloning fails, or the queue has been closed.
 *
 * @param[in] record Record whose string fields may point at stack storage.
 * @return 0 on successful enqueue (or sync fallback without clone failure),
 *         -1 if clone failed (sync fallback still attempted).
 */
int log_async_write(log_record_t *record);

#endif /* LOG_ASYNC_H */
