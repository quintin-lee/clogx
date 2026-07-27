#ifndef LOG_RECORD_H
#define LOG_RECORD_H

#include <stdint.h>
#include <stdbool.h>

// Log levels
typedef enum {
    LOG_LEVEL_TRACE,
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_FATAL
} log_level_t;

// Log record structure
typedef struct {
    log_level_t level;                // Log level
    uint64_t timestamp;               // Timestamp (microseconds since epoch)
    uint32_t tid;                     // Thread ID
    uint32_t pid;                     // Process ID
    const char *file;                 // Source file name
    const char *func;                 // Function name
    int line;                         // Line number
    const char *module;               // Module name
    const char *tag;                  // Tag/label
    const char *message;              // Message text
} log_record_t;

// Color codes for terminal output
typedef enum {
    COLOR_NONE = 0,
    COLOR_BLACK,
    COLOR_RED,
    COLOR_GREEN,
    COLOR_YELLOW,
    COLOR_BLUE,
    COLOR_PURPLE,
    COLOR_CYAN,
    COLOR_WHITE
} log_color_t;

// Map log level to color
static inline log_color_t get_log_color(log_level_t level) {
    switch ((int)level) {
        case LOG_LEVEL_TRACE: return COLOR_BLACK;
        case LOG_LEVEL_DEBUG: return COLOR_BLUE;
        case LOG_LEVEL_INFO: return COLOR_GREEN;
        case LOG_LEVEL_WARN: return COLOR_YELLOW;
        case LOG_LEVEL_ERROR: return COLOR_RED;
        case LOG_LEVEL_FATAL: return COLOR_PURPLE;
        default: return COLOR_NONE;
    }
}

#endif // LOG_RECORD_H
