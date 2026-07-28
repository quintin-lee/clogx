/**
 * @file log_rate_limit.h
 * @brief Token bucket rate limiter for clogx log events.
 */

#ifndef LOG_RATE_LIMIT_H
#define LOG_RATE_LIMIT_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Initialize or update rate limiter settings.
 *
 * @param enable       True to enable rate limiting.
 * @param max_per_sec  Maximum log events allowed per second.
 * @param burst        Maximum burst capacity (token bucket capacity).
 */
void log_rate_limit_init(bool enable, int max_per_sec, int burst);

/**
 * @brief Check if a log event is allowed by the rate limiter.
 *
 * @param[out] out_suppressed_count If non-NULL and log is allowed, receives the count
 *                                  of suppressed log messages since last allowed log.
 * @return true if log is allowed, false if suppressed due to rate limiting.
 */
bool log_rate_limit_allow(uint64_t *out_suppressed_count);

/**
 * @brief Reset the rate limiter state and counters.
 */
void log_rate_limit_reset(void);

#endif /* LOG_RATE_LIMIT_H */
