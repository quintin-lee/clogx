/**
 * @file log_config.h
 * @brief Runtime configuration loaded from a simple key:value text file.
 */

#ifndef LOG_CONFIG_H
#define LOG_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include "log_record.h"

/* Reuse CLOGX_API from log.h if already included, otherwise define it. */
#ifndef CLOGX_API
#if defined(__GNUC__) || defined(__clang__)
#define CLOGX_API __attribute__((visibility("default")))
#else
#define CLOGX_API
#endif
#endif

/**
 * @struct log_config_t
 * @brief Process-wide logging configuration.
 *
 * @note Hot-path fields `level`, `async`, and `color` should be read via
 *       @ref log_get_level, @ref log_config_is_async, and
 *       @ref log_config_color_enabled. Prefer those over @ref log_config_get
 *       on concurrent paths.
 */
typedef struct {
    log_level_t level;       /**< Minimum level that will be emitted. */
    bool async;              /**< Enable background worker + queue. */
    int queue_size;          /**< Async queue capacity. */
    bool color;              /**< Enable ANSI color for console sinks. */
    const char *format;      /**< Format string (points at internal storage). */
    const char *time_format; /**< strftime template for %%time (points at internal storage). */
    int console_enable;      /**< Non-zero to enable stdout/stderr sink. */
    int console_stderr;      /**< Non-zero to use stderr instead of stdout. */
    int file_enable;         /**< Non-zero to enable file sink. */
    char file_path[256];     /**< Log file path. */
    uint64_t file_max_size;  /**< Rotate when file reaches this many bytes. */
    int file_backups;        /**< Number of rotated backups to keep. */
    int socket_enable;       /**< Non-zero to enable TCP socket sink. */
    char socket_host[256];   /**< Socket peer IPv4 address. */
    int socket_port;         /**< Socket peer port. */
} log_config_t;

/**
 * @brief Access the process-wide configuration object.
 * @return Pointer valid after @ref log_config_init.
 */
CLOGX_API log_config_t *log_config_get(void);

/**
 * @brief Initialize configuration from defaults and an optional file.
 *
 * Remembers @p yaml_path for later @ref log_config_reload calls.
 *
 * @param[in] yaml_path Config path. NULL or "" defaults to `./config.yaml`.
 * @return 0 on success.
 */
CLOGX_API int log_config_init(const char *yaml_path);

/**
 * @brief Re-parse the config path remembered by @ref log_config_init.
 * @return 0 on success.
 */
CLOGX_API int log_config_reload(void);

/**
 * @brief Set the minimum emit level (thread-safe).
 * @param[in] level New minimum level.
 * @return 0 on success.
 */
CLOGX_API int log_set_level(log_level_t level);

/**
 * @brief Get the current minimum emit level (thread-safe).
 * @return Current @ref log_level_t.
 */
CLOGX_API log_level_t log_get_level(void);

/**
 * @brief Whether async mode is enabled (thread-safe).
 * @return true when the async worker path is configured.
 */
CLOGX_API bool log_config_is_async(void);

/**
 * @brief Whether console ANSI coloring is enabled (thread-safe).
 * @return true when color output is configured.
 */
CLOGX_API bool log_config_color_enabled(void);

/**
 * @brief Check whether @p level should be emitted under the current config.
 * @param[in] level Candidate severity.
 * @return Non-zero if the message should be written.
 */
static inline int log_should_emit(log_level_t level) {
    return level >= log_get_level();
}

#endif /* LOG_CONFIG_H */
