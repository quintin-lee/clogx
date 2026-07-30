/**
 * @file log.h
 * @brief Public application-facing API for the clogx logging library.
 *
 * @details Typical usage:
 * @code
 *   if (log_init("config.yaml") != 0) return 1;
 *   LOG_INFO("hello %d", 42);
 *   log_flush();
 *   log_destroy();
 * @endcode
 */

#ifndef LOG_H
#define LOG_H

#include <stdarg.h>
#include <string.h>
#include "log_config.h"
#include "log_record.h"
#include "log_sink.h"
#include "log_prometheus.h"
#include "clogx_plugin.h"

/* Compiler portability macros. */
#if defined(__GNUC__) || defined(__clang__)
#define CLOGX_PRINTF_FMT(n, m) __attribute__((format(printf, n, m)))
#define CLOGX_API __attribute__((visibility("default")))
#else
#define CLOGX_PRINTF_FMT(n, m)
#define CLOGX_API
#endif

/**
 * @enum clogx_errno_t
 * @brief Structured error codes returned by clogx APIs.
 */
typedef enum {
    CLOG_OK = 0,
    CLOG_ERR_INVALID_ARG = -1,
    CLOG_ERR_INIT_REENTRANT = -2,
    CLOG_ERR_CONFIG_OPEN = -3,
    CLOG_ERR_CONFIG_PARSE = -4,
    CLOG_ERR_NO_SINKS = -5,
    CLOG_ERR_FILE_OPEN = -6,
    CLOG_ERR_FILE_WRITE = -7,
    CLOG_ERR_QUEUE_FULL = -8,
    CLOG_ERR_THREAD_CREATE = -9,
    CLOG_ERR_SOCKET_CONNECT = -10,
    CLOG_ERR_OOM = -11,
    CLOG_ERR_RELOAD = -12,
} clogx_errno_t;

/**
 * @brief Get a human-readable string description for an error code.
 *
 * Maps a clogx error code (negative value from @ref clogx_errno_t) to a descriptive
 * null-terminated text string. This is useful for logging or reporting errors that
 * occur during clogx API calls. The function does NOT use thread-local storage; the
 * returned string is static and valid until the next call, making it safe to use
 * across threads without synchronization.
 *
 * @param[in] err Error code, typically a negative value returned by a clogx API
 *                (e.g., CLOG_ERR_INVALID_ARG, CLOG_ERR_CONFIG_OPEN, etc.). Passing
 *                CLOG_OK returns "success". Unknown codes return "unknown error".
 *
 * @return Pointer to a static null-terminated error message string. Never returns NULL.
 *
 * Note Example: if log_init() fails, you can call log_strerror(clogx_errno) to print
 *       the reason. The string content should not be modified or freed by the caller.
 */
CLOGX_API const char *log_strerror(int err);

/**
 * @brief Register a callback invoked when async mode degrades to sync.
 *
 * When the async queue becomes full during heavy load, clogx can optionally degrade
 * from asynchronous to synchronous logging: instead of dropping queued records, it
 * blocks the caller thread until space is available, effectively falling back to
 * inline logging. This callback notifies the application of this state transition
 * so it can take corrective action (e.g., alert, initiate backpressure, reduce log rate).
 *
 * The callback is invoked from within the async queue path when the degradation occurs.
 * It is NOT called in an async-signal-safe context—avoid using non-async-safe functions
 * (like printf, malloc, etc.) inside the handler. Use it to set a flag or wake a worker.
 *
 * Registering a callback does not enable degradation itself; that is controlled by
 * configuration. See the async fallback behavior documentation in the config.
 *
 * @param[in] cb Callback function to invoke on async-to-sync degradation, or NULL to
 *               clear any previously registered callback.
 *
 * Note Thread-safe: registration is atomic. Only one callback can be active at a time per
 *       process-wide logger. The callback is invoked in the context of the thread that
 *       attempted to enqueue when the queue was full (the calling application thread,
 *       not the async worker).
 */
CLOGX_API void log_set_async_fallback_cb(void (*cb)(void));

/**
 * @brief Get the currently registered async fallback callback.
 *
 * Returns the callback function previously set by @ref log_set_async_fallback_cb, or NULL
 * if no callback is registered. This allows the application to inspect or replace the
 * current handler.
 *
 * @return Pointer to the callback function that will be invoked when async-to-sync degradation
 *         occurs, or NULL if no callback has been set. The returned pointer is valid until
 *         the next call to log_set_async_fallback_cb or log_destroy.
 *
 * Note Thread-safe: read of a single atomic variable. Safe to call concurrently with other
 *       operations. Does not invoke the callback; merely returns the stored function pointer.
 */
CLOGX_API void (*log_get_async_fallback_cb(void))(void);

/**
 * @brief Set the process-wide module name used for `%module` formatting token.
 *
 * The module name appears in every log record when formatted with the %module token,
 * providing a logical grouping identifier for log entries (e.g., "auth", "billing",
 * "webserver"). This is distinct from the source file/module field which reflects
 * the actual code location.
 *
 * Setting this affects all subsequently logged records on the current thread unless
 * overridden per-record via MDC (Mapped Diagnostic Context) thread-local context.
 * The default module name is "main" if not explicitly set.
 *
 * @param[in] module Module name string. If NULL or empty string, resets to the default
 *                   value "main". The string is copied internally; caller may free
 *                   after call.
 *
 * Note Thread-safe: uses a mutex protecting the global default value. Changing the
 *       module name does NOT affect already-queued log records—only those emitted
 *       after the change. Safe to call from any thread concurrently with logging.
 */
CLOGX_API void log_set_module(const char *module);

/**
 * @brief Copy the current module name into @p buf.
 *
 * Retrieves the module name that will be used for %module formatting in subsequent
 * log records. This is the value most recently set via log_set_module or the default
 * "main" if never set.
 *
 * @param[out] buf Destination buffer to receive the module name string (must be non-NULL).
 * @param[in] n    Capacity of @p buf in bytes. At most n-1 characters are written plus
 *                 terminating NUL. If n is 0 or buf is NULL, no action occurs.
 *
 * Note Thread-safe: reads under mutex protection. Caller must ensure buf is large enough
 *       to hold the module name (recommended CLOG_MAX_MODULE_SIZE or more). The returned
 *       string is null-terminated.
 */
CLOGX_API void log_get_module(char *buf, size_t n);

/**
 * @brief Append a custom sink to the process-wide logger after @ref log_init.
 *
 * Adds a sink (console, file, socket, custom plugin, etc.) to the singleton logger's
 * sink list. Sinks are iterated in addition order when writing each log record—every
 * enabled sink receives copies of all records that pass its level filter. The caller
 * transfers ownership of the sink object to the logger; it will be destroyed via the
 * sink's destroy callback during @ref log_destroy or subsequent reload.
 *
 * This function can only be called AFTER @ref log_init has succeeded. Attempting to
 * add sinks before initialization (or while a reload is in progress) returns an error.
 * Sinks added via this API are NOT persisted across configuration reloads—they are
 * discarded on @ref log_reload because they are not defined in the YAML config. For
 * persistent sinks that survive reload, define them in the plugins or built-in sink
 * sections of the configuration file instead.
 *
 * @param[in] sink Sink pointer created by one of the *_sink_create() factory functions.
 *                 Must not be NULL. Takes ownership upon successful addition.
 *
 * @return CLOG_OK on success, negative @ref clogx_errno_t on failure (e.g., invalid
 *         arguments, out-of-memory during sink list expansion, logger not initialized).
 *
 * Note Thread-safe: acquires internal mutex while modifying sink list. Concurrent log
 *       writes may continue safely—the snapshot approach ensures consistency. A newly
 *       added sink will receive records enqueued after the addition completes.
 */
