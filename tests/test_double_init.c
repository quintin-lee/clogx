#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "log.h"

#define CONFIG_PATH "build/config_double_init_test.yaml"
#define LOG_PATH "logs/double_init_test.log"

static int write_config(void) {
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return -1;
    fprintf(f,
            "level: INFO\n"
            "async: false\n"
            "color: false\n"
            "format: [%%level] %%msg\n"
            "console_enable: false\n"
            "file_enable: true\n"
            "file_path: %s\n"
            "max_size: 100MB\n"
            "backups: 2\n"
            "socket_enable: false\n",
            LOG_PATH);
    fclose(f);
    return 0;
}

int main(void) {
    remove(LOG_PATH);
    if (write_config() != 0) return 1;

    if (log_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "First log_init failed\n");
        return 1;
    }

    if (log_init(CONFIG_PATH) == 0) {
        fprintf(stderr, "Second log_init should have failed\n");
        log_destroy();
        return 1;
    }

    log_destroy();

    if (log_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "log_init after destroy should succeed\n");
        return 1;
    }

    log_destroy();
    printf("double init test passed\n");
    return 0;
}
