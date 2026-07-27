#ifndef LOG_FORMATTER_H
#define LOG_FORMATTER_H

#include <stddef.h>
#include "log_record.h"

// Format a log record into a buffer
// Returns the number of characters written, or negative on error
int log_formatter_format(log_record_t *record, char *buf, size_t buf_size);

// Initialize formatter with format string
// format: e.g., "[%time] [%level] %msg"
// Supported tokens: %time, %level, %thread, %pid, %file, %line,
//                   %func, %msg, %module, %tag, %newline
int log_formatter_init(const char *format);

// Reset formatter to default format
void log_formatter_reset(void);

// Get current format string
const char *log_formatter_get_format(void);

#endif // LOG_FORMATTER_H
