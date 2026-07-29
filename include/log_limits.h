/**
 * @file log_limits.h
 * @brief Configurable maximum buffer size constants.
 */

#ifndef LOG_LIMITS_H
#define LOG_LIMITS_H

#ifndef CLOG_MAX_MESSAGE_SIZE
#define CLOG_MAX_MESSAGE_SIZE 4096
#endif

#ifndef CLOG_MAX_FORMATTED_SIZE
#define CLOG_MAX_FORMATTED_SIZE 8192
#endif

#ifndef CLOG_MAX_COLORED_SIZE
#define CLOG_MAX_COLORED_SIZE 16384
#endif

#ifndef CLOG_MAX_FORMAT_SIZE
#define CLOG_MAX_FORMAT_SIZE 1024
#endif

#ifndef CLOG_MAX_PATH_SIZE
#define CLOG_MAX_PATH_SIZE 512
#endif

#ifndef CLOG_MAX_PLUGINS
#define CLOG_MAX_PLUGINS 8
#endif

#ifndef CLOG_MAX_PLUGIN_CONFIG_SIZE
#define CLOG_MAX_PLUGIN_CONFIG_SIZE 4096
#endif

#endif /* LOG_LIMITS_H */