CLOGX_API int log_add_sink(log_sink_t *sink);

/**
 * @brief Remove a previously added sink from the process-wide logger without destroying it.
 *
 * Removes the specified sink from the active sink list. After removal, new log records
 * will no longer be delivered to this sink. However, any records already queued or in
 * flight may still be processed by the sink before it is fully detached; those pending
 * writes are not cancelled. The sink object itself remains valid and owned by the caller,
 * who must eventually call sink->destroy to release its resources.
 *
 * This function only affects sinks added via @ref log_add_sink (custom dynamic sinks).
 * Sinks defined in the configuration file (built-in or plugin sinks) are removed during
 * reload according to the config and should not be manually removed with this API.
 *
 * @param[in] sink Sink pointer previously passed to @ref log_add_sink. Must be non-NULL
 *                 and present in the current sink list.
 *
 * @return CLOG_OK on success, CLOG_ERR_INVALID_ARG if sink is NULL or not found in the list.
 *
 * Note Thread-safe: acquires sink list mutex. Removal is immediate for new records; existing
 *       queued records referencing this sink may still be delivered. Caller must manage the
 *       sink lifetime independently—this function does not free the sink object.
 */
CLOGX_API int log_remove_sink(log_sink_t *sink);

/**
 * @brief Initialize the logging subsystem.
 *
 * Loads configuration from a YAML file (or programmatic defaults), creates
 * configured sinks (console, file, socket, etc.), initializes the formatter
 * (format string and time template), and starts the background async worker thread
 * if `async` is enabled in the configuration. This function must be called exactly
 * once before any other logging operations, and must be paired with exactly one
 * call to @ref log_destroy.
 *
 * The configuration loading process parses the YAML file at @p yaml_path (or
 * "./config.yaml" if path is NULL/empty) and sets up the internal logger state.
 * If the config file does not exist or is malformed, initialization fails and
 * an error code is returned. On success, the library enters an operational state
 * ready to accept log records.
 *
 * Thread Safety: This function is thread-safe during first-time initialization
 * via a mutex-guarded one-time activation pattern. Reentrant calls without an
 * intervening @ref log_destroy will fail with CLOG_ERR_INIT_REENTRANT.
 *
 * @param[in] yaml_path Path to a YAML configuration file. NULL or empty string
 *                      causes the library to default to "./config.yaml". If the
 *                      file is not found, fallback to built-in defaults occurs.
 *
 * @return CLOG_OK on success, negative @ref clogx_errno_t on failure. Error codes:
 *   - CLOG_ERR_CONFIG_OPEN: Failed to open config file
 *   - CLOG_ERR_CONFIG_PARSE: YAML/INI parsing failed
 *   - CLOG_ERR_NO_SINKS: No sinks were configured after load
 *   - CLOG_ERR_THREAD_CREATE: Failed to start async worker thread
 *   - CLOG_ERR_OOM: Memory allocation failure
 *
 * @note After a successful call, the global logger is initialized. Call @ref log_destroy
 *       when the application is done using logging. Re-initialization requires a prior
 *       destroy. The function is idempotent regarding sink creation—subsequent init
 *       calls without destroy return errors.
 */
CLOGX_API int log_init(const char *yaml_path);

/**
 * @brief Shut down the logging subsystem.
 *
 * Terminates the async background worker thread (waiting until any remaining
 * queued records are processed and flushed), calls flush on each active sink,
 * destroys all allocated sink objects, cleans up the formatter state, and
 * releases internal mutexes and condition variables. After this call, the
 * library is uninitialized and must be re-initialized via @ref log_init before
 * further use.
 *
 * This function is safe to call from any thread, including signal handlers.
 * It will block if the async worker is stuck processing a slow sink—in normal
 * operation it returns quickly once all pending work completes.
 *
 * @note Must be called exactly once after a successful @ref log_init. Calling
 *       without prior initialization is a no-op but may leave resources leaked.
 *       All sink objects created via factory functions are freed; custom sinks
 *       allocated by the application should not be passed to log_add_sink unless
 *       they were also created by clogx factories.
 */
CLOGX_API void log_destroy(void);

/**
 * @brief Flush all pending log output to sinks.
 *
 * Ensures that any log records queued for asynchronous processing are written
 * to their destinations and any buffered data in sinks is flushed (e.g., to
 * files or network sockets). This is important before program exit or when
 * guaranteeing log durability (e.g., before a checkpoint or crash).
 *
 * In synchronous mode, this simply calls flush on each sink. In asynchronous
 * mode, it blocks the caller thread until the background worker has dequeued
 * and processed all remaining records in the queue, then flushes each sink.
 *
 * @note Blocking: if the async worker is stalled (e.g., due to a slow sink),
 *       this call may block indefinitely. Use judiciously in critical paths.
 *       Thread-safe: can be called from any thread concurrently with logging.
 */
CLOGX_API void log_flush(void);

/**
 * @brief Install POSIX sigaction handlers for SIGTERM and SIGINT for graceful shutdown.
 *
 * Registers signal handlers using sigaction(2) for SIGTERM and SIGINT that defer
 * shutdown processing to the main event loop via a self-pipe trick. When these
 * signals are received, a flag is set and the main loop can flush logs and call
 * log_destroy before terminating, allowing the async worker to drain pending records.
 *
 * This implementation uses a pipe-based self-notification mechanism: the signal
 * handler writes to one end of a pipe, and the main loop reads from it to detect
 * pending signals. This avoids non-async-signal-safe operations inside the signal
 * handler itself.
 *
 * On platforms without POSIX signals (e.g., Windows), this function may return an
 * error or be a no-op. Applications should check the return value.
 *
 * @return CLOG_OK on success, negative error code if installation fails (e.g., pipe
 *         creation failed, sigaction failed, or platform unsupported).
 *
 * @note Thread-safe: installs handlers once. Idempotent—calling multiple times is safe.
 *       Must be called before logging begins if graceful shutdown is desired. Typically
 *       paired with log_signal_handler and log_process_pending_signals in the main loop.
 */
CLOGX_API clogx_errno_t log_install_signal_handlers(void);

/**
 * @brief Signal handler for graceful shutdown.
 *
 * This function is installed as a signal handler for SIGTERM and SIGINT via
 * @ref log_install_signal_handlers. It sets a global pending signal flag and
 * writes to a self-pipe descriptor to wake the main event loop. The main loop
 * then calls @ref log_process_pending_signals which will flush logs and potentially
 * re-raise the signal for default termination behavior after cleanup.
 *
 * The handler performs only async-signal-safe operations (setting a volatile flag
 * and writing to a pipe). It does NOT call logging functions or any non-async-signal-safe
 * routines directly.
 *
 * @param[in] sig Signal number received (should be SIGTERM or SIGINT).
 *
 * @note Must be async-signal-safe per POSIX signal handler requirements. Do not
 *       call this function directly—it is invoked by the kernel when signals are delivered.
 */
