/**
 * @file dispatcher.h
 * @brief Fan-out of formatted log lines to all configured sinks.
 *
 * ## Architecture
 *
 * The dispatcher holds an array of @ref log_sink_t pointers and, on each
 * @ref log_dispatcher_dispatch call, formats the record once then writes
 * the formatted line to every sink. ANSI color sequences are applied only
 * for console sinks that have color enabled; file and socket outputs
 * always receive plain text.
 *
 * ## Atomic Reload
 *
 * To support hot reconfiguration (@ref log_reload), the dispatcher uses a
 * snapshot pattern:
 * 1. @ref log_dispatcher_build_snapshot creates a new sink set from config
 *    without touching the active set.
 * 2. @ref log_dispatcher_commit_snapshot swaps the pointer atomically.
 * 3. The old snapshot is destroyed after ensuring no dispatcher call is
 *    in flight (guarded by a read-write lock).
 *
 * This ensures that log lines are never written to a partially-configured
 * or destroyed sink during reload.
 *
 * ## Thread Safety
 *
 * - @ref log_dispatcher_dispatch is thread-safe (read-locked).
 * - @ref log_dispatcher_init and @ref log_dispatcher_destroy must be
 *   called while the logger is quiesced.
 * - @ref log_dispatcher_add_sink and @ref log_dispatcher_remove_sink are
 *   mutex-protected.
 *
 * @see log_sink.h for the sink interface each dispatcher entry calls.
 */

#ifndef DISPATCHER_H
#define DISPATCHER_H

#include "log_record.h"
#include "log_sink.h"
#include "log_config.h"

/* Opaque logger_t forward declaration for _for variants. */
typedef struct logger_t logger_t;

/**
 * @struct log_dispatcher_snapshot_t
 * @brief Opaque snapshot of sink configuration used for atomic hot reload.
 *
 * Created by @ref log_dispatcher_build_snapshot and either committed
 * (ownership transferred to the dispatcher) or destroyed.
 */
typedef struct {
    log_sink_t **sinks; /**< Array of sink pointers (owned by the snapshot or dispatcher). */
    int sink_count;     /**< Number of elements in @ref sinks. */
} log_dispatcher_snapshot_t;

/**
 * @brief Initialise the default-logger dispatcher from the current config.
 *
 * Destroys any existing sinks, then reads @ref log_config to create the
 * appropriate set of sinks (console, file, socket, syslog) and stores
 * them internally.
 *
 * @retval 0  Success.
 * @retval -1 Sink creation failure (propagated from individual
 *            `*_sink_create` functions).
 */
int log_dispatcher_init(void);

/**
 * @brief Destroy all registered sinks and release the sink array.
 *
 * Each sink's `destroy` vfunc is called. The internal mutex is NOT
 * destroyed so that a subsequent init/reload can reuse it safely.
 */
void log_dispatcher_destroy(void);

/**
 * @brief Call `flush` on every registered sink.
 *
 * Ensures all buffered log data has been written to the underlying output
 * (file fsync, socket send, etc.).
 */
void log_dispatcher_flush(void);

/**
 * @brief Format a log record and write it to every registered sink.
 *
 * The record is formatted once via @ref log_formatter_format, then the
 * resulting line string is written to each sink through its `write` vfunc.
 * For console sinks with color enabled, ANSI-coloured output is produced
 * instead.
 *
 * @param[in] record  Log event to format and dispatch.
 * @retval 0   Success (written to at least one sink).
 * @retval -1  NULL argument or format failure.
 *
 * @note Thread-safe (acquires a read lock).
 */
int log_dispatcher_dispatch(log_record_t *restrict record);

/**
 * @brief Dynamically append a sink to the default dispatcher.
 *
 * The dispatcher takes ownership: @p sink will be destroyed during
 * @ref log_dispatcher_destroy or the next @ref log_dispatcher_init.
 *
 * @param[in] sink  Sink to add (must not be NULL). Ownership transferred.
 * @retval 0  Success.
 * @retval -1 NULL argument or allocation failure.
 */
int log_dispatcher_add_sink(log_sink_t *restrict sink);

/**
 * @brief Remove a sink from the dispatcher without destroying it.
 *
 * Ownership returns to the caller, who must eventually call
 * `sink->destroy()` to release resources.
 *
 * @param[in] sink  Sink pointer previously added via @ref log_dispatcher_add_sink.
 * @retval 0  Success.
 * @retval -1 @p sink is NULL or was not found in the current list.
 */
