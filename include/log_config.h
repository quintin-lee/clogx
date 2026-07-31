/**
 * @file log_config.h
 * @brief Runtime configuration for the clogx logging library.
 *
 * The configuration system supports loading settings from YAML files or
 * programmatically via C structures. All configuration fields are copied
 * into internal storage at set time, making the config objects safe to free
 * immediately after use.
 *
 * Configuration is per-logger-instance when using the multi-instance API;
 * the singleton logger (@ref log_init) uses its own independent config copy.
 */

#ifndef LOG_CONFIG_H
#define LOG_CONFIG_H

#include "log_limits.h"
#include "log_record.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Reuse CLOGX_API from log.h if already included, otherwise define it. */
#ifndef CLOGX_API
#if defined(__GNUC__) || defined(__clang__)
#define CLOGX_API __attribute__((visibility("default")))
#else
#define CLOGX_API
#endif
#endif

/**
 * @enum log_format_type_t
 * @brief Output format mode for log records.
 *
 * The format type determines how log messages are rendered:
 *   - LOG_FORMAT_TEXT: Custom text format string (e.g., "[%time] [%level] %msg")
 *   - LOG_FORMAT_JSON: Single-line JSON formatted per RFC 8259
 *   - LOG_FORMAT_OTEL_JSON: OpenTelemetry-compatible JSON with resource attributes
 *
 * These values correspond to the format string setting in configuration.
 * When set to JSON/OTEL, the custom format string is ignored.
 */
typedef enum {
    LOG_FORMAT_TEXT = 0, /**< Custom text format specified by @c format string. */
    LOG_FORMAT_JSON,     /**< Structured single-line JSON output. */
    LOG_FORMAT_OTEL_JSON /**< OpenTelemetry-compatible JSON with resource attributes. */
} log_format_type_t;

/**
 * @struct log_config_t
 * @brief Process-wide (or per-instance) logging configuration.
 *
 * This structure holds all configurable parameters for the logging system.
 * Fields can be modified either through the @ref log_config_set API (for
 * the process-wide singleton) or directly on a @ref logger_t instance before
 * calling @ref logger_create_from_config.
 *
 * Design Notes:
 *   - String fields (@c format, @c time_format, @c file_path, etc.) point
 *     into internally-managed buffers that outlive this struct.
 *   - The @c async flag controls whether an background worker thread is
 *     started; when true, @c queue_size must be positive and non-zero.
 *   - Rate limiting uses a token bucket algorithm: @c rate_limit_max_per_sec
 *     sets the refill rate, @c rate_limit_burst sets the bucket capacity.
 *   - File rotation is size-based: when the file reaches @c file_max_size,
 *     it is renamed and a new one is created; up to @c file_backups
 *     rotated copies are retained.
 *   - Plugin sinks are loaded from the YAML "plugins:" section; each plugin
 *     has a shared object path and optional JSON parameters.
 *
 * Thread Safety Considerations:
 *   - On concurrent access, hot-path fields like @c level, @c async, and
 *     @c color should be accessed via accessor functions (@ref
 *     log_get_level, @ref log_config_is_async, etc.) rather than direct
 *     struct reads to avoid tearing.
 *   - The entire config snapshot used by a logger is immutable after
 *     initialization/modification—changes require a full reload.
 */