CLOGX_API void log_signal_handler(int sig);

/**
 * @brief Get the currently pending signal number, or 0 if none.
 *
 * Used by the main event loop to determine whether a shutdown signal was received
 * and should be processed (via @ref log_process_pending_signals). The flag is set
 * by the signal handler (@ref log_signal_handler) and cleared after processing.
 *
 * @return Pending signal number (e.g., SIGTERM, SIGINT), or 0 if no signal is pending.
 *
 * Note: Thread-safe read of volatile flag. Safe to call from any thread, including the
 *       main loop after detecting activity on the signal fd.
 */
CLOGX_API int log_get_pending_signal(void);

/**
 * @brief Get the read-end file descriptor of the self-pipe signal handler.
 *
 * Allows integration of clogx's signal handling into the application's event loop.
 * Reading from this fd indicates that a shutdown signal has been received and the
 * main loop should call @ref log_process_pending_signals to flush logs and potentially
 * re-raise the signal for graceful termination.
 *
 * The file descriptor is non-blocking and remains valid as long as signal handlers
 * are installed (via @ref log_install_signal_handlers). On platforms without POSIX
 * signals, this returns -1.
 *
 * @return Non-blocking read file descriptor, or -1 if signal handlers are not installed or
 *         on unsupported platform.
 *
 * Note Typically used with poll()/select()/epoll() to wait for signal events alongside
 *       other file descriptors. The fd is always ready for reading when a signal is pending;
 *       reading consumes the notification (writes once per signal arrival).
 */
CLOGX_API int log_get_signal_fd(void);

/**
 * @brief Process any pending signal: flush logs and optionally re-raise.
 *
 * Checks if a shutdown signal has been received (via the flag set by
 * @ref log_signal_handler). If so, this function flushes all pending log records
 * to ensure they are persisted before potentially re-raising the signal for
 * default termination behavior (SIGTERM/SIGINT). This enables graceful shutdown
 * where applications can drain the async queue before exiting.
 *
 * Typically called in the main event loop after detecting activity on the
 * signal fd returned by @ref log_get_signal_fd. After calling this, if a signal
 * was processed, the application should normally terminate or call _exit().
 *
 * @note Thread-safe: should be called in the main thread after receiving signal
 *       notification via self-pipe fd. Does not block; performs immediate flush
 *       and signal re-raising as appropriate.
 */
CLOGX_API void log_process_pending_signals(void);

/**
 * @struct log_stats_t
 * @brief Operational runtime statistics.
 */
typedef struct {
    uint64_t total_logged_count;       /**< Total log records submitted. */
    uint64_t dropped_queue_full_count; /**< Total records dropped due to async queue full. */
    uint64_t suppressed_rate_count;    /**< Total records suppressed by rate limiter. */
    size_t current_queue_depth;        /**< Current pending records in async queue. */
} log_stats_t;

/**
 * @brief Retrieve current operational statistics.
 *
 * Populates the provided structure with counters reflecting the current state
 * of the logging subsystem. Useful for monitoring and debugging log throughput,
 * backpressure conditions, and rate limiter behavior.
 *
 * The statistics include:
 *   - Total number of log records submitted since initialization.
 *   - Records dropped because the async queue was full (backpressure).
 *   - Records suppressed by the global rate limiter.
 *   - Current pending record count in the async queue (0 in sync mode).
 *
 * @param[out] stats Pointer to log_stats_t struct to populate (must be non-NULL).
 *
 * @note Thread-safe: reads atomically updated counters. Safe to call concurrently
 *       from any thread without additional synchronization.
 */
CLOGX_API void log_get_stats(log_stats_t *stats);

/**
 * @brief Set a thread-local context key-value pair for structured/formatted logs.
 *
 * Stores a key-value pair in the current thread's Mapped Diagnostic Context (MDC).
 * These values are automatically included in log records when using the %context
 * token or when emitting JSON format. Useful for propagating correlation IDs,
 * tenant IDs, user IDs, or other request-scoped data across log lines within a thread.
 *
 * The MDC is per-thread; each thread maintains its own independent map. Keys and
 * values are limited to maximum lengths defined in log_limits.h. Setting a key
 * overwrites any existing value for that key in the current thread. Passing an empty
 * key or NULL value deletes the entry.
 *
 * @param[in] key Context key name (null-terminated). Pass NULL or empty string to delete the key.
 * @param[in] value Context value string. Pass NULL to delete the key.
 *
 * @return CLOG_OK on success, CLOG_ERR_INVALID_ARG if key is too long, value too long, or other
 * errors.
 *
 * Note Thread-specific: affects only the calling thread's MDC map. Safe to call concurrently
 *       from different threads (each operates on its own storage). No allocation—uses pre-allocated
 * buffers.
 */
CLOGX_API clogx_errno_t log_set_thread_context(const char *key, const char *value);

/**
 * @brief Get a thread-local context value for the given key.
 *
 * Retrieves the value associated with @p key from the calling thread's MDC (Mapped Diagnostic
 * Context). The returned pointer remains valid until the key is overwritten or the thread exits. If
 * the key is not set, returns NULL.
 *
 * This is the counterpart to @ref log_set_thread_context and enables reading contextual data that
 * was set earlier in the same thread (e.g., a correlation ID set at request start).
 *
 * @param[in] key Context key name to look up.
 *
 * @return Pointer to the value string if found, or NULL if the key is not set in the current
 * thread.
 *
 * Note Thread-safe per-thread lookup: no locking needed since each thread accesses its own storage.
 *       Does not allocate memory; returns reference to existing buffer. Caller must not free or
 * modify the returned string.
 */
CLOGX_API const char *log_get_thread_context(const char *key);

/**
 * @brief Clear all thread-local context key-value pairs for the calling thread.
 *
 * Removes every entry from the current thread's MDC map. After this call, subsequent log records
 * generated in this thread will not include any contextual key-value data until new values are set
 * via @ref log_set_thread_context.
 *
 * Useful at request boundaries or thread cleanup points to avoid cross-request contamination of
 * contextual information (e.g., correlation IDs).
 *
 * @note Thread-specific: only affects the calling thread's MDC map; no effect on other threads.
 */
CLOGX_API void log_clear_thread_context(void);

/**
 * @brief Set thread-local W3C TraceContext trace ID (16 bytes) and span ID (8 bytes).
 *
 * Propagates distributed tracing identifiers through log records for end-to-end tracing
 * across services. When non-zero, these values appear in JSON/OTEL formatted logs as
 * `trace_id` and `span_id` fields, enabling correlation with distributed tracing systems
 * such as Jaeger, Zipkin, or OpenTelemetry collectors.
 *
 * Trace ID follows RFC 7906 (16 bytes hexadecimal), span ID follows RFC 7907 (8 bytes hexadecimal).
 * Zero arrays indicate no active trace/span context; see the zero-id detection helper
 * in `formatter.c` (`is_zero_id`).
 *
 * This API sets raw binary trace/span IDs directly. For string-based propagation (e.g.,
 * from HTTP `traceparent` headers), use @ref clog_set_trace_context_hex instead.
 *
 * @param[in] trace_id 16-byte raw trace ID array. Pass NULL to clear/disable.
 * @param[in] span_id  8-byte raw span ID array. Pass NULL to clear/disable.
 *
 * Note Thread-local: only affects subsequent log records generated in the calling thread.
 *       No validation is performed on the ID contents—any bit pattern is accepted.
 */
