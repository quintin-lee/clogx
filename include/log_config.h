/**
 * @file log_config.h
 * @brief Runtime configuration loaded from a simple key:value text file.
 */

#ifndef LOG_CONFIG_H
#define LOG_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "log_record.h"
#include "log_limits.h"

/* Reuse CLOGX_API from log.h if already included, otherwise define it. */
#ifndef CLOGX_API
#if defined(__GNUC__) || defined(__clang__)
#define CLOGX_API __attribute__((visibility("default")))
#else
#define CLOGX_API
#endif
#endif

typedef enum { LOG_FORMAT_TEXT = 0, LOG_FORMAT_JSON } log_format_type_t;

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
    log_level_t level;             /**< Minimum level that will be emitted. */
    bool async;                    /**< Enable background worker + queue. */
    int queue_size;                /**< Async queue capacity. */
    bool color;                    /**< Enable ANSI color for console sinks. */
    log_format_type_t format_type; /**< Output format mode (TEXT or JSON). */
    const char *format;            /**< Format string (points at internal storage). */
    const char *time_format; /**< strftime template for %%time (points at internal storage). */
    int console_enable;      /**< Non-zero to enable stdout/stderr sink. */
    int console_stderr;      /**< Non-zero to use stderr instead of stdout. */
    int file_enable;         /**< Non-zero to enable file sink. */
    char file_path[CLOG_MAX_PATH_SIZE];          /**< Log file path. */
    uint64_t file_max_size;                      /**< Rotate when file reaches this many bytes. */
    int file_backups;                            /**< Number of rotated backups to keep. */
    int socket_enable;                           /**< Non-zero to enable TCP socket sink. */
    char socket_host[CLOG_MAX_PATH_SIZE];        /**< Socket peer IPv4 address. */
    int socket_port;                             /**< Socket peer port. */
    bool socket_tls;                             /**< Enable TLS encryption for socket sink. */
    char socket_tls_ca_file[CLOG_MAX_PATH_SIZE]; /**< Path to CA certificate file (optional). */
    bool socket_tls_skip_verify;                 /**< Skip server cert verification. */
    bool rate_limit_enable;                      /**< Enable global rate limiting. */
    int rate_limit_max_per_sec;                  /**< Max allowed log events per second. */
    int rate_limit_burst;                        /**< Maximum burst capacity. */
    bool catch_signals;                          /**< Catch SIGTERM/SIGINT for graceful shutdown. */

    /* ---- Plugin sink configuration (loaded from YAML "plugins:" section) ---- */

    /** @brief Per-plugin configuration entry. */
    char plugin_so_paths[CLOG_MAX_PLUGINS][CLOG_MAX_PATH_SIZE];
    /** @brief Opaque JSON params for each plugin sink. */
    char plugin_params_json[CLOG_MAX_PLUGINS][CLOG_MAX_PLUGIN_CONFIG_SIZE];
    /** @brief Number of plugin entries parsed (0 .. CLOG_MAX_PLUGINS). */
    int plugin_count;
} log_config_t;

/**
 * @brief Access the process-wide configuration object.
 *
 * @note The returned pointer and its `format` / `time_format` strings point to
 *       internal storage.  They remain valid until the next @ref log_config_set,
 *       @ref log_config_init, or @ref log_config_reload call.
 *
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
 * @brief Apply a caller-provided configuration directly (thread-safe).
 *
 * All fields from @p cfg are copied into the process-wide config, including
 * `format` and `time_format` strings (deep-copied into internal buffers).
 * After this call @ref log_config_get returns the new values and any
 * previously remembered reload path is cleared.
 *
 * @note The caller retains ownership of @p cfg and its string members; they
 *       may be freed or reused immediately after this call returns.
 *
 * @param[in] cfg Configuration to apply.
 * @return 0 on success.
 */
CLOGX_API int log_config_set(const log_config_t *cfg);

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
