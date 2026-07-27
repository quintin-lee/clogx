/**
 * @file log_config.h
 * @brief Runtime configuration loaded from a simple key:value text file.
 */

#ifndef LOG_CONFIG_H
#define LOG_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include "log_record.h"

/**
 * @struct log_config_t
 * @brief Process-wide logging configuration.
 *
 * @note `level` is read/written under an rwlock via @ref log_get_level /
 *       @ref log_set_level. Other fields are typically read after init/reload.
 */
typedef struct {
    log_level_t level;       /**< Minimum level that will be emitted. */
    bool async;              /**< Enable background worker + queue. */
    int queue_size;          /**< Async queue capacity. */
    bool color;              /**< Enable ANSI color for console sinks. */
    const char *format;      /**< Format string (points at internal storage). */
    int console_enable;      /**< Non-zero to enable stdout sink. */
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
log_config_t *log_config_get(void);

/**
 * @brief Initialize configuration from defaults and an optional file.
 *
 * Remembers @p yaml_path for later @ref log_config_reload calls.
 *
 * @param[in] yaml_path Config path. NULL or "" defaults to `./config.yaml`.
 * @return 0 on success.
 */
int log_config_init(const char *yaml_path);

/**
 * @brief Re-parse the config path remembered by @ref log_config_init.
 * @return 0 on success.
 */
int log_config_reload(void);

/**
 * @brief Set the minimum emit level (thread-safe).
 * @param[in] level New minimum level.
 * @return 0 on success.
 */
int log_set_level(log_level_t level);

/**
 * @brief Get the current minimum emit level (thread-safe).
 * @return Current @ref log_level_t.
 */
log_level_t log_get_level(void);

/**
 * @brief Check whether @p level should be emitted under the current config.
 * @param[in] level Candidate severity.
 * @return Non-zero if the message should be written.
 */
static inline int log_should_emit(log_level_t level) {
    return level >= log_get_level();
}

#endif /* LOG_CONFIG_H */
