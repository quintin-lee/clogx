#include <stdio.h>
#include "log.h"
int main(void) {
    printf("=== Full Pipeline Test ===\n");
    int ret = log_init("./config.yaml");
    printf("log_init returned: %d\n", ret);
    
    log_config_t *cfg = log_config_get();
    printf("file_enable=%d, file_path='%s'\n", cfg->file_enable, cfg->file_path);
    
    if (ret == 0) {
        LOG_INFO("Server started");
        LOG_WARN("Disk space low");
        LOG_ERROR("Connection failed");
        log_flush();
    }
    
    // Check file size
    FILE *f = fopen("logs/server.log", "r");
    if (f) { fseek(f, 0, SEEK_END); long sz = ftell(f); fclose(f); printf("Log file size: %ld bytes\n", sz); }
    
    log_destroy();
    printf("Test complete.\n");
    return 0;
}
