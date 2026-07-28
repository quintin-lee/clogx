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
 * @brief Get a string description for the last error.
 * @return Static error string.
 */
const char *log_strerror(int err);

/**
 * @brief Register a callback invoked when async mode degrades to sync.
 * @param[in] cb Callback, or NULL to clear.
 *
 * @note Called with async fallback context (not async-signal-safe).
 */
void log_set_async_fallback_cb(void (*cb)(void));

/**
 * @brief Get the currently registered async fallback callback.
 * @return Callback function pointer, or NULL if none registered.
 */
void (*log_get_async_fallback_cb(void))(void);

/**
 * @brief Set the process-wide module name used for `%module`.
 * @param[in] module Module name; NULL or "" resets to `"main"`.
 */
void log_set_module(const char *module);

/**
 * @brief Copy the current module name into @p buf.
 * @param[out] buf Destination buffer.
 * @param[in]  n   Buffer capacity.
 */
void log_get_module(char *buf, size_t n);

/**
 * @brief Append a custom sink after @ref log_init.
 * @param[in] sink Sink to take ownership of (destroyed by @ref log_destroy / reload).
 * @return @p CLOG_OK on success, negative @ref clogx_errno_t on failure.
 *
 * @note Custom sinks added this way are discarded on @ref log_reload.
 */
int log_add_sink(log_sink_t *sink);

/**
 * @brief Remove a previously added sink without destroying it.
 * @param[in] sink Sink pointer previously passed to @ref log_add_sink.
 * @return @p CLOG_OK on success, @p CLOG_ERR_INVALID_ARG if @p sink is NULL.
 *
 * @note Caller retains ownership and must call @c sink->destroy when finished.
 */
int log_remove_sink(log_sink_t *sink);

/**
 * @brief Initialize the logging subsystem.
 *
 * Loads configuration, creates sinks, initializes the formatter, and starts
 * the async worker when `async` is enabled in the config.
 *
 * @param[in] yaml_path Path to a key:value config file. NULL or "" uses
 *                      `./config.yaml` if present.
 * @return @p CLOG_OK on success, negative @ref clogx_errno_t on failure.
 *
 * @note Call @ref log_destroy when finished. Calling @ref log_init again
 *       without destroy is not supported.
 */
int log_init(const char *yaml_path);

/**
 * @brief Shut down logging.
 *
 * Stops the async worker (draining pending records) and destroys all sinks.
 */
void log_destroy(void);

/**
 * @brief Flush pending log output.
 *
 * When async mode is enabled, waits until the queue is empty, then flushes
 * every sink. In sync mode, only flushes sinks.
 */
void log_flush(void);

/**
 * @brief Reload configuration and rebuild sinks.
 *
 * Always shuts down the async worker before replacing sinks, then restarts
 * it if the new config enables async mode.
 *
 * @return @p CLOG_OK on success, negative @ref clogx_errno_t on failure.
 */
int log_reload(void);

/**
 * @brief Format a message and dispatch a log record.
 *
 * Used by the @c LOG_* macros. Builds a @ref log_record_t on the stack and
 * either enqueues it asynchronously or dispatches it synchronously.
 *
 * @param[in] level Log severity.
 * @param[in] file  Source file name (usually from @p LOG_FILENAME_ONLY).
 * @param[in] line  Source line number.
 * @param[in] func  Source function name.
 * @param[in] fmt   printf-style format string.
 * @param[in] ...   Format arguments.
 */
void log_writevprintf(log_level_t level, const char *file, int line, const char *func,
                      const char *fmt, ...);

/**
 * @brief Extract the basename of @c __FILE__.
 * @return Pointer to the file name portion of @c __FILE__.
 */
/** @brief Extract the basename from a file path. Used by @p LOG_FILENAME_ONLY. */
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
 */

/**
 * @def LOG_INFO(...)
 * @brief Log an informational message.
 */
/**
 * @def LOG_DEBUG(...)
 * @brief Log a debug message.
 */
/**
 * @def LOG_WARN(...)
 * @brief Log a warning message.
 */
/**
 * @def LOG_ERROR(...)
 * @brief Log an error message.
 */
/**
 * @def LOG_FATAL(...)
 * @brief Log a fatal error message.
 */
/**
 * @def LOG_TRACE(...)
 * @brief Log a trace-level message.
 */
/**
 * @def TRACE(...)
 * @brief Deprecated alias for @ref LOG_TRACE.
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
#define TRACE(...) LOG_TRACE(__VA_ARGS__)

#endif /* LOG_H */
