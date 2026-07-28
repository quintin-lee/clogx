/**
 * @file rate_limit.c
 * @brief Token bucket rate limiter implementation using CLOCK_MONOTONIC.
 */

#define _POSIX_C_SOURCE 200809L
#include "log_rate_limit.h"
#include <pthread.h>
#include <time.h>

static volatile bool g_enabled = false;
static double g_tokens = 0.0;
static double g_max_tokens = 0.0;
static double g_fill_rate = 0.0; /* tokens per microsecond */
static uint64_t g_last_update_us = 0;
static uint64_t g_suppressed_count = 0;
static pthread_mutex_t g_rate_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t get_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

void log_rate_limit_init(bool enable, int max_per_sec, int burst) {
    pthread_mutex_lock(&g_rate_mutex);
    g_enabled = enable;
    if (enable && max_per_sec > 0 && burst > 0) {
        g_max_tokens = (double)burst;
        g_tokens = (double)burst;
        g_fill_rate = (double)max_per_sec / 1000000.0;
        g_last_update_us = get_now_us();
        g_suppressed_count = 0;
    } else {
        g_enabled = false;
    }
    pthread_mutex_unlock(&g_rate_mutex);
}

bool log_rate_limit_allow(uint64_t *out_suppressed_count) {
    if (out_suppressed_count) {
        *out_suppressed_count = 0;
    }

    if (!g_enabled) {
        return true;
    }

    pthread_mutex_lock(&g_rate_mutex);
    if (!g_enabled) {
        pthread_mutex_unlock(&g_rate_mutex);
        return true;
    }

    uint64_t now_us = get_now_us();
    if (now_us > g_last_update_us) {
        double elapsed_us = (double)(now_us - g_last_update_us);
        g_tokens += elapsed_us * g_fill_rate;
        if (g_tokens > g_max_tokens) {
            g_tokens = g_max_tokens;
        }
        g_last_update_us = now_us;
    }

    if (g_tokens >= 1.0) {
        g_tokens -= 1.0;
        if (g_suppressed_count > 0) {
            if (out_suppressed_count) {
                *out_suppressed_count = g_suppressed_count;
            }
            g_suppressed_count = 0;
        }
        pthread_mutex_unlock(&g_rate_mutex);
        return true;
    } else {
        g_suppressed_count++;
        pthread_mutex_unlock(&g_rate_mutex);
        return false;
    }
}

void log_rate_limit_reset(void) {
    pthread_mutex_lock(&g_rate_mutex);
    g_enabled = false;
    g_tokens = 0.0;
    g_max_tokens = 0.0;
    g_fill_rate = 0.0;
    g_last_update_us = 0;
    g_suppressed_count = 0;
    pthread_mutex_unlock(&g_rate_mutex);
}
