/**
 * @file dispatcher.h
 * @brief Fan-out of formatted log lines to configured sinks.
 *
 * ANSI color sequences are applied only for color-enabled console sinks so
 * file and socket outputs remain plain text.
 */

#ifndef DISPATCHER_H
#define DISPATCHER_H

#include "log_record.h"
#include "log_sink.h"

/**
 * @brief Destroy existing sinks (if any) and create sinks from current config.
 * @return 0 on success.
 */
int log_dispatcher_init(void);

/**
 * @brief Destroy all sinks and clear the sink list.
 *
 * @note The process-wide mutex is intentionally kept alive so init/reload
 *       can reuse it without undefined behavior.
 */
void log_dispatcher_destroy(void);

/**
 * @brief Flush every registered sink.
 */
void log_dispatcher_flush(void);

/**
 * @brief Format @p record and write it to every sink.
 * @param[in] record Log event to dispatch.
 * @return 0 on success, -1 on invalid input or format failure.
 *
 * @note Thread-safe.
 */
int log_dispatcher_dispatch(log_record_t *record);

/**
 * @brief Append a sink to the dispatcher.
 * @param[in] sink Sink to take ownership of (destroyed on @ref log_dispatcher_destroy).
 * @return 0 on success, -1 on error.
 */
int log_dispatcher_add_sink(log_sink_t *sink);

/**
 * @brief Remove a sink from the list without destroying it.
 * @param[in] sink Sink pointer previously added.
 * @return 0 on success, -1 if @p sink is NULL.
 *
 * @note Caller retains ownership and must call @c sink->destroy if needed.
 */
int log_dispatcher_remove_sink(log_sink_t *sink);

#endif /* DISPATCHER_H */
