#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "log.h"
#include "log_config.h"

#define CONFIG_PATH "build/config_invalid_test.yaml"
#define LOG_PATH "logs/invalid_config_test.log"

static int write_config(const char *extra) {
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f)
        return -1;
    fprintf(f,
            "log:\n"
            "  async: false\n"
            "  color: false\n"
            "  format: '[%%level] %%msg'\n"
            "  console_enable: false\n"
            "  file_enable: true\n"
            "  file_path: %s\n"
            "  max_size: 100MB\n"
            "  backups: 2\n"
            "  %s\n"
            "  socket_enable: false\n",
            LOG_PATH, extra);
    fclose(f);
    return 0;
}

int main(void) {
    remove(LOG_PATH);

    if (write_config("queue_size: abc") != 0)
        return 1;
    if (log_init(CONFIG_PATH) == 0) {
        fprintf(stderr, "Expected log_init to fail with invalid queue_size\n");
        return 1;
    }

    if (write_config("port: -1") != 0)
        return 1;
    if (log_init(CONFIG_PATH) == 0) {
        fprintf(stderr, "Expected log_init to fail with invalid port\n");
        return 1;
    }

    if (write_config("backups: -3") != 0)
        return 1;
    if (log_init(CONFIG_PATH) == 0) {
        fprintf(stderr, "Expected log_init to fail with invalid backups\n");
        return 1;
    }

    if (write_config("level: INVALID") != 0)
        return 1;
    if (log_init(CONFIG_PATH) == 0) {
        fprintf(stderr, "Expected log_init to fail with unknown level\n");
        return 1;
    }

    printf("invalid config test passed\n");
    return 0;
}
