/**
 * @file log_record.h
 * @brief Log event types shared across the write path, queue, and dispatcher.
 */

#ifndef LOG_RECORD_H
#define LOG_RECORD_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @enum log_level_t
 * @brief Severity levels ordered from most verbose to most severe.
 */
typedef enum {
    LOG_LEVEL_TRACE, /**< Finest-grained diagnostic output. */
    LOG_LEVEL_DEBUG, /**< Debug diagnostics. */
    LOG_LEVEL_INFO,  /**< Normal operational messages. */
    LOG_LEVEL_WARN,  /**< Unexpected but recoverable conditions. */
    LOG_LEVEL_ERROR, /**< Failures that affect a request/operation. */
    LOG_LEVEL_FATAL  /**< Severe failures; process may be unusable. */
} log_level_t;

/**
 * @struct log_record_t
 * @brief In-memory representation of a single log event.
 *
 * @details String fields (`file`, `func`, `module`, `tag`, `message`) are
 * borrowed pointers on the synchronous path. The async layer deep-copies
 * them onto the heap before enqueueing.
 */
typedef struct {
    log_level_t level;   /**< Severity. */
    uint64_t timestamp;  /**< Microseconds since Unix epoch. */
    uint32_t tid;        /**< Thread identifier (truncated pthread_t). */
    uint32_t pid;        /**< Process identifier. */
    const char *file;    /**< Source file name. */
    const char *func;    /**< Source function name. */
    int line;            /**< Source line number. */
    const char *module;  /**< Logical module name. */
    const char *tag;     /**< Optional tag/label (may be NULL). */
    const char *message; /**< Formatted message body. */
} log_record_t;

/**
 * @enum log_color_t
 * @brief ANSI color selectors used for console output.
 */
typedef enum {
    COLOR_NONE = 0, /**< No color / fallback. */
    COLOR_BLACK,
    COLOR_RED,
    COLOR_GREEN,
    COLOR_YELLOW,
    COLOR_BLUE,
    COLOR_PURPLE,
    COLOR_CYAN,
    COLOR_WHITE
} log_color_t;

/**
 * @brief Map a log level to a console color.
 * @param[in] level Log severity.
 * @return Corresponding @ref log_color_t value.
 */
static inline log_color_t get_log_color(log_level_t level) {
    switch ((int)level) {
    case LOG_LEVEL_TRACE:
        return COLOR_BLACK;
    case LOG_LEVEL_DEBUG:
        return COLOR_BLUE;
    case LOG_LEVEL_INFO:
        return COLOR_GREEN;
    case LOG_LEVEL_WARN:
        return COLOR_YELLOW;
    case LOG_LEVEL_ERROR:
        return COLOR_RED;
    case LOG_LEVEL_FATAL:
        return COLOR_PURPLE;
    default:
        return COLOR_NONE;
    }
}

#endif /* LOG_RECORD_H */
