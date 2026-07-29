#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "log_rate_limit.h"

int main(void) {
    log_init(NULL);

    log_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    log_get_stats(&stats);

    LOG_INFO("observability test message 1");
    LOG_INFO("observability test message 2");

    log_get_stats(&stats);
    if (stats.total_logged_count < 2) {
        fprintf(stderr, "expected at least 2 total_logged_count, got %llu\n",
                (unsigned long long)stats.total_logged_count);
        return 1;
    }

    log_destroy();
    printf("observability stats test passed\n");
    return 0;
}