typedef struct {
    log_level_t level;       /**< Minimum level that will be emitted (TRACE..FATAL). */
    bool        async;       /**< Enable background worker + queue (true/false). */
    int         queue_size;  /**< Async queue capacity (number of records to buffer). Must be > 0 if
                                async=true. */
    bool              color; /**< Enable ANSI color codes for console sink output. */
    log_format_type_t format_type; /**< Output format mode (TEXT or JSON/OTEL). */
    const char       *format; /**< Format string template (points at internal storage). Ignored if
                                 format_type != TEXT. */
    const char *time_format; /**< strftime template for %time token (points at internal storage). */
    int         console_enable; /**< Non-zero to enable stdout/stderr sink. 0 disables. */
    int         console_stderr; /**< Non-zero to use stderr instead of stdout for console output. */
    int         file_enable;    /**< Non-zero to enable file sink with rotation. */
    char        file_path[CLOG_MAX_PATH_SIZE]; /**< Destination log file path (null-terminated). */
    uint64_t    file_max_size; /**< Rotation threshold in bytes (0 = disable rotation). */
    int         file_backups;  /**< Number of numbered backups to keep (.1 .. .N). */
    int         socket_enable; /**< Non-zero to enable TCP socket sink. */
    char socket_host[CLOG_MAX_PATH_SIZE]; /**< Socket peer IPv4 address (e.g., "127.0.0.1"). */
    int  socket_port;                     /**< Socket peer port in host byte order (1..65535). */
    bool socket_tls; /**< Enable TLS encryption for socket sink (requires OpenSSL). */
    char socket_tls_ca_file[CLOG_MAX_PATH_SIZE]; /**< Path to CA certificate file for TLS
                                                     verification. */
    bool socket_tls_skip_verify;    /**< Skip server certificate verification (insecure, for testing
                                        only). */
    bool     socket_async;          /**< Enable async non-blocking socket with ring buffer. */
    size_t   socket_ring_capacity;  /**< Ring buffer capacity for async socket (lines). 0 = 8192. */
    uint32_t socket_backoff_min_ms; /**< Initial reconnect backoff in ms. 0 = 1000. */
    uint32_t socket_backoff_max_ms; /**< Max reconnect backoff in ms. 0 = 60000. */
    bool     rate_limit_enable;     /**< Enable global token bucket rate limiting. */
    int      rate_limit_max_per_sec; /**< Maximum allowed log events per second. */
    int      rate_limit_burst;       /**< Maximum burst capacity for the token bucket. */
    bool     catch_signals; /**< Catch SIGTERM/SIGINT for graceful shutdown via self-pipe. */

    /* ---- Prometheus /metrics endpoint ---- */

    bool prometheus_enable; /**< Enable Prometheus HTTP /metrics endpoint. */
    int  prometheus_port;   /**< Listen port for metrics HTTP server (0 = disable). */

    /* ---- Plugin sink configuration (loaded from YAML "plugins:" section) ---- */

    /** @brief Array of shared object paths for plugin libraries (up to CLOG_MAX_PLUGINS). */
    char plugin_so_paths[CLOG_MAX_PLUGINS][CLOG_MAX_PATH_SIZE];
    /** @brief Opaque JSON params for each plugin sink (serialized string). */
    char plugin_params_json[CLOG_MAX_PLUGINS][CLOG_MAX_PLUGIN_CONFIG_SIZE];
    /** @brief Number of plugin entries actually parsed (0 .. CLOG_MAX_PLUGINS). */
    int plugin_count;
} log_config_t;

typedef struct logger_t logger_t;

/**
 * @brief Load configuration into a specific logger instance.
 *
 * This function is called during @ref logger_create and @ref logger_reload.
 * It sets default values on @p logger->config, then optionally parses a
 * YAML file to override defaults. Format strings are copied into the
 * logger's owned storage, ensuring they remain valid as long as the logger exists.
 *
 * @param[out] logger Target logger instance to initialize (must be non-NULL).
 * @param[in] yaml_path Config file path. NULL or "" loads defaults only;
 *                      relative paths are resolved relative to working directory.
 *
 * @return 0 on success, -1 on parse error or file IO failure. Error details
 *         are available via @ref log_strerror if errno is set.
 *
 * Note: The function writes to @p logger->config fields directly; after
 *       success, the config contains valid internal string pointers.
 *       The @p yaml_path is remembered for subsequent reloads unless
 *       overwritten by @ref log_config_set.
 */
CLOGX_API int log_config_load_into(logger_t *logger, const char *yaml_path);

/**
 * @brief Access the process-wide configuration object for the singleton logger.
 *
 * Returns a pointer to the config structure that governs the process-wide
 * logging behavior initialized by @ref log_init. The returned pointer remains
 * valid until the next call to @ref log_config_set, @ref log_config_init,
 * or @ref log_config_reload, which may invalidate internal string pointers.
 *
 * @note The returned pointer and its `format` / `time_format` strings point to
 *       internal storage managed by the library. Do not modify them directly.
 *       Copy the struct if you need a mutable snapshot.
 *
 * @return Pointer valid after @ref log_config_init or @ref log_init.
 *
 * Warning: Concurrent access to this struct without using accessor functions
 *          may cause data races. Use @ref log_get_level, @ref
 *          log_config_is_async, etc. for thread-safe reads.
 */
CLOGX_API log_config_t *log_config_get(void);

/**
 * @brief Initialize configuration from defaults and an optional YAML file.
 *
 * Sets process-wide config defaults and optionally parses a YAML file to
 * override them. Remembers the @p yaml_path for later @ref log_config_reload
 * calls, allowing dynamic configuration updates without storing the path separately.
 *
 * @param[in] yaml_path Config file path. NULL or "" defaults to "./config.yaml".
 *                      If the file does not exist, defaults are used without error.
 *
 * @return 0 on success, -1 on file IO or parsing failure.
 *
 * Note: After a successful call, @ref log_config_get returns a valid config.
 *       The remembered path enables hot-reload via @ref log_config_reload.
 */
