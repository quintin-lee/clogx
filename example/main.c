#include <stdio.h>
#include "log.h"

int main(void) {
    printf("=== CLog Example ===\n\n");

    if (log_init("./config.yaml") != 0) {
        fprintf(stderr, "Failed to initialize logging\n");
        return 1;
    }
    printf("Logging initialized!\n\n");

    LOG_INFO("Server started");
    for (int i = 0; i < 3; i++) {
        LOG_INFO("Processing batch %d", i);
    }
    LOG_WARN("Disk space low: 85% used");
    LOG_ERROR("Failed to connect to database");

    log_flush();
    printf("All logs flushed.\n");

    log_destroy();
    printf("Logging destroyed.\n\n");

    printf("Example completed successfully!\n");
    return 0;
}
