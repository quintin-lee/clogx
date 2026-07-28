#include <stdio.h>
#include "log.h"

int main(void) {
    printf("=== CLog Example ===\n\n");

    /* Initialize from config file (default: ./config.yaml).
     * See config.yaml in the repo root for available keys. */
    clogx_errno_t err = log_init("./config.yaml");
    if (err != CLOG_OK) {
        fprintf(stderr, "log_init failed: %s\n", log_strerror(err));
        return 1;
    }
    printf("Logging initialized! (current level: %d)\n\n", log_get_level());

    /* Tag all messages with a module name — useful for filtering. */
    log_set_module("server");

    /* All six log levels. */
    LOG_TRACE("Verbose trace for deep debugging");
    LOG_DEBUG("Debugging connection pool state");
    LOG_INFO("Server started on port %d", 8080);
    LOG_WARN("Disk space low: 85%% used");
    LOG_ERROR("Failed to connect to database: %s", "timeout");
    LOG_FATAL("Out of memory — aborting transaction");

    /* Runtime level change: raise threshold so only WARN+ appear. */
    printf("\n--- Raising threshold to WARN ---\n");
    log_set_level(LOG_LEVEL_WARN);
    printf("Current level: %d (WARN=%d)\n", log_get_level(), LOG_LEVEL_WARN);

    /* These two should NOT appear (below WARN). */
    LOG_DEBUG("debug — filtered out");
    LOG_INFO("info — filtered out");

    /* These two SHOULD appear. */
    LOG_WARN("warning — visible");
    LOG_ERROR("error — visible");

    /* Switch module for context. */
    log_set_module("database");
    LOG_INFO("Query executed in 42ms — filtered out");
    LOG_ERROR("Connection pool exhausted — visible");

    /* Reset back to original threshold. */
    log_set_level(LOG_LEVEL_TRACE);
    LOG_DEBUG("debug — visible again");

    log_flush();
    printf("\nAll logs flushed.\n\n");

    /* Demonstrate error code description. */
    printf("Error codes:\n");
    printf("  CLOG_ERR_CONFIG_OPEN: %s\n", log_strerror(CLOG_ERR_CONFIG_OPEN));
    printf("  CLOG_ERR_INIT_REENTRANT: %s\n", log_strerror(CLOG_ERR_INIT_REENTRANT));
    printf("  CLOG_ERR_QUEUE_FULL: %s\n", log_strerror(CLOG_ERR_QUEUE_FULL));

    log_destroy();
    printf("\nLogging destroyed.\n");
    printf("Example completed successfully!\n");
    return 0;
}
