#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "log.h"

#define CONFIG_PATH "build/config_empty_test.yaml"

static int write_config(int console, int file, int socket) {
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return -1;
    fprintf(f,
            "level: INFO\n"
            "async: false\n"
            "color: false\n"
            "format: [%%level] %%msg\n"
            "console_enable: %s\n"
            "file_enable: %s\n"
            "%s"
            "max_size: 100MB\n"
            "backups: 2\n"
            "%s\n"
            "socket_enable: %s\n",
            console ? "true" : "false",
            file ? "true" : "false",
            file ? "file_path: logs/empty_sink_test.log\n" : "",
            socket ? "host: 127.0.0.1\nport: 1" : "socket_enable: false",
            socket ? "true" : "false");
    fclose(f);
    return 0;
}

int main(void) {
    if (write_config(0, 0, 0) != 0) return 1;
    if (log_init(CONFIG_PATH) == 0) {
        fprintf(stderr, "Expected failure with no sinks enabled\n");
        return 1;
    }

    printf("empty sink test passed\n");
    return 0;
}
