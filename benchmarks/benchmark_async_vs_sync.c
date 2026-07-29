#include "log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static void bench_mode(const char *label, bool is_async) {
    if (log_init("benchmarks/bench_config.yaml") != CLOG_OK) {
        fprintf(stderr, "Failed to initialize benchmark config\n");
        return;
    }
    log_config_t *cfg = log_config_get();
    cfg->async = is_async;
    cfg->queue_size = is_async ? 8192 : 0;
    log_config_set(cfg);

    const int N = 200000;
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    for (int i = 0; i < N; i++) {
        LOG_INFO("benchmark async vs sync message %d", i);
    }

    log_flush();

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed = (double)(ts_end.tv_sec - ts_start.tv_sec) +
                     (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
    printf("mode=%-8s elapsed=%.4fs msgs/sec=%.0f\n", label, elapsed, (double)N / elapsed);

    log_destroy();
}

int main(void) {
    printf("=== clogx benchmark: async vs sync ===\n");
    bench_mode("sync", false);
    bench_mode("async", true);
    return 0;
}