CLOGX_API void clog_set_trace_context(const uint8_t trace_id[16], const uint8_t span_id[8]);

/**
 * @brief Get thread-local W3C TraceContext trace ID and span ID.
 *
 * Retrieves the currently stored trace and span identifiers for the calling thread. If no
 * trace context has been set (i.e., both arrays are all zeros), output buffers are filled
 * with zeros to indicate absence of context. This allows callers to check whether tracing
 * is active by examining the returned values.
 *
 * The retrieved IDs match those previously set via @ref clog_set_trace_context or
 * @ref clog_set_trace_context_hex, or parsed from the TRACEPARENT environment variable in
 * the formatter's W3C TraceContext parser.
 *
 * @param[out] trace_id 16-byte buffer populated with current trace ID (zeroed if unset).
 *                  Buffer must be at least 16 bytes in size.
 * @param[out] span_id  8-byte buffer populated with current span ID (zeroed if unset).
 *                 Buffer must be at least 8 bytes in size.
 *
 * Note Both buffers are always zeroed on exit, even when no prior context was set. This
 *       ensures a clean state and avoids uninitialized memory exposure. Safe to call
 *       concurrently from multiple threads (each accesses its own thread-local storage).
 */
CLOGX_API void clog_get_trace_context(uint8_t trace_id[16], uint8_t span_id[8]);

/**
 * @brief Clear thread-local W3C TraceContext for the calling thread.
 *
 * Sets the thread-local trace ID and span ID arrays to zero, effectively removing any
 * active tracing context from subsequent log records generated in this thread. This is
 * useful at request boundaries when a new distributed trace begins, or when exiting
 * a traced region to avoid contaminating unrelated logs.
 *
 * After this call, @ref clog_get_trace_context will report zeroed arrays until a new
 * context is set via @ref clog_set_trace_context or parsed from environment variables.
 *
 * Note Thread-specific: only affects the calling thread's trace context. No side effects
 *       on other threads or global state. Safe to call concurrently.
 */
CLOGX_API void clog_clear_trace_context(void);

/**
 * @brief Set thread-local W3C TraceContext using hexadecimal strings.
 *
 * Parses 32-character hex trace ID and 16-character hex span ID strings, converts them to binary,
 * and stores them thread-locally via @ref clog_set_trace_context after validation. This is
 * convenient for string-based propagation of tracing context from HTTP headers (e.g., `traceparent`
 * header) or environment variables.
 *
 * The input strings must contain exactly 32 hex digits for trace ID and 16 hex digits for span ID.
 * Whitespace is not allowed; format must be continuous hexadecimal characters (0-9, a-f, A-F). If
 * either argument is NULL or empty, the corresponding context field is cleared (zeroed).
 *
 * @param[in] trace_id_hex 32-character hex string representing 16-byte trace ID, or NULL/empty to
 * clear. Format: exactly 32 hex digits (e.g., "4f9a8b3c1d2e3f4a5b6c7d8e9f0a1b2c").
 * @param[in] span_id_hex  16-character hex string representing 8-byte span ID, or NULL/empty to
 * clear. Format: exactly 16 hex digits (e.g., "1a2b3c4d5e6f7a8b").
 *
 * @return CLOG_OK on success, CLOG_ERR_INVALID_ARG if hex string is malformed (wrong length or
 * non-hex chars).
 *
 * Note Underlying storage matches binary format set by @ref clog_set_trace_context; both functions
 * are interchangeable. Parsing is strict - any deviation results in failure and no state change.
 */
CLOGX_API clogx_errno_t clog_set_trace_context_hex(const char *trace_id_hex,
                                                   const char *span_id_hex);

/* The Plugin ABI API (log_plugin_load, log_plugin_unload, log_plugin_create_sink,
 * log_plugin_info, log_plugin_scan) is declared in <clogx_plugin.h> included above. */

/**
 * @brief Reload configuration and rebuild sinks.
 *
 * Re-reads the configuration file remembered from the last @ref log_init or
 * @ref log_config_set call, parses it, and atomically replaces the active
 * configuration. This enables hot-reloading of logging settings without stopping
 * the application. The operation follows these steps:
 *
 *  1. Shut down the async worker (if enabled), draining pending records.
 *  2. Parse the configuration file; fail if parse fails.
 *  3. Build a snapshot of the new sink list based on config.
 *  4. Atomically replace the current sink list with the new snapshot.
 *  5. Re-initialize rate limiter per new config.
 *  6. If async mode is enabled in the new config, restart the async worker.
 *
 * Sinks that were added via @ref log_add_sink (custom sinks not defined in YAML)
 * are discarded during reload—they must be re-added by the application after
 * successful reload. Plugin sinks listed in the config are recreated according
 * to the new plugin configuration.
 *
 * @return CLOG_OK on success, negative @ref clogx_errno_t on failure (e.g., config parse error,
 *         failed to open file, no sinks configured, thread creation failure). Use @ref log_strerror
 *         to obtain a human-readable description of the error.
 *
 * Note Thread-safe: the reload operation is serialized internally; concurrent calls will queue.
 *       However, it's recommended to avoid calling reload while actively logging to prevent
 *       transient inconsistencies. The function does not block long-term—async worker shutdown
 *       may wait briefly for queued records to flush.
 */
CLOGX_API int log_reload(void);

/**
 * @brief Format a message and dispatch a log record.
 *
 * This low-level function is invoked by the @c LOG_* macro family (e.g., LOG_INFO, LOG_DEBUG).
 * It constructs a @ref log_record_t on the caller's stack, formats the message using
 * printf-style formatting with compile-time format string checking (when supported by the
 * compiler via CLOGX_PRINTF_FMT), and then either:
 *
 * - In synchronous mode: directly dispatches the formatted record through the sink dispatcher.
 * - In asynchronous mode: deep-copies the record's string fields, copies the entire record
 *   into the async queue, and returns immediately while the background worker handles
 *   eventual dispatch.
 *
 * This function performs formatting (expensive string operations) on the caller's thread
 * even in async mode; only the enqueueing/decoupling happens asynchronously. For high-
 * volume logging, consider this cost when choosing between sync vs async mode.
 *
 * @param[in] level Log severity (LOG_LEVEL_TRACE .. LOG_LEVEL_FATAL).
 * @param[in] file Source file name (typically from LOG_FILENAME_ONLY(), which expands
 *                 to the basename of __FILE__).
 * @param[in] line Source line number (__LINE__).
 * @param[in] func Source function name (__func__ or __FUNCTION__).
 * @param[in] fmt printf-style format string (never NULL). The first argument after fmt
 *                is interpreted as the format string and validated for argument count/type
 *                when built with GCC/Clang (CLOGX_PRINTF_FMT attribute ensures safety).
 * @param[in] ... Format arguments matching @p fmt. Must be present and compatible with
 *                the format specifiers. Unsafe usage (mismatched types) invokes undefined behavior.
 *
 * Note Thread-safe: this function may be called concurrently from any thread. In async mode,
 *       it acquires the queue mutex to enqueue; the caller should ensure that the formatted
 *       message does not depend on temporaries that could be invalidated before enqueue completes
 *       (since the record is copied). The LOG_* macros handle this automatically by passing
 *       stack-allocated strings that remain valid during the function call.
 */
