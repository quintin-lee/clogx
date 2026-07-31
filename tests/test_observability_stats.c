/**
 * @file test_observability_stats.c
 * @brief Regression test: log_get_stats observability counters.
 */

#include "log.h"
#include "log_rate_limit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clog_port.h"
#include <assert.h>

#define THREAD_COUNT 4
#define LOGS_PER_THREAD 500

static void *stress_log_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < LOGS_PER_THREAD; i++) {
        LOG_INFO("concurrent log test %d", i);
    }
    return NULL;
}

static void test_concurrent_stats_increment(void)
{
    log_init(NULL);

    log_config_t cfg   = {0};
    cfg.level          = LOG_LEVEL_INFO;
    cfg.async          = false;
    cfg.console_enable = false;
    cfg.file_enable    = false;
    log_config_set(&cfg);

    log_stats_t init_stats = {0};
    log_get_stats(&init_stats);

    clog_thread_t threads[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; i++) {
        clog_thread_create(&threads[i], stress_log_worker, NULL);
    }
    for (int i = 0; i < THREAD_COUNT; i++) {
        clog_thread_join(threads[i]);
    }

    log_stats_t stats = {0};
    log_get_stats(&stats);

    log_destroy();

    uint64_t added = stats.total_logged_count - init_stats.total_logged_count;
    if (added != (uint64_t)(THREAD_COUNT * LOGS_PER_THREAD)) {
        fprintf(stderr,
                "Expected %llu total_logged_count added, got %llu\n",
                (unsigned long long)(THREAD_COUNT * LOGS_PER_THREAD),
                (unsigned long long)added);
        exit(1);
    }
    printf("test_concurrent_stats_increment PASSED (%llu logs added)\n",
           (unsigned long long)added);
}

int main(void)
{
    log_init(NULL);

    log_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    log_get_stats(&stats);

    LOG_INFO("observability test message 1");
    LOG_INFO("observability test message 2");

    log_get_stats(&stats);
    if (stats.total_logged_count < 2) {
        fprintf(stderr,
                "expected at least 2 total_logged_count, got %llu\n",
                (unsigned long long)stats.total_logged_count);
        return 1;
    }

    log_destroy();
    printf("observability stats test passed\n");

    test_concurrent_stats_increment();
    return 0;
}