int log_dispatcher_remove_sink(log_sink_t *restrict sink);

/**
 * @brief Build a new dispatcher snapshot from configuration without
 *        modifying the active dispatcher.
 *
 * This is the first step of the atomic reload sequence. It allocates
 * a fresh sink array and creates each sink type according to @p cfg.
 * The caller must either commit the result or destroy it.
 *
 * @param[in]  cfg   Configuration to materialise.
 * @param[out] snap  Receives the newly built snapshot.
 * @retval 0  Success.
 * @retval -1 Sink creation or allocation failure (caller must call
 *            @ref log_dispatcher_destroy_snapshot on the partially-filled
 *            snapshot).
 */
int log_dispatcher_build_snapshot(log_config_t *restrict cfg,
                                  log_dispatcher_snapshot_t *restrict snap);

/**
 * @brief Free a snapshot's resources without affecting the active dispatcher.
 *
 * Calls `destroy` on each sink in the snapshot and frees the sink array.
 * Safe to call on a partially-built snapshot after a build failure.
 *
 * @param[in,out] snap  Snapshot to free (may be zeroed; NULL-safe).
 */
void log_dispatcher_destroy_snapshot(log_dispatcher_snapshot_t *restrict snap);

/**
 * @brief Atomically swap the active dispatcher with a new snapshot.
 *
 * The old sink array is destroyed. The new snapshot's sinks become the
 * active set. After this call, @p snap is invalid (ownership transferred).
 *
 * @param[in,out] snap  Fully built snapshot to commit.
 */
void log_dispatcher_commit_snapshot(log_dispatcher_snapshot_t *restrict snap);

/**
 * @brief Prepare the dispatcher for fork (called before fork).
 *
 * Acquires the internal lock so the child starts with a consistent view.
 */
void log_dispatcher_atfork_prepare(void);

/**
 * @brief Re-acquire the dispatcher lock in the parent after fork.
 */
void log_dispatcher_atfork_parent(void);

/**
 * @brief Re-initialise the dispatcher lock in the child after fork.
 *
 * The lock is reset to the initial unlocked state. The sink pointers
 * remain valid (file descriptors are inherited across fork).
 */
void log_dispatcher_atfork_child(void);

/* ── Instance variants (same contract, scoped to a @ref logger_t) ── */

/** @brief Instance variant of @ref log_dispatcher_dispatch. */
int log_dispatcher_dispatch_for(logger_t *logger, log_record_t *restrict record);
/** @brief Instance variant of @ref log_dispatcher_add_sink. */
int log_dispatcher_add_sink_for(logger_t *logger, log_sink_t *restrict sink);
/** @brief Instance variant of @ref log_dispatcher_remove_sink. */
int log_dispatcher_remove_sink_for(logger_t *logger, log_sink_t *restrict sink);
/** @brief Instance variant of @ref log_dispatcher_destroy. */
void log_dispatcher_destroy_for(logger_t *logger);
/** @brief Instance variant of @ref log_dispatcher_flush. */
void log_dispatcher_flush_for(logger_t *logger);
/** @brief Instance variant of @ref log_dispatcher_init. */
int log_dispatcher_init_for(logger_t *logger);
/** @brief Instance variant of @ref log_dispatcher_build_snapshot. */
int log_dispatcher_build_snapshot_for(logger_t *logger, log_config_t *restrict cfg,
                                      log_dispatcher_snapshot_t *restrict snap);
/** @brief Instance variant of @ref log_dispatcher_commit_snapshot. */
void log_dispatcher_commit_snapshot_for(logger_t *logger, log_dispatcher_snapshot_t *restrict snap);
/** @brief Instance variant of @ref log_dispatcher_destroy_snapshot. */
void log_dispatcher_destroy_snapshot_for(logger_t *logger,
                                         log_dispatcher_snapshot_t *restrict snap);
/** @brief Instance variant of @ref log_dispatcher_atfork_prepare. */
void log_dispatcher_atfork_prepare_for(logger_t *logger);
/** @brief Instance variant of @ref log_dispatcher_atfork_parent. */
void log_dispatcher_atfork_parent_for(logger_t *logger);
/** @brief Instance variant of @ref log_dispatcher_atfork_child. */
void log_dispatcher_atfork_child_for(logger_t *logger);

#endif /* DISPATCHER_H */
