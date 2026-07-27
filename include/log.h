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

/**
 * @brief Initialize the logging subsystem.
 *
 * Loads configuration, creates sinks, initializes the formatter, and starts
 * the async worker when `async` is enabled in the config.
 *
 * @param[in] yaml_path Path to a key:value config file. NULL or "" uses
 *                      `./config.yaml` if present.
 * @return 0 on success, -1 on failure.
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
 * @return 0 on success, -1 on failure.
 */
int log_reload(void);

/**
 * @brief Format a message and dispatch a log record.
 *
 * Used by the @c LOG_* macros. Builds a @ref log_record_t on the stack and
 * either enqueues it asynchronously or dispatches it synchronously.
 *
 * @param[in] level Log severity.
 * @param[in] file  Source file name (usually from @ref LOG_FILENAME_ONLY).
 * @param[in] line  Source line number.
 * @param[in] func  Source function name.
 * @param[in] fmt   printf-style format string.
 * @param[in] ...   Format arguments.
 */
void log_writevprintf(
    log_level_t level,
    const char *file,
    int line,
    const char *func,
    const char *fmt,
    ...);

/**
 * @brief Extract the basename of @c __FILE__ when it is a compile-time constant.
 * @return Pointer to the file name portion of @c __FILE__.
 */
#define LOG_FILENAME_ONLY() (__builtin_constant_p(__FILE__) ? \
    strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__ : \
    __FILE__)

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
 * @def TRACE(...)
 * @brief Log a trace-level message.
 */
#define LOG_INFO(...)   log_writevprintf(LOG_LEVEL_INFO,  LOG_FILENAME_ONLY(), __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOG_DEBUG(...)  log_writevprintf(LOG_LEVEL_DEBUG, LOG_FILENAME_ONLY(), __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOG_WARN(...)   log_writevprintf(LOG_LEVEL_WARN,  LOG_FILENAME_ONLY(), __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOG_ERROR(...)  log_writevprintf(LOG_LEVEL_ERROR, LOG_FILENAME_ONLY(), __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOG_FATAL(...)  log_writevprintf(LOG_LEVEL_FATAL, LOG_FILENAME_ONLY(), __LINE__, __FUNCTION__, __VA_ARGS__)
#define TRACE(...)      log_writevprintf(LOG_LEVEL_TRACE, LOG_FILENAME_ONLY(), __LINE__, __FUNCTION__, __VA_ARGS__)

#endif /* LOG_H */