CLOGX_API void log_writevprintf(log_level_t level, const char *file, int line, const char *func,
                                const char *fmt, ...) CLOGX_PRINTF_FMT(5, 6);

/**
 * @brief Extract the basename portion of a file path.
 *
 * Given a full path string (e.g., "/home/user/src/main.c"), returns a pointer
 * to the last component of the path ("main.c"). Handles both Unix-style (/) and
 * Windows-style (\) separators. If path is NULL or empty, returns an empty string.
 *
 * This function is used internally by the LOG_FILENAME_ONLY() macro to provide
 * concise file names in log output without directory clutter.
 *
 * @param[in] path File path string (can be @c __FILE__ directly).
 *
 * @return Pointer to the basename substring within the input string (or to an
 *         empty static string if input is NULL/empty). The returned pointer points
 *         into the input buffer and is valid only as long as the input string
 *         remains alive and unmodified.
 */
static inline const char *clogx_filename_only(const char *path) {
    const char *slash;
    const char *base = path ? path : "";
    slash = strrchr(base, '/');
    if (slash)
        base = slash + 1;
    slash = strrchr(base, '\\');
    if (slash)
        base = slash + 1;
    return base;
}

#define LOG_FILENAME_ONLY() clogx_filename_only(__FILE__)

/**
 * @def LOG_FILENAME_ONLY
 * @brief Convenience wrapper around @c clogx_filename_only() using @c __FILE__.
 *
 * Expands to a call to clogx_filename_only(__FILE__), producing just the filename
 * portion of the source file location where the LOG macro is invoked. This keeps
 * log output tidy while preserving useful source location information.
 *
 * Typical usage: inside the LOG_* macro definitions, injected as the 'file' parameter
 * to log_writevprintf.
 */

#define LOG_FILENAME_ONLY() clogx_filename_only(__FILE__)

/**
 * @def LOG_FILENAME_ONLY
 * @brief Convenience wrapper around @c clogx_filename_only() using @c __FILE__.
 */

/**
 * @def LOG_INFO(msg, ...)
 * @brief Log an informational message at INFO severity level.
 *
 * Records a log entry with severity LOG_LEVEL_INFO, including source file, line number,
 * function name, thread ID, process ID, module name, and the formatted message. The
 * message is formatted using printf-style formatting with compile-time argument checking
 * when built with GCC/Clang.
 *
 * Example:
 * @code
 *   LOG_INFO("Server started on port %d", port);
 *   LOG_USER("User %s logged in", username);
 * @endcode
 *
 * This macro expands to a call to log_writevprintf with appropriate parameters. It is the
 * primary logging interface for normal operational messages, such as service startup,
 * configuration loaded, successful requests, etc.
 *
 * @param[in] msg Format string followed by zero or more arguments. The format string is
 *                treated as a literal and validated against subsequent arguments when
 *                supported by the compiler (via CLOGX_PRINTF_FMT attribute on the underlying
 *                function). Mismatched arguments cause undefined behavior at runtime.
 *
 * Note Messages are filtered according to the current minimum log level set via
 *       log_set_level or logger_set_level. If the level is below threshold, the macro
 *       evaluates quickly without constructing the format string (pre-check via
 *       log_should_emit). Therefore, expensive function calls in arguments should be guarded
 *       if they would be unnecessary when the level is higher.
 */
/**
 * @def LOG_DEBUG(msg, ...)
 * @brief Log a debug message at DEBUG severity level.
 *
 * Similar to LOG_INFO but with severity LOG_LEVEL_DEBUG. Typically used for diagnostic
 * information that is helpful during development and troubleshooting but may be too
 * verbose for normal operation. In production, these messages are often filtered out by
 * setting a higher minimum log level (e.g., INFO or WARN).
 *
 * Debug messages commonly include variable values, function entry/exit traces, condition
 * checks, and other detailed state information that aids in understanding program flow.
 *
 * @param[in] msg Format string with arguments, same as LOG_INFO. See notes about format
 *                safety and filtering behavior under LOG_INFO.
 *
 * Note Use judiciously in performance-critical paths because formatting still occurs even
 *       when the message is later filtered (the pre-check in log_writevprintf avoids some
 *       work, but the arguments are still evaluated by the C compiler). Guard expensive
 *       calls with conditional checks if needed.
 */
/**
 * @def LOG_WARN(msg, ...)
 * @brief Log a warning message at WARN severity level.
 *
 * Indicates unusual or potentially problematic conditions that are not errors per se but
 * warrant attention. Warnings are typically retained in production logs as they can
 * indicate incipient failures, configuration issues, or unexpected inputs that the system
 * handled gracefully.
 *
 * Examples include: approaching resource limits, deprecated feature usage, transient
 * network retries, failed fallback attempts, etc.
 *
 * @param[in] msg Format string with arguments, same as other LOG macros.
 *
 * Note Warnings should be actionable and informative—not too generic (like "something happened")
 *       but specific enough to guide investigation. Include relevant values or context when
 * possible.
 */
/**
 * @def LOG_ERROR(msg, ...)
 * @brief Log an error message at ERROR severity level.
 *
 * Signifies failures that affect a specific request, operation, or component. Errors should
 * be investigated promptly and are typically included in log aggregates, alerting systems,
 * and monitoring dashboards. Unlike WARN, errors indicate something went wrong that may
 * require intervention or at least awareness.
 *
 * Examples: database connection failed, authentication rejected, file not found (when unexpected),
 * network timeout, validation error, etc.
 *
 * @param[in] msg Format string with arguments. Should include enough detail to understand the
 *                cause of the error (e.g., error codes, affected identifiers, retry counts).
 *
 * Note Errors are always logged regardless of the minimum log level (they are above INFO by
 * default). Use structured logging (JSON format) to enable automated parsing and alerting on error
 * patterns.
 */
/**
 * @def LOG_FATAL(msg, ...)
 * @brief Log a fatal message at FATAL severity level.
 *
 * Indicates a severe failure that may render the process unusable or require immediate
 * termination. After logging a FATAL message, the application should typically call abort()
 * or exit(1) as soon as possible to prevent running in an inconsistent state. The log record
 * is written regardless of the current log level or async queue status—FATAL messages are
 * always emitted and flushed.
 *
 * Examples: critical assertion failed, corrupted persistent data detected, unrecoverable
 * system error (e.g., out of memory in a critical section), internal invariant violation.
 *
 * @param[in] msg Format string describing the fatal condition. Should be concise but include
 *                enough context for post-mortem analysis (e.g., version, build ID, last operation).
 *
 * Note Use sparingly—only for truly catastrophic failures. Prefer ERROR for recoverable errors.
 *       FATAL logs often trigger automatic alerting and incident response workflows.
 */
