/**
 * @file log_limits.h
 * @brief Compile-time configurable buffer size limits and constants.
 *
 * All limits are defined as preprocessor macros guarded by `#ifndef`, so
 * users can override any value at compile time via `-D` flags without
 * modifying the header:
 *
 *     cc -DCLOG_MAX_MESSAGE_SIZE=8192 ...
 *
 * ## Buffer Size Reference
 *
 * | Macro                         | Default  | Used by                                        |
 * |-------------------------------|----------|------------------------------------------------|
 * | @ref CLOG_MAX_MESSAGE_SIZE    | 4096     | Raw log message body (vsnprintf output)        |
 * | @ref CLOG_MAX_FORMATTED_SIZE  | 8192     | Formatted log line (after token expansion)     |
 * | @ref CLOG_MAX_COLORED_SIZE    | 16384    | ANSI-colorised output line                     |
 * | @ref CLOG_MAX_FORMAT_SIZE     | 1024     | Format string storage                          |
 * | @ref CLOG_MAX_PATH_SIZE       | 512      | File paths, socket addresses                   |
 * | @ref CLOG_MAX_PLUGINS         | 8        | Number of concurrently loaded plugins          |
 * | @ref CLOG_MAX_PLUGIN_CONFIG_SIZE | 4096  | Per-plugin YAML/JSON config blob size          |
 *
 * ## Safety
 *
 * All formatting functions check these limits at runtime: if output would
 * exceed the buffer size, the line is truncated and NUL-terminated. No
 * unchecked snprintf or strcpy calls exist in the formatting pipeline.
 *
 * @note These are NOT tuning parameters for performance — they are
 *       safety limits. Performance-sensitive sizing is driven by
 *       @ref log_config_t::queue_size and the token bucket rates.
 */

#ifndef LOG_LIMITS_H
#define LOG_LIMITS_H

/**
 * @def CLOG_MAX_MESSAGE_SIZE
 * @brief Maximum byte length of a single log message body (the user's
 *        format string after vsnprintf expansion).
 *
 * Messages longer than this are silently truncated.
 */
#ifndef CLOG_MAX_MESSAGE_SIZE
#define CLOG_MAX_MESSAGE_SIZE 4096
#endif

/**
 * @def CLOG_MAX_FORMATTED_SIZE
 * @brief Maximum byte length of a formatted log line (message body after
 *        token substitution: `[%time] [%level] %msg`).
 *
 * This must be >= @ref CLOG_MAX_MESSAGE_SIZE plus the overhead of prefix
 * tokens (timestamp, level name, file:line, etc.).
 */
#ifndef CLOG_MAX_FORMATTED_SIZE
#define CLOG_MAX_FORMATTED_SIZE 8192
#endif

/**
 * @def CLOG_MAX_COLORED_SIZE
 * @brief Maximum byte length of an ANSI-coloured log line.
 *
 * Colour escape sequences can add ~30–40 bytes per line, so this is
 * larger than @ref CLOG_MAX_FORMATTED_SIZE.
 */
#ifndef CLOG_MAX_COLORED_SIZE
#define CLOG_MAX_COLORED_SIZE 16384
#endif

/**
 * @def CLOG_MAX_FORMAT_SIZE
 * @brief Maximum byte length of the user-specified format string
 *        (e.g. `"[%time] [%level] %file:%line %msg"`).
 */
#ifndef CLOG_MAX_FORMAT_SIZE
#define CLOG_MAX_FORMAT_SIZE 1024
#endif

/**
 * @def CLOG_MAX_PATH_SIZE
 * @brief Maximum byte length of a file path, socket host, or other string
 *        path configuration value.
 */
#ifndef CLOG_MAX_PATH_SIZE
#define CLOG_MAX_PATH_SIZE 512
#endif

/**
 * @def CLOG_MAX_PLUGINS
 * @brief Maximum number of dynamically loaded plugins that can be active
 *        simultaneously.
 */
#ifndef CLOG_MAX_PLUGINS
#define CLOG_MAX_PLUGINS 8
#endif

/**
 * @def CLOG_MAX_PLUGIN_CONFIG_SIZE
 * @brief Maximum byte length of the configuration data passed to each
 *        plugin's init function.
 */
#ifndef CLOG_MAX_PLUGIN_CONFIG_SIZE
#define CLOG_MAX_PLUGIN_CONFIG_SIZE 4096
#endif

/**
 * @def CLOG_MAX_KV
 * @brief Maximum number of structured key-value attributes per log record.
 */
#ifndef CLOG_MAX_KV
#define CLOG_MAX_KV 16
#endif

#endif /* LOG_LIMITS_H */
