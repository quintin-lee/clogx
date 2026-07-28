#include "log.h"
#include <stdio.h>
#include <time.h>

static void bench_mode(const char *label, log_config_t *cfg) {
    log_config_set(cfg);
    log_init(NULL);

    const int N = 50000;
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    for (int i = 0; i < N; i++) {
        LOG_INFO("async benchmark message");
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed = (ts_end.tv_sec - ts_start.tv_sec) + (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
    printf("mode=%s elapsed=%.4fs msgs/sec=%.0f\n", label, elapsed, N / elapsed);

    log_destroy();
}

int main(void) {
    printf("=== clogx benchmark: async vs sync ===\n");

    log_config_t sync_cfg = {0};
    sync_cfg.format = "text";
    sync_cfg.queue_size = 0;
    sync_cfg.async = false;
    bench_mode("sync", &sync_cfg);

    log_config_t async_cfg = {0};
    async_cfg.format = "text";
    async_cfg.queue_size = 1024;
    async_cfg.async = true;
    bench_mode("async", &async_cfg);

    return 0;
}