/**
 * @def LOG_TRACE(msg, ...)
 * @brief Log a trace message at TRACE severity level.
 *
 * Finest-grained diagnostic output, typically used for detailed flow tracing, entry/exit
 * of functions, loop iterations, and other high-volume debug information. Trace messages
 * are often disabled in production due to their volume; they are most useful during
 * development and troubleshooting when set to the lowest log level (TRACE).
 *
 * Examples: function entry with parameters, variable state changes, branch decisions,
 * cache hits/misses, network packet details.
 *
 * @param[in] msg Format string with arguments. Due to high volume, keep trace messages
 *                concise and avoid expensive formatting unless necessary.
 *
 * Note Because trace logs can be extremely numerous, consider using pre-check guards like
 *       if (log_should_emit(LOG_LEVEL_TRACE)) LOG_TRACE(...) around very expensive
 *       string constructions. The macro itself does not evaluate the format string if the
 *       current level is higher than TRACE (due to log_should_emit check inside), so simple
 *       calls are safe even in hot paths.
 */
#define LOG_INFO(...)                                                                              \
    log_writevprintf(LOG_LEVEL_INFO, LOG_FILENAME_ONLY(), __LINE__, __func__, __VA_ARGS__)
#define LOG_DEBUG(...)                                                                             \
    log_writevprintf(LOG_LEVEL_DEBUG, LOG_FILENAME_ONLY(), __LINE__, __func__, __VA_ARGS__)
#define LOG_WARN(...)                                                                              \
    log_writevprintf(LOG_LEVEL_WARN, LOG_FILENAME_ONLY(), __LINE__, __func__, __VA_ARGS__)
#define LOG_ERROR(...)                                                                             \
    log_writevprintf(LOG_LEVEL_ERROR, LOG_FILENAME_ONLY(), __LINE__, __func__, __VA_ARGS__)
#define LOG_FATAL(...)                                                                             \
    log_writevprintf(LOG_LEVEL_FATAL, LOG_FILENAME_ONLY(), __LINE__, __func__, __VA_ARGS__)
#define LOG_TRACE(...)                                                                             \
    log_writevprintf(LOG_LEVEL_TRACE, LOG_FILENAME_ONLY(), __LINE__, __func__, __VA_ARGS__)

/* ════════════════════════════════════════════════════════════════════════
 *  Multi-instance API (Phase 2)
 * ════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Opaque logger instance handle.
 *
 * Create independent loggers with @ref logger_create or
 * @ref logger_create_from_config. Each instance has its own config, sinks,
 * async worker, rate limiter, formatter, and module name.
 */
typedef struct logger_t logger_t;

/* Instance creation / destruction */

/**
 * @brief Create a new logger instance from a YAML config file.
 *
 * Loads configuration from the specified YAML file, initializes the formatter,
 * allocates configured sinks (console, file, socket, plugins), and starts an
 * async worker thread if async is enabled in the config. Returns a handle that
 * must be used with other logger_* functions; it becomes invalid after
 * @ref logger_destroy is called.
 *
 * The created logger is independent of the process-wide singleton logger (@ref
 * log_init). Multiple loggers can coexist within the same process, each with its
 * own configuration, sink list, async queue, rate limiter, and module name. This
 * enables per-component or per-service logging configurations.
 *
 * If @p yaml_path is NULL or empty string, the logger uses a default in-memory
 * configuration (equivalent to calling @ref logger_create_from_config with a
 * default-configured log_config_t). To specify a non-default path, provide the
 * full or relative path to a YAML config file; if the file does not exist,
 * initialization fails.
 *
 * @param[in] yaml_path Path to configuration file. NULL or "" uses built-in defaults.
 *
 * @return Pointer to a newly allocated logger instance, or NULL on failure (memory
 *         allocation error, config parse error, sink creation failure). Use
 *         @ref log_strerror to obtain the error description.
 *
 * Note The caller owns the returned pointer and MUST call @ref logger_destroy when
 *       finished. The first call to create any logger also initializes the global
 *       singleton logger; subsequent loggers are independent but share some
 *       global resources (mutexes, RNG state, etc.).
 */
CLOGX_API logger_t *logger_create(const char *yaml_path);

/**
 * @brief Create a logger from an in-memory configuration structure.
 *
 * Useful when the desired configuration is computed programmatically rather than
 * loaded from a file. All fields from @p cfg are copied into the new logger,
 * including format strings, time template, sink specifications, rate limiting,
 * etc. The logger behaves identically to one created from a YAML file, except
 * that hot-reload by re-parsing the same file is not possible (since no path is
 * remembered).
 *
 * @param[in] cfg Pointer to a fully populated log_config_t struct. Must not be NULL.
 *
 * @return New logger instance, or NULL on memory allocation failure or if
 *         configuration is invalid (e.g., negative queue_size, unsupported level).
 *
 * Note The caller retains ownership of @p cfg; it may be freed immediately after
 *       this call returns because the logger makes its own deep copies of all
 *       string fields and structs. The returned logger must eventually be freed
 *       via @ref logger_destroy.
 */
CLOGX_API logger_t *logger_create_from_config(const log_config_t *cfg);

/**
 * @brief Destroy a logger instance and release all associated resources.
 *
 * Shuts down the async worker (draining any remaining queued records), flushes
 * pending logs to sinks, destroys all sink objects associated with this logger,
 * frees internal data structures (formatter config, rate limiter, etc.), and
 * releases locks. After this call, the logger handle becomes invalid and must
 * not be used in any further calls.
 *
 * @param[in] logger Pointer to the logger instance to destroy. Passing NULL is a
 *                   safe no-op.
 *
 * Note Thread-safe: concurrent logging attempts from other threads against this
 *       logger during/after destroy will be safely handled (records dropped).
 *       The caller is responsible for ensuring no other threads are using the
 *       logger when destroying it, or must synchronize externally.
 */
CLOGX_API void logger_destroy(logger_t *logger);

/* Instance-level operations */
/**
 * @brief Format a message and dispatch to the specified logger instance.
 *
 * Builds a log record on the stack, formats it according to this logger's
 * configured format string and time template, then either enqueues it to the
 * async queue (if this logger has async enabled) or dispatches it synchronously
 * to its sink list. Source location information (file, line, func) is injected
 * automatically by the caller via the LOG_FILENAME_ONLY() macro expansion.
 *
 * This function is the implementation backing the LOGGER_* macros and is also
 * available for direct programmatic calls when a specific logger (other than the
 * global singleton) should be used.
 *
 * @param[in] logger Target logger instance (must be non-NULL and valid).
 * @param[in] level Log severity (LOG_LEVEL_TRACE .. LOG_LEVEL_FATAL).
 * @param[in] file Source file name (typically from LOG_FILENAME_ONLY()).
 * @param[in] line Source line number (__LINE__).
 * @param[in] func Source function name (__func__).
 * @param[in] fmt printf-style format string (never NULL). Uses compile-time format
 *       checking when built with GCC/Clang (CLOGX_PRINTF_FMT attribute ensures argument
 *       count/type safety matching the format string).
 * @param[in] ... Format arguments matching @p fmt. Mismatched types invoke undefined
 *       behavior; ensure format specifiers correspond correctly to argument types.
 *
 * Note In async mode, formatting occurs on the caller thread; only the enqueueing is
 *       deferred. The record's string fields are deep-copied before insertion into the
 *       queue, so the caller's stack variables remain valid after return. The
 *       CLOGX_PRINTF_FMT attribute provides compile-time validation of variadic
 *       arguments against the format string when using GCC/Clang.
 */
CLOGX_API void logger_writevprintf(logger_t *logger, log_level_t level, const char *file, int line,
                                   const char *func, const char *fmt, ...) CLOGX_PRINTF_FMT(6, 7);
