/**
 * @file log_signal.h
 * @brief Internal signal handler installation and restoration for graceful
 *        shutdown.
 *
 * ## Purpose
 *
 * When `catch_signals: true` is set in the config (or
 * @ref log_install_signal_handlers is called), clogx installs POSIX
 * signal handlers for `SIGTERM` and `SIGINT`. On receiving either signal,
 * the handler sets a flag; the logger checks this flag during dispatch and
 * initiates a graceful shutdown (flush remaining records, stop the async
 * worker, close file descriptors).
 *
 * ## Signal Handling Strategy
 *
 * - The handler is **async-signal-safe**: it only writes to a `volatile
 *   sig_atomic_t` flag (and optionally to a self-pipe / signalfd).
 * - The actual shutdown work happens in the main logging thread or via
 *   @ref log_process_pending_signals, not in signal context.
 * - @ref log_restore_signal_handlers reverts to the previous disposition
 *   (either SIG_DFL or whatever was installed before clogx took over).
 *
 * @see log.h for the public @ref log_install_signal_handlers and
 *      @ref log_get_pending_signal API.
 */

#ifndef LOG_SIGNAL_H
#define LOG_SIGNAL_H

/**
 * @brief Restore the previous signal handlers for SIGTERM/SIGINT.
 *
 * Reverts to the `struct sigaction` that was saved when
 * @ref log_install_signal_handlers was first called. If the handlers
 * were never installed, this is a no-op.
 *
 * Safe to call multiple times.
 */
void log_restore_signal_handlers(void);

#endif /* LOG_SIGNAL_H */
