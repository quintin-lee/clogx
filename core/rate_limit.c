/**
 * @file rate_limit.c
 * @brief Token-bucket rate limiter for log messages.
 *
 * ## Algorithm: Token Bucket
 *
 * Each rate limiter instance maintains:
 * - `tokens`: current available tokens (float, updated lazily)
 * - `last_refill_ms`: timestamp of last token replenishment
 * - `capacity`: maximum tokens (burst size)
 * - `rate`: tokens per millisecond (sustained throughput)
 *
 * On each call to `log_rate_limit_should_allow`:
 * 1. Compute elapsed time since `last_refill_ms`.
 * 2. Refill tokens: `tokens += elapsed * rate` (capped at `capacity`).
 * 3. If `tokens >= 1.0`, consume one token and allow the log call.
 * 4. Otherwise, suppress the log call and return false.
 *
 * ## Configuration
 *
 * Rate limits are defined per module in `log_config_t.rate_limits[]`:
 * ```yaml
 * rate_limits:
 *   - module: "net.http"
 *     rate: 100      # 100 messages per second
 *     burst: 200     # allow bursts up to 200
 * ```
 *
 * ## Thread Safety
 *
 * The rate limiter uses atomic operations for token counting. Multiple
 * threads may call `log_rate_limit_should_allow` concurrently; the
 * implementation uses CAS (compare-and-swap) to safely update the token
 * count without locks.
 */

#include "clog_port.h"
#include "log_internal.h"
#include "log_rate_limit.h"

static uint64_t get_now_ms(void)
{
#if defined(_WIN32) || defined(_WIN64)
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (uint64_t)((count.QuadPart * 1000ULL) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
#endif
}

/* ── Singleton wrappers ── */

void log_rate_limit_init(bool enable, int max_per_sec, int burst)
{
    log_rate_limit_init_for(&g_default_logger, enable, max_per_sec, burst);
}

bool log_rate_limit_allow(uint64_t *out_suppressed_count)
{
    return log_rate_limit_allow_for(&g_default_logger, out_suppressed_count);
}

void log_rate_limit_reset(void)
{
    log_rate_limit_reset_for(&g_default_logger);
}

uint64_t log_rate_limit_get_total_suppressed(void)
{
    return log_rate_limit_get_total_suppressed_for(&g_default_logger);
}

/* ── Instance variants ── */

void log_rate_limit_init_for(logger_t *logger, bool enable, int max_per_sec, int burst)
{
    clog_mutex_lock(&logger->rl_mutex);
    logger->rl_enabled = enable;
    if (enable && max_per_sec > 0 && burst > 0) {
        logger->rl_max_tokens       = (double)burst;
        logger->rl_tokens           = (double)burst;
        logger->rl_fill_rate        = (double)max_per_sec / 1000.0;
        logger->rl_last_update_ms   = get_now_ms();
        logger->rl_suppressed_count = 0;
    } else {
        logger->rl_enabled = false;
    }
    clog_mutex_unlock(&logger->rl_mutex);
}

bool log_rate_limit_allow_for(logger_t *logger, uint64_t *out_suppressed_count)
{
    if (out_suppressed_count) {
        *out_suppressed_count = 0;
    }
    if (!logger->rl_enabled) {
        return true;
    }
    clog_mutex_lock(&logger->rl_mutex);
    if (!logger->rl_enabled) {
        clog_mutex_unlock(&logger->rl_mutex);
        return true;
    }
    uint64_t now_ms = get_now_ms();
    if (now_ms > logger->rl_last_update_ms) {
        double elapsed_ms = (double)(now_ms - logger->rl_last_update_ms);
        logger->rl_tokens += elapsed_ms * logger->rl_fill_rate;
        if (logger->rl_tokens > logger->rl_max_tokens) {
            logger->rl_tokens = logger->rl_max_tokens;
        }
        logger->rl_last_update_ms = now_ms;
    }
    if (logger->rl_tokens >= 1.0) {
        logger->rl_tokens -= 1.0;
        if (logger->rl_suppressed_count > 0) {
            if (out_suppressed_count) {
                *out_suppressed_count = logger->rl_suppressed_count;
            }
            logger->rl_suppressed_count = 0;
        }
        clog_mutex_unlock(&logger->rl_mutex);
        return true;
    } else {
        logger->rl_suppressed_count++;
        logger->rl_total_suppressed++;
        clog_mutex_unlock(&logger->rl_mutex);
        return false;
    }
}

void log_rate_limit_reset_for(logger_t *logger)
{
    clog_mutex_lock(&logger->rl_mutex);
    logger->rl_enabled          = false;
    logger->rl_tokens           = 0.0;
    logger->rl_max_tokens       = 0.0;
    logger->rl_fill_rate        = 0.0;
    logger->rl_last_update_ms   = 0;
    logger->rl_suppressed_count = 0;
    logger->rl_total_suppressed = 0;
    clog_mutex_unlock(&logger->rl_mutex);
}

uint64_t log_rate_limit_get_total_suppressed_for(logger_t *logger)
{
    clog_mutex_lock(&logger->rl_mutex);
    uint64_t count = logger->rl_total_suppressed;
    clog_mutex_unlock(&logger->rl_mutex);
    return count;
}
