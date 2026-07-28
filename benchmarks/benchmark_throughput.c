#include "log.h"
#include <stdio.h>
#include <time.h>

int main(void) {
    log_config_t cfg = {0};
    cfg.format = "text";
    cfg.queue_size = 1024;
    log_config_set(&cfg);
    log_init(NULL);

    const int N = 100000;
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    for (int i = 0; i < N; i++) {
        LOG_INFO("benchmark throughput message");
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed = (ts_end.tv_sec - ts_start.tv_sec) + (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
    printf("throughput: %.0f msgs/sec\n", N / elapsed);

    log_destroy();
    return 0;
}
