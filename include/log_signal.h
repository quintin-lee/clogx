/**
 * @file log_signal.h
 * @brief Internal signal handler definitions.
 */

#ifndef LOG_SIGNAL_H
#define LOG_SIGNAL_H

#include "log.h"

/**
 * @brief Restore default or previous signal handlers.
 */
void log_restore_signal_handlers(void);

#endif /* LOG_SIGNAL_H */