CLOGX_API int log_config_init(const char *yaml_path);

/**
 * @brief Apply a caller-provided configuration directly (thread-safe).
 *
 * Copies all fields from @p cfg into the process-wide configuration, including
 * deep-copies `format` and `time_format` strings into internal buffers. After
 * this call, @ref log_config_get returns the new values and any previously
 * remembered reload path is cleared (since there is no longer a source file
 * to re-parse).
 *
 * This API is useful for programmatic configuration where YAML files are not
 * desired, or when configuration values are computed at runtime.
 *
 * @param[in] cfg Configuration to apply (must be non-NULL).
 *
 * @return 0 on success, negative error code if validation fails (e.g., invalid queue_size).
 *
 * @note The caller retains ownership of @p cfg and its string members; they
 *       may be freed or reused immediately after this call returns because
 *       the library makes its own copies. Deep-copying ensures the config
 *       remains valid even after the caller's struct goes out of scope.
 *       Thread-safe: acquires an internal mutex during the copy operation.
 */
CLOGX_API int log_config_set(const log_config_t *cfg);

/**
 * @brief Re-parse the config path remembered by @ref log_config_init.
 *
 * Reloads the configuration from the file originally passed to @ref log_config_init
 * (or last remembered via some other means). This enables hot-reloading of
 * logging configuration without restarting the application.
 *
 * After successful reload, the process-wide config is updated atomically;
 * active loggers will receive the new config on their next operation (some
 * implementations may require explicit sync via @ref logger_reload).
 *
 * @return 0 on success, -1 on file IO or parsing failure.
 *
 * Note: This is similar to @ref log_reload but operates only on the config,
 *       not necessarily recreating sinks (depends on implementation). For full
 *       sink recreation, use @ref log_reload which also rebuilds the sink list.
 */
CLOGX_API int log_config_reload(void);

/**
 * @brief Set the minimum emit level for the process-wide logger (thread-safe).
 *
 * Changes the severity threshold below which log messages are discarded.
 * This affects all subsequently logged messages across all sinks unless
 * overridden per-sink with @ref log_sink_set_level.
 *
 * @param[in] level New minimum severity level (LOG_LEVEL_TRACE .. LOG_LEVEL_FATAL).
 *
 * @return 0 on success, -1 if the level is invalid (out of range).
 *
 * Note Thread-safe: writes protected by internal mutex. The change takes effect
 *       immediately for all new log records. Does not affect pending records.
 */
CLOGX_API int log_set_level(log_level_t level);

/**
 * @brief Get the current minimum emit level for the process-wide logger (thread-safe).
 *
 * Reads the configured minimum severity threshold. Used internally by
 * @ref log_should_emit and exposed for inspection/debugging purposes.
 *
 * @return Current log level setting. Default is LOG_LEVEL_INFO if not explicitly set.
 *
 * Note: This function is lightweight and lock-free or read-lock optimized for
 *       hot-path usage. Safe to call from any thread, including signal handlers.
 */
CLOGX_API log_level_t log_get_level(void);

/**
 * @brief Check whether async mode is enabled (thread-safe).
 *
 * Returns the value of the @c async field from the current configuration.
 * Preferred over reading log_config_get()->async directly on concurrent paths
 * to avoid potential tearing or stale reads.
 *
 * @return true when the async worker path is configured and active, false otherwise.
 *
 * Note: Thread-safe read operation. Suitable for hot-path decision making about
 *       whether to enqueue or dispatch directly.
 */
CLOGX_API bool log_config_is_async(void);

/**
 * @brief Whether console ANSI coloring is enabled (thread-safe).
 *
 * Checks the @c color field in the active configuration. Use this accessor
 * instead of direct struct access on performance-critical paths.
 *
 * @return true when color output is configured for console sinks, false otherwise.
 *
 * Note: Thread-safe. Fast read with minimal synchronization overhead.
 */
CLOGX_API bool log_config_color_enabled(void);

/**
 * @brief Check whether @p level should be emitted under the current config.
 *
 * Fast-path inline function that compares the candidate severity against
 * the minimum configured level. Typically used as a pre-filter before
 * constructing the full log record to avoid unnecessary formatting work
 * when the message would be dropped anyway.
 *
 * @param[in] level Candidate log severity (log_level_t).
 *
 * @return Non-zero (true) if the message should be written; zero (false) if
 *         below the minimum configured level.
 *
 * Note: Always evaluates @p level exactly once. Inlined at call site for
 *       zero runtime cost. Uses @ref log_get_level internally (thread-safe).
 */
static inline int log_should_emit(log_level_t level)
{
    return level >= log_get_level();
}

#endif /* LOG_CONFIG_H */
