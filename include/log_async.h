/**
 * @file log_async.h
 * @brief Asynchronous logging worker and queue management.
 *
 * ## Design
 *
 * When async mode is enabled, the logger does not call the dispatcher
 * directly from the producer thread. Instead, each log record is
 * deep-copied and enqueued into a @ref mpsc_queue_t (see queue.h).
 * A single dedicated worker thread dequeues records in batches (up to 64)
 * and forwards them to @ref log_dispatcher_dispatch.
 *
 * This decouples the logging hot path (the caller's thread) from I/O
 * (sink writes) and avoids blocking the application on slow outputs.
 *
 * ## Lifecycle
 *
 * ```
 * log_async_init ──► log_async_write (repeated) ──► log_async_flush ──► log_async_shutdown
 * ```
 *
 * ## Thread Safety
 *
 * - @ref log_async_write is safe for multiple concurrent producers
 *   (delegates to the queue's mutex-protected put).
 *
 * @dot "Async Logging Pipeline"
 * digraph async_pipeline {
 *     rankdir=LR;
 *     node [shape=box, style=filled, fontname="Helvetica", fontsize=10];
 *     edge [color="#666666", fontname="Helvetica", fontsize=9];
 *
 *     prod1 [label="Thread 1\nLOG_INFO()" fillcolor="#E3F2FD"];
 *     prod2 [label="Thread 2\nLOG_WARN()" fillcolor="#E3F2FD"];
 *     prodN [label="Thread N\n..." fillcolor="#E3F2FD"];
 *
 *     queue [label="MPSC Queue\n(lock-free put)\nring buffer" fillcolor="#FFF9C4" shape=cylinder];
 *
 *     worker [label="Worker Thread\nbatch dequeue (≤64)\ncondvar wait" fillcolor="#F3E5F5"];
 *
 *     batch [label="Batch Array\nlog_record_t[64]" fillcolor="#FFF3E0"];
 *
 *     dispatch [label="Dispatcher\nlevel filter → sinks" fillcolor="#E8F5E9"];
 *
 *     sinks [label="Sinks\n(console/file/socket/...)" fillcolor="#E0F2F1"];
 *
 *     prod1 -> queue [label="deep-copy\nput"];
 *     prod2 -> queue [label="deep-copy\nput"];
 *     prodN -> queue [label="deep-copy\nput"];
 *     queue -> worker [label="get_batch\n(≤64)"];
 *     worker -> batch;
 *     batch -> dispatch [label="for each\nrecord"];
 *     dispatch -> sinks;
 * }
 * @enddot
 */

#ifndef LOG_ASYNC_H
#define LOG_ASYNC_H

#include "log_record.h"

typedef struct logger_t logger_t;

/* ── Instance API (_for variants, scoped to a @ref logger_t) ── */

/**
 * @brief Start the async worker for a logger instance.
 * @param[in] logger     Logger instance.
 * @param[in] queue_size Capacity of the internal ring buffer.
 * @retval 0  Success.
 * @retval -1 Queue allocation or thread creation failure.
 */
int log_async_init_for(logger_t *logger, int queue_size);

/**
 * @brief Shut down the async worker for a logger instance.
 *
 * Closes the queue, waits for it to drain, then joins the worker thread.
 * After this call, @ref log_async_write_for returns -1.
 */
void log_async_shutdown_for(logger_t *logger);

/**
 * @brief Block until the async queue is fully drained.
 *
 * Ensures all enqueued records have been dispatched before returning.
 */
void log_async_flush_for(logger_t *logger);

/**
 * @brief Check if the async worker thread is running.
 * @return 1 if running, 0 if not initialised or shut down.
 */
int log_async_is_running_for(logger_t *logger);

/**
 * @brief Enqueue a deep-copied record for async processing.
 *
 * The record's string fields (message, file, func, module, tag) are
 * deep-copied so the caller's memory can be freed or reused immediately.
 * The worker thread dispatches via @ref log_dispatcher_dispatch_for.
 *
 * @param[in,out] logger  Logger instance.
 * @param[in]     record  Log record to enqueue (contents are copied).
 * @retval 0  Success.
 * @retval -1 Queue is full or shut down.
 */
int log_async_write_for(logger_t *logger, log_record_t *restrict record);

/**
 * @brief Get the current number of records waiting in the async queue.
 * @return Queue depth (0 if not initialised or shut down).
 */
size_t log_async_get_queue_depth_for(logger_t *logger);

/**
 * @brief Re-create the async worker in the child after fork(2).
 *
 * The parent's thread does not exist in the child; this function starts
 * a fresh worker. Safe to call only once after fork.
 */
void log_async_atfork_child_for(logger_t *logger);

/* ── Singleton API (default logger, same contract) ── */

/** @brief Singleton variant of @ref log_async_init_for. */
int log_async_init(int queue_size);
/** @brief Singleton variant of @ref log_async_shutdown_for. */
void log_async_shutdown(void);
/** @brief Singleton variant of @ref log_async_flush_for. */
void log_async_flush(void);
/** @brief Singleton variant of @ref log_async_is_running_for. */
int log_async_is_running(void);
/** @brief Singleton variant of @ref log_async_write_for. */
int log_async_write(log_record_t *restrict record);
/** @brief Singleton variant of @ref log_async_get_queue_depth_for. */
size_t log_async_get_queue_depth(void);
/** @brief Singleton variant of @ref log_async_atfork_child_for. */
void log_async_atfork_child(void);

#endif /* LOG_ASYNC_H */
