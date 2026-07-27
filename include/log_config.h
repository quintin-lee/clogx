#ifndef LOG_CONFIG_H
#define LOG_CONFIG_H

#include <stdbool.h>
#include "log_record.h"
#include "log_sink.h"

// Configuration for the logging system
typedef struct {
    log_level_t level;              // Minimum log level to emit
    bool async;                     // Whether to use async mode
    int queue_size;                 // Size of async log queue
    bool color;                     // Whether to use colored output
    const char *format;             // Format string for log messages
    int console_enable;             // Enable console sink
    int file_enable;                // Enable file sink
    char file_path[256];            // File log path
    uint64_t file_max_size;         // Max file size before rotation
    int file_backups;               // Number of backup files
    int socket_enable;              // Enable socket sink
    char socket_host[256];          // Socket host
    int socket_port;                // Socket port
} log_config_t;

// Get global config instance
log_config_t *log_config_get(void);

// Initialize config from YAML file
int log_config_init(const char *yaml_path);

// Reload configuration from file
int log_config_reload(void);

// Set log level dynamically
int log_set_level(log_level_t level);

// Get current log level
log_level_t log_get_level(void);

// Check if a log message should be emitted based on level and config
static inline int log_should_emit(log_level_t level) {
    log_config_t *cfg = log_config_get();
    return level >= cfg->level;
}

#endif // LOG_CONFIG_H
