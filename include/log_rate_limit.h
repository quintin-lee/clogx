/**
 * @file log_rate_limit.h
 * @brief Token-bucket rate limiter for clogx log events.
 *
 * ## Design
 *
 * The rate limiter uses a token-bucket algorithm:
 * - A bucket holds up to @p burst tokens.
 * - Tokens are replenished at @p max_per_sec tokens per second.
 * - Each call to @ref log_rate_limit_allow consumes one token.
 * - When the bucket is empty, the event is suppressed (dropped).
 *
 * When rate limiting is disabled, @ref log_rate_limit_allow always
 * returns true via a fast-path that avoids acquiring the internal mutex.
 *
 * ## Thread Safety
 *
 * - @ref log_rate_limit_allow is thread-safe (mutex-protected bucket update
 *   when enabled; lock-free fast-path when disabled).
 * - @ref log_rate_limit_init is NOT thread-safe with respect to concurrent
 *   @ref log_rate_limit_allow calls — call it during initialisation or
 *   while the logger is quiesced.
 * - @ref log_rate_limit_reset is safe to call at any time (resets counters
 *   under the internal lock).
 *
 * @see log_config_t for the rate_limit_enable / max_per_sec / burst config keys.
 */

#ifndef LOG_RATE_LIMIT_H
#define LOG_RATE_LIMIT_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Configure the global rate limiter parameters.
 *
 * Resets the token bucket state. If @p enable is false, @ref log_rate_limit_allow
 * takes a fast-path that avoids any mutex acquisition.
 *
 * @param[in] enable       True to enable token-bucket rate limiting.
 * @param[in] max_per_sec  Token refill rate (events per second). Must be > 0
 *                         when @p enable is true.
 * @param[in] burst        Maximum accumulated tokens (bucket depth). Must be >=
 *                         @p max_per_sec for the refill to be meaningful.
 */
void log_rate_limit_init(bool enable, int max_per_sec, int burst);

/**
 * @brief Check whether a log event may proceed or should be suppressed.
 *
 * Consumes one token from the bucket if available. When the bucket is empty,
 * the event is suppressed and the suppressed counter is incremented.
 *
 * When rate limiting is disabled, always returns true without side effects
 * (no mutex, no atomic ops).
 *
 * @param[out] out_suppressed_count  If non-NULL and this event IS allowed,
 *                                   receives the number of events that were
 *                                   suppressed since the last allowed event.
 *                                   Not written if the event is suppressed.
 * @retval true   Event is allowed to proceed.
 * @retval false  Event is suppressed (rate limit exceeded).
 */
bool log_rate_limit_allow(uint64_t *out_suppressed_count);

/**
 * @brief Reset the rate limiter state to its initial condition.
 *
 * Refills the token bucket to @p burst and zeroes the suppressed counter.
 * Useful after a config reload or when recovering from a backpressure event.
 */
void log_rate_limit_reset(void);

/**
 * @brief Get the total number of log events suppressed since initialisation
 *        or the last reset.
 * @return Cumulative suppressed count.
 */
uint64_t log_rate_limit_get_total_suppressed(void);

/* ── Instance variants (same contract, scoped to a @ref logger_t) ── */

typedef struct logger_t logger_t;

/** @brief Instance variant of @ref log_rate_limit_init. */
void log_rate_limit_init_for(logger_t *logger, bool enable, int max_per_sec, int burst);
/** @brief Instance variant of @ref log_rate_limit_allow. */
bool log_rate_limit_allow_for(logger_t *logger, uint64_t *out_suppressed_count);
/** @brief Instance variant of @ref log_rate_limit_reset. */
void log_rate_limit_reset_for(logger_t *logger);
/** @brief Instance variant of @ref log_rate_limit_get_total_suppressed. */
uint64_t log_rate_limit_get_total_suppressed_for(logger_t *logger);

#endif /* LOG_RATE_LIMIT_H */
