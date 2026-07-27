#ifndef LOG_H
#define LOG_H

#include <stdarg.h>
#include <string.h>
#include "log_config.h"
#include "log_record.h"

// Function prototypes (public API)
int log_init(const char *yaml_path);
void log_destroy(void);
void log_flush(void);
int log_reload(void);
void log_writevprintf(
    log_level_t level,
    const char *file,
    int line,
    const char *func,
    const char *fmt,
    ...);

// Macro to get current file name (without path)
#define LOG_FILENAME_ONLY() (__builtin_constant_p(__FILE__) ? \
    strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__ : \
    __FILE__)

// Log level macros - pass all variadic args directly to log_writevprintf
#define LOG_INFO(...)   log_writevprintf(LOG_LEVEL_INFO,  LOG_FILENAME_ONLY(), __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOG_DEBUG(...)  log_writevprintf(LOG_LEVEL_DEBUG, LOG_FILENAME_ONLY(), __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOG_WARN(...)   log_writevprintf(LOG_LEVEL_WARN,  LOG_FILENAME_ONLY(), __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOG_ERROR(...)  log_writevprintf(LOG_LEVEL_ERROR, LOG_FILENAME_ONLY(), __LINE__, __FUNCTION__, __VA_ARGS__)
#define LOG_FATAL(...)  log_writevprintf(LOG_LEVEL_FATAL, LOG_FILENAME_ONLY(), __LINE__, __FUNCTION__, __VA_ARGS__)
#define TRACE(...)      log_writevprintf(LOG_LEVEL_TRACE, LOG_FILENAME_ONLY(), __LINE__, __FUNCTION__, __VA_ARGS__)

#endif // LOG_H
