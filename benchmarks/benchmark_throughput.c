/**
 * @file benchmark_throughput.c
 * @brief Raw logging throughput benchmark (500k messages).
 *
 * Logs 500,000 INFO-level messages in synchronous mode and reports
 * messages/sec and µs/message.  Run via `make bench`.
 */

#include "log.h"
#include <stdio.h>
#include <time.h>

int main(void)
{
    if (log_init("benchmarks/bench_config.yaml") != CLOG_OK) {
        fprintf(stderr, "Failed to initialize benchmark config\n");
        return 1;
    }

    const int       N = 500000;
    struct timespec ts_start, ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    for (int i = 0; i < N; i++) {
        LOG_INFO("benchmark throughput message %d", i);
    }

    log_flush();

    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    double elapsed = (double)(ts_end.tv_sec - ts_start.tv_sec) +
                     (double)(ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
    printf(
        "throughput: %.0f msgs/sec (total %d msgs in %.4f sec)\n", (double)N / elapsed, N, elapsed);

    log_destroy();
    return 0;
}
