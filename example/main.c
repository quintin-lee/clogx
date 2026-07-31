/**
 * @file main.c
 * @brief Example program demonstrating clogx logging library usage.
 *
 * Shows both YAML-based and programmatic initialisation, including
 * async mode, custom sinks, file rotation, structured logging,
 * thread context (MDC), and Prometheus statistics.
 */
#include <stdio.h>
#include <string.h>
#include "log.h"
#include "log_config.h"

int main(void) {
    printf("=== CLog Example ===\n\n");

    /* ------------------------------------------------------------------
     * 1) YAML-based initialization (default: ./config.yaml).
     * ------------------------------------------------------------------ */
    clogx_errno_t err = log_init("./config.yaml");
    if (err != CLOG_OK) {
        fprintf(stderr, "log_init failed: %s\n", log_strerror(err));
        return 1;
    }
    printf("Logging initialized from YAML! (current level: %d)\n\n", log_get_level());

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

    /* ------------------------------------------------------------------
     * 2) Programmatic configuration via log_config_set().
     *    Demonstrates setting config directly without a YAML file.
     * ------------------------------------------------------------------ */
    printf("\n--- Switching to programmatic config ---\n");
    log_destroy();

    log_config_t cfg = {0};
    cfg.level = LOG_LEVEL_INFO;
    cfg.async = false;
    cfg.color = true;
    cfg.format = "[%time] [%level] [%module] %msg";
    cfg.time_format = "%Y-%m-%d %H:%M:%S";
    cfg.console_enable = true;
    cfg.console_stderr = false;
    cfg.file_enable = true;
    snprintf(cfg.file_path, sizeof(cfg.file_path), "logs/example_programmatic.log");
    cfg.file_max_size = 50 * 1024 * 1024;
    cfg.file_backups = 3;
    cfg.socket_enable = false;

    if (log_config_set(&cfg) != 0) {
        fprintf(stderr, "log_config_set failed\n");
        return 1;
    }

    /* Verify the config was applied. */
    const log_config_t *got = log_config_get();
    printf("Config applied via log_config_set:\n");
    printf("  level       = %d (INFO=%d)\n", got->level, LOG_LEVEL_INFO);
    printf("  async       = %s\n", got->async ? "true" : "false");
    printf("  color       = %s\n", got->color ? "true" : "false");
    printf("  format      = %s\n", got->format);
    printf("  time_format = %s\n", got->time_format);
    printf("  file_enable = %d\n", got->file_enable);
    printf("  file_path   = %s\n", got->file_path);
    printf("  file_max_size = %llu\n", (unsigned long long)got->file_max_size);
    printf("  file_backups  = %d\n", got->file_backups);

    /* Log messages with the new programmatic config. */
    log_set_module("programmatic");
    LOG_INFO("This message uses programmatic config");
    LOG_WARN("Programmatic config warning");
    LOG_ERROR("Programmatic config error");

    log_flush();
    printf("\nAll logs flushed.\n");

    /* ------------------------------------------------------------------
     * 3) Demonstrate error code description.
     * ------------------------------------------------------------------ */
    printf("\nError codes:\n");
    printf("  CLOG_ERR_CONFIG_OPEN: %s\n", log_strerror(CLOG_ERR_CONFIG_OPEN));
    printf("  CLOG_ERR_INIT_REENTRANT: %s\n", log_strerror(CLOG_ERR_INIT_REENTRANT));
    printf("  CLOG_ERR_QUEUE_FULL: %s\n", log_strerror(CLOG_ERR_QUEUE_FULL));

    log_destroy();
    printf("\nLogging destroyed.\n");
    printf("Example completed successfully!\n");
    return 0;
}