/**
 * @brief Flush pending logs for this logger instance.
 *
 * For async loggers, this blocks until all queued records have been dequeued and
 * flushed to sinks (i.e., the background worker has processed everything). For
 * sync loggers, it merely calls flush on each sink without waiting for background
 * work (since there is none). This ensures that all previously submitted log
 * records are persisted before returning.
 *
 * Useful at shutdown points or before critical operations where log durability is
 * required. However, because it can block indefinitely if the async worker is
 * stalled (e.g., due to a slow sink or blocked network), use judiciously in
 * performance-critical paths.
 *
 * @param[in] logger Target logger instance. If NULL, this function is a no-op.
 *
 * Note Thread-safe: safe to call concurrently; the caller will block until this
 *       logger's queue is drained and sinks flushed. Does not affect other loggers.
 */
CLOGX_API void logger_flush(logger_t *logger);
/**
 * @brief Reload the logger's configuration and rebuild its sink list.
 *
 * Similar to @ref log_reload but operates on a specific logger instance. Shuts down
 * the current async worker (draining any pending records), re-reads the config file
 * originally used to create this logger (or the config remembered from last set),
 * recreates all sinks according to the new configuration, and restarts the async
 * worker if enabled in the new config. Existing pending records are dropped during
 * shutdown; only records submitted after reload begin using the new settings.
 *
 * This enables dynamic configuration changes (e.g., changing log level or adding/removing
 * sinks) without restarting the application. The old sink objects are freed; new ones
 * are allocated according to the updated config. Plugin sinks are reloaded as specified.
 *
 * @param[in] logger Target logger instance (must be non-NULL and initialized).
 *
 * @return CLOG_OK on success, negative clogx_errno_t on failure (file parse error,
 *         sink creation failure, async thread start failure, etc.). Use @ref log_strerror
 *         for detailed error message.
 *
 * Note Hot-reload is designed for scenarios where configuration is changed externally
 *       (e.g., via SIGHUP signal handler). The caller must ensure that concurrent logging
 *       does not interfere; the implementation uses snapshotting internally to maintain
 *       consistency during the transition. Any custom sinks added via direct pointer
 *       manipulation not present in the config will be lost after reload.
 */
CLOGX_API int logger_reload(logger_t *logger);
/**
 * @brief Add a sink to the logger instance.
 *
 * Adds a sink (console, file, socket, custom plugin, etc.) to this logger's sink list.
 * Sinks are iterated in order when writing records—each record gets passed to all
 * enabled sinks that accept its severity level. The caller transfers ownership of the
 * sink to the logger; the logger will destroy it via its destroy callback when the
 * sink is removed or the logger is destroyed.
 *
 * @param[in] logger Target logger instance (must be non-NULL and initialized).
 * @param[in] sink Sink to add, created by one of the *_sink_create() factory functions.
 *                 Must not be NULL. Takes ownership upon successful addition.
 *
 * @return CLOG_OK on success, CLOG_ERR_OOM if allocation fails during snapshotting,
 *         CLOG_ERR_INVALID_ARG if arguments are invalid, or other negative error codes.
 *
 * Note Thread-safe: adds under logger lock. Cannot modify sinks concurrently while logging;
 *       the implementation uses a snapshot approach to maintain consistency. Custom sinks
 *       added directly without going through config may be lost on subsequent reloads.
 */
CLOGX_API int logger_add_sink(logger_t *logger, log_sink_t *sink);

/**
 * @brief Remove a sink from the logger instance.
 *
 * Removes the specified sink from the logger's active sink list. After removal, new log
 * records will no longer be sent to this sink. However, any records already queued for
 * this sink may still be processed before the sink is fully detached. The sink object
 * itself remains valid and owned by the caller; the caller must call sink->destroy when
 * finished using it.
 *
 * @param[in] logger Target logger instance (must be non-NULL).
 * @param[in] sink Sink to remove; must have been previously added via logger_add_sink.
 *
 * @return CLOG_OK on success, CLOG_ERR_INVALID_ARG if sink is NULL or not found in the list.
 *
 * Note The caller retains ownership and responsibility for freeing the sink. Removal does
 *       immediately affect queued records—they may still be delivered to the sink until
 *       processing completes. Safe to call concurrently with logging under internal locking.
 */
CLOGX_API int logger_remove_sink(logger_t *logger, log_sink_t *sink);
/**
 * @brief Set the minimum emit level for this logger instance.
 *
 * Messages below the configured severity level are dropped before reaching any sink.
 * This affects all subsequent log records until changed. Useful for adjusting logging
 * verbosity dynamically without recreating the logger.
 *
 * @param[in] logger Target logger instance (must be non-NULL).
 * @param[in] level New minimum severity level (LOG_LEVEL_TRACE .. LOG_LEVEL_FATAL).
 *
 * @return 0 on success, -1 if logger is NULL or level is invalid.
 *
 * Note Thread-safe: writes under instance mutex immediately visible to subsequent calls.
 *       Does not affect records already queued; only filters newly submitted messages.
 */
CLOGX_API int logger_set_level(logger_t *logger, log_level_t level);

/**
 * @brief Get the current minimum emit level for this logger instance.
 *
 * Returns the configured threshold below which log messages are discarded. If logger is
 * NULL, returns the global default (LOG_LEVEL_INFO). This function provides a fast path
 * to query the active filter level without touching global state.
 *
 * @param[in] logger Target logger instance (may be NULL to retrieve global default).
 *
 * @return Current log_level_t setting (min severity to emit), or LOG_LEVEL_INFO if logger is NULL.
 *
 * Note Lightweight read operation; typically lock-free or uses read-side synchronization.
 *       Safe to call from any thread, including hot paths where checking log_should_emit
 *       would use this value internally.
 */
CLOGX_API log_level_t logger_get_level(const logger_t *logger);
/**
 * @brief Set the module name for this logger instance.
 *
 * Overrides the process-wide default module name for this logger's records. The module
 * name appears in log output when using the %module formatting token and in JSON logs
 * as the "module" field. Useful to distinguish logs from different components or services
 * within the same process.
 *
 * @param[in] logger Target logger instance (must be non-NULL).
 * @param[in] module Module name string. NULL or empty string resets to "main" (default).
 *
 * Note Thread-safe: sets instance-specific field under mutex immediately visible to subsequent
 *       log calls. Does not affect already-queued records; only impacts newly submitted ones.
 */
CLOGX_API void logger_set_module(logger_t *logger, const char *module);

/**
 * @brief Copy the current module name into @p buffer.
 *
 * Retrieves the module name assigned to this logger (set via logger_set_module or inherited
 * at creation if not explicitly set). If logger is NULL, buffer is NULL, or n is 0, the call
 * is a no-op. Otherwise, writes up to n-1 characters plus null terminator.
 *
 * @param[in] logger Target logger instance (may be NULL for fallback).
 * @param[out] buf Destination buffer to receive the module name (must be non-NULL).
 * @param[in] n Size of destination buffer in bytes.
 *
 * Note Safe for concurrent calls; reads under lock. Caller must ensure buffer is large enough
 *       (typically CLOG_MAX_MODULE_SIZE or more, but not guaranteed—call with sufficient size).
 */
CLOGX_API void logger_get_module(const logger_t *logger, char *buf, size_t n);

