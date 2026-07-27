/**
 * @file log_formatter.h
 * @brief Token-based formatter for log lines.
 *
 * Format strings use tokens such as `[%time] [%level] %msg`.
 */

#ifndef LOG_FORMATTER_H
#define LOG_FORMATTER_H

#include <stddef.h>
#include "log_record.h"

/**
 * @brief Format @p record into @p buf according to the active format string.
 *
 * @param[in]  record   Log event to format.
 * @param[out] buf      Output buffer (NUL-terminated on success).
 * @param[in]  buf_size Capacity of @p buf in bytes.
 * @return Bytes written excluding the terminating NUL, or <= 0 on failure.
 */
int log_formatter_format(log_record_t *record, char *buf, size_t buf_size);

/**
 * @brief Set the active format string (copied into internal storage).
 *
 * Supported tokens:
 * - `%time` — local timestamp with microseconds
 * - `%level` — severity name
 * - `%msg` — message body
 * - `%thread` / `%pid`
 * - `%file` / `%line` / `%func`
 * - `%module` / `%tag`
 * - `%newline`
 *
 * @param[in] format Format string; NULL or empty restores `"%msg"`.
 * @return 0 on success.
 */
int log_formatter_init(const char *format);

/**
 * @brief Reset the formatter to the default format (`%msg`).
 */
void log_formatter_reset(void);

/**
 * @brief Get the currently active format string.
 * @return Pointer to internal storage (do not free).
 */
const char *log_formatter_get_format(void);

#endif /* LOG_FORMATTER_H */
