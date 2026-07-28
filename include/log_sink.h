/**
 * @file log_sink.h
 * @brief Sink interface and factory functions for log destinations.
 */

#ifndef LOG_SINK_H
#define LOG_SINK_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "log_record.h"

/**
 * @struct log_sink
 * @brief Polymorphic sink: write/flush/destroy plus private state.
 */
typedef struct log_sink log_sink_t;

struct log_sink {
    /**
     * @brief Write @p len bytes from @p buf.
     * @return Number of bytes written, or -1 on error.
     */
    int (*write)(log_sink_t *sink, const char *buf, size_t len);

    /** @brief Flush buffered output. */
    void (*flush)(log_sink_t *sink);

    /** @brief Release resources and free the sink object. */
    void (*destroy)(log_sink_t *sink);

    /** @brief Implementation-private data. */
    void *private_data;

    /**
     * @brief Minimum severity level this sink accepts.
     *
     * Messages below this level are silently dropped by the dispatcher.
     * Initialised to LOG_LEVEL_TRACE (accepts everything) in the factory
     * helpers; override with log_sink_set_level().
     */
    log_level_t min_level;
};

/**
 * @brief Create a stdout console sink.
 * @param[in] use_color When true, the dispatcher may wrap output in ANSI colors.
 * @return New sink, or NULL on allocation failure.
 */
log_sink_t *console_sink_create(bool use_color);

/**
 * @brief Create a stderr console sink.
 * @param[in] use_color When true, the dispatcher may wrap output in ANSI colors.
 * @return New sink, or NULL on allocation failure.
 */
log_sink_t *console_sink_create_stderr(bool use_color);

/**
 * @brief Test whether @p sink is a color-enabled console sink.
 * @param[in] sink Sink to query (may be NULL).
 * @return true if color output is enabled for this sink.
 */
bool console_sink_is_color_enabled(log_sink_t *sink);

/**
 * @brief Create an append-only file sink with size-based rotation.
 *
 * Creates parent directories as needed. Rotates when the cumulative written
 * size reaches @p max_size.
 *
 * @param[in] path     Destination file path.
 * @param[in] max_size Rotate threshold in bytes (0 disables rotation).
 * @param[in] backups  Number of numbered backups to retain (`.1` .. `.N`).
 * @return New sink, or NULL on error.
 */
log_sink_t *file_sink_create(const char *path, uint64_t max_size, int backups);

/**
 * @brief Create a lazy-connect TCP socket sink.
 * @param[in] host IPv4 address string (e.g. `"127.0.0.1"`).
 * @param[in] port Destination port in host byte order (1..65535).
 * @return New sink, or NULL on invalid arguments / allocation failure.
 */
log_sink_t *socket_sink_create(const char *host, int port);

/**
 * @brief Set the minimum severity level for a sink.
 *
 * Messages below this level are dropped by the dispatcher before reaching
 * the sink's write callback.  Default is LOG_LEVEL_TRACE (no filtering).
 *
 * @param[in] sink  Target sink (must not be NULL).
 * @param[in] level Minimum level to accept.
 */
static inline void log_sink_set_level(log_sink_t *sink, log_level_t level) {
    if (sink)
        sink->min_level = level;
}

/**
 * @brief Get the minimum severity level for a sink.
 * @param[in] sink Sink to query (may be NULL).
 * @return Current minimum level, or LOG_LEVEL_TRACE if sink is NULL.
 */
static inline log_level_t log_sink_get_level(const log_sink_t *sink) {
    return sink ? sink->min_level : LOG_LEVEL_TRACE;
}

#endif /* LOG_SINK_H */
