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
#include "log_config.h"

/**
 * @brief Dispatcher snapshot used for atomic reload.
 */
typedef struct {
    log_sink_t **sinks;
    int sink_count;
} log_dispatcher_snapshot_t;

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
int log_dispatcher_dispatch(log_record_t *restrict record);

/**
 * @brief Append a sink to the dispatcher.
 * @param[in] sink Sink to take ownership of (destroyed on @ref log_dispatcher_destroy).
 * @return 0 on success, -1 on error.
 */
int log_dispatcher_add_sink(log_sink_t *restrict sink);

/**
 * @brief Remove a sink from the list without destroying it.
 * @param[in] sink Sink pointer previously added.
 * @return 0 on success, -1 if @p sink is NULL.
 *
 * @note Caller retains ownership and must call @c sink->destroy if needed.
 */
int log_dispatcher_remove_sink(log_sink_t *restrict sink);

/**
 * @brief Build a new dispatcher snapshot from config without touching global state.
 * @param[in]  cfg   Configuration to apply.
 * @param[out] snap  Receives the newly allocated snapshot.
 * @return 0 on success, -1 on error.
 *
 * @note Caller must call @ref log_dispatcher_destroy_snapshot on failure, or
 *       @ref log_dispatcher_commit_snapshot on success to take ownership.
 */
int log_dispatcher_build_snapshot(log_config_t *restrict cfg, log_dispatcher_snapshot_t *restrict snap);

/**
 * @brief Destroy a snapshot without affecting the global dispatcher.
 * @param[in,out] snap Snapshot to free.
 */
void log_dispatcher_destroy_snapshot(log_dispatcher_snapshot_t *restrict snap);

/**
 * @brief Atomically replace the global dispatcher with @p snap.
 * @param[in,out] snap Snapshot to commit (ownership transferred).
 */
void log_dispatcher_commit_snapshot(log_dispatcher_snapshot_t *restrict snap);

#endif /* DISPATCHER_H */