/**
 * @brief Retrieve runtime statistics for this logger instance.
 *
 * Populates the provided structure with counters specific to this logger's queue and activity:
 * total logged count, dropped records due to queue full, suppressed by rate limiter, and
 * current queue depth. Useful for monitoring backpressure and health of the logging system.
 *
 * @param[in] logger Target logger instance (must be non-NULL).
 * @param[out] stats Pointer to log_stats_t to fill (must be non-NULL).
 *
 * Note Thread-safe: reads atomically updated counters; safe to call concurrently from any thread.
 *       Does not block; returns current snapshot. Values are approximate if updates occur during
 * read.
 */
CLOGX_API void logger_get_stats(const logger_t *logger, log_stats_t *stats);

/**
 * @brief Apply a new configuration to this logger instance.
 *
 * Copies the provided config structure into the logger, updating format strings, time template,
 * rate limiter settings, minimum level, async flag, etc. Existing sink list is NOT replaced—
 * sinks must be modified separately via logger_add_sink/logger_remove_sink. This function is
 * useful for programmatic adjustments without triggering a full reload (which would drop queued
 * records and recreate sinks).
 *
 * @param[in] logger Target logger instance (must be non-NULL).
 * @param[in] cfg New configuration (must be non-NULL). String fields are deep-copied internally.
 *
 * @return 0 on success, -1 on failure (e.g., NULL logger, invalid config values).
 *
 * Note Deep copies format/time_format strings into internal buffers; caller may free cfg
 * immediately. Does not restart async worker—if you need to change queue size or async setting, use
 *       logger_reload instead. Thread-safe update under instance lock.
 */
CLOGX_API int logger_config_set(logger_t *logger, const log_config_t *cfg);

/**
 * @brief Retrieve the current configuration of this logger instance.
 *
 * Returns a pointer to the logger's active configuration structure. The returned pointer is
 * valid as long as the logger exists and should not be freed or modified. String fields like
 * format and time_format point into internal storage managed by the logger.
 *
 * If logger is NULL, the function returns a pointer to the global singleton configuration
 * (the one used by log_init/unix-style API), which may be different from per-instance configs.
 *
 * @param[in] logger Target logger instance (NULL returns global config).
 *
 * @return Pointer to the logger's config structure. Valid until logger_destroy is called.
 *
 * Note The caller should treat the returned struct as read-only; modify via logger_config_set
 *       instead. For a mutable copy, duplicate the struct manually. Thread-safe read; no lock
 *       held during access (config is immutable after set/initialization).
 */
CLOGX_API log_config_t *logger_config_get(const logger_t *logger);

/**
 * @name LOGGER_* macros
 * @brief Instance-level logging macros, analogous to @c LOG_* but operate on a specific logger
 * instance.
 *
 * These macros provide the same functionality as the global LOG_* macros but log to a specific
 * logger created with @ref logger_create or @ref logger_create_from_config. They allow applications
 * to maintain multiple independent logging contexts within the same process, each with its own
 * configuration, sinks, async settings, etc.
 *
 * Example:
 * @code
 *   logger_t *app_logger = logger_create("./applogger.yaml");
 *   if (app_logger) {
 *       LOGGER_INFO(app_logger, "Application starting");
 *       // ... use app_logger ...
 *       logger_destroy(app_logger);
 *   }
 * @endcode
 *
 * The macros expand to calls to logger_writevprintf, injecting the source location automatically.
 * @{
 */
/**
 * @def LOGGER_TRACE(logger, ...)
 * @brief Log a trace-level message on the given logger instance.
 *
 * Equivalent to TRACE but uses the specified logger instead of the global singleton. See
 * LOG_TRACE for message semantics and usage notes. The logger pointer must be non-NULL and
 * valid; passing NULL may cause undefined behavior (the function does not check for NULL
 * in the current implementation—it's the caller's responsibility).
 *
 * @param[in] logger Target logger instance to log to.
 * @param[in] ... Format arguments as per LOG_TRACE.
 */
#define LOGGER_TRACE(logger, ...)                                                                  \
    logger_writevprintf((logger), LOG_LEVEL_TRACE, LOG_FILENAME_ONLY(), __LINE__, __func__,        \
                        __VA_ARGS__)
/**
 * @def LOGGER_DEBUG(logger, ...)
 * @brief Log a debug-level message on the given logger instance.
 *
 * Equivalent to DEBUG but uses the specified logger instead of the global singleton. See
 * LOG_DEBUG for semantics. The logger must be non-NULL and valid.
 *
 * @param[in] logger Target logger instance.
 * @param[in] ... Format arguments as per LOG_DEBUG.
 */
#define LOGGER_DEBUG(logger, ...)                                                                  \
    logger_writevprintf((logger), LOG_LEVEL_DEBUG, LOG_FILENAME_ONLY(), __LINE__, __func__,        \
                        __VA_ARGS__)
/**
 * @def LOGGER_INFO(logger, ...)
 * @brief Log an informational message on the given logger instance.
 *
 * Equivalent to INFO but uses the specified logger instead of the global singleton. See
 * LOG_INFO for semantics and typical use cases.
 *
 * @param[in] logger Target logger instance (must be non-NULL and initialized).
 * @param[in] ... Format arguments as per LOG_INFO.
 */
#define LOGGER_INFO(logger, ...)                                                                   \
    logger_writevprintf((logger), LOG_LEVEL_INFO, LOG_FILENAME_ONLY(), __LINE__, __func__,         \
                        __VA_ARGS__)
/**
 * @def LOGGER_WARN(logger, ...)
 * @brief Log a warning message on the given logger instance.
 *
 * Equivalent to WARN but uses the specified logger instead of the global singleton. See
 * LOG_WARN for semantics and typical use cases (unusual conditions needing attention).
 *
 * @param[in] logger Target logger instance.
 * @param[in] ... Format arguments as per LOG_WARN.
 */
#define LOGGER_WARN(logger, ...)                                                                   \
    logger_writevprintf((logger), LOG_LEVEL_WARN, LOG_FILENAME_ONLY(), __LINE__, __func__,         \
                        __VA_ARGS__)
/**
 * @def LOGGER_ERROR(logger, ...)
 * @brief Log an error message on the given logger instance.
 *
 * Equivalent to ERROR but uses the specified logger instead of the global singleton. See
 * LOG_ERROR for semantics (failures affecting a request/operation).
 *
 * @param[in] logger Target logger instance.
 * @param[in] ... Format arguments as per LOG_ERROR.
 */
#define LOGGER_ERROR(logger, ...)                                                                  \
    logger_writevprintf((logger), LOG_LEVEL_ERROR, LOG_FILENAME_ONLY(), __LINE__, __func__,        \
                        __VA_ARGS__)
/**
 * @def LOGGER_FATAL(logger, ...)
 * @brief Log a fatal message on the given logger instance.
 *
 * Equivalent to FATAL but uses the specified logger instead of the global singleton. See
 * LOG_FATAL for semantics (severe failures requiring process termination).
 *
 * @param[in] logger Target logger instance.
 * @param[in] ... Format arguments as per LOG_FATAL.
 */
#define LOGGER_FATAL(logger, ...)                                                                  \
    logger_writevprintf((logger), LOG_LEVEL_FATAL, LOG_FILENAME_ONLY(), __LINE__, __func__,        \
                        __VA_ARGS__)
/** @} */

#endif /* LOG_H */
