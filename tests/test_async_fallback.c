#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "log.h"
#include "log_async.h"

#define CONFIG_PATH "build/config_fallback_test.yaml"
#define LOG_PATH "logs/fallback_test.log"

static int write_config(void) {
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return -1;
    fprintf(f,
            "level: INFO\n"
            "async: true\n"
            "queue_size: 16\n"
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

static int callback_invoked = 0;

static void fallback_cb(void) {
    callback_invoked++;
}

int main(void) {
    remove(LOG_PATH);
    if (write_config() != 0 || log_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "init failed\n");
        return 1;
    }

    log_set_async_fallback_cb(fallback_cb);

    log_async_shutdown();

    LOG_INFO("after-shutdown");

    log_flush();
    log_destroy();

    if (callback_invoked == 0) {
        fprintf(stderr, "fallback callback was not invoked\n");
        return 1;
    }

    printf("async fallback test passed (callbacks=%d)\n", callback_invoked);
    return 0;
}
