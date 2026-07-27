#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log_config.h"

extern log_config_t *log_config_get(void);
extern int log_config_init(const char *yaml_path);

int main(void) {
    printf("=== Config Verification Test ===\n\n");

    int ret = log_config_init("./config.yaml");
    printf("log_config_init returned: %d\n\n", ret);

    log_config_t *cfg = log_config_get();
    printf("Loaded configuration:\n");
    printf("  level: %d (INFO=2, DEBUG=1, TRACE=0)\n", cfg->level);
    printf("  async: %d\n", cfg->async);
    printf("  color: %d\n", cfg->color);
    printf("  console_enable: %d\n", cfg->console_enable);
    printf("  file_enable: %d\n", cfg->file_enable);
    printf("  file_path: '%s'\n", cfg->file_path);
    printf("  file_max_size: %lu bytes\n", cfg->file_max_size);
    printf("  file_backups: %d\n", cfg->file_backups);
    printf("  socket_enable: %d\n", cfg->socket_enable);

    printf("\n=== Expected: file_enable=1, file_path='logs/server.log' ===\n");

    if (cfg->file_enable == 1) {
        printf("✓ file_enable is CORRECTLY set to 1\n");
    } else {
        printf("✗ file_enable is WRONG (expected 1, got %d)\n", cfg->file_enable);
    }

    if (strcmp(cfg->file_path, "logs/server.log") == 0) {
        printf("✓ file_path is CORRECTLY set to 'logs/server.log'\n");
    } else {
        printf("✗ file_path is WRONG (expected 'logs/server.log', got '%s')\n", cfg->file_path);
    }

    return 0;
}
