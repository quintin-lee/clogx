#include <stdio.h>
#include <string.h>
#include "clog_port.h"
#include "log.h"

#define CONFIG_PATH "build/config_add_sink_test.yaml"
#define BASE_LOG "logs/add_sink_base.log"
#define EXTRA_LOG "logs/add_sink_extra.log"

static int write_config(void) {
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f)
        return -1;
    fprintf(f,
            "log:\n"
            "  async: false\n"
            "  color: false\n"
            "  format: '%%msg'\n"
            "  console_enable: false\n"
            "  file_enable: true\n"
            "  file_path: %s\n"
            "  socket_enable: false\n",
            BASE_LOG);
    fclose(f);
    return 0;
}

static int file_contains(const char *path, const char *needle) {
    char buf[1024];
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f)
        return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

int main(void) {
    log_sink_t *extra;

    remove(BASE_LOG);
    remove(EXTRA_LOG);
    if (write_config() != 0 || log_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "init failed\n");
        return 1;
    }

    extra = file_sink_create(EXTRA_LOG, 0, 0);
    if (!extra || log_add_sink(extra) != CLOG_OK) {
        fprintf(stderr, "log_add_sink failed\n");
        log_destroy();
        return 1;
    }

    LOG_INFO("dual-sink-line");
    log_flush();

    if (!file_contains(BASE_LOG, "dual-sink-line") || !file_contains(EXTRA_LOG, "dual-sink-line")) {
        fprintf(stderr, "expected line in both sinks\n");
        log_destroy();
        return 1;
    }

    if (log_remove_sink(extra) != CLOG_OK) {
        fprintf(stderr, "log_remove_sink failed\n");
        log_destroy();
        return 1;
    }
    extra->destroy(extra);

    LOG_INFO("base-only-line");
    log_flush();
    log_destroy();

    if (!file_contains(BASE_LOG, "base-only-line")) {
        fprintf(stderr, "base sink missing post-remove line\n");
        return 1;
    }
    if (file_contains(EXTRA_LOG, "base-only-line")) {
        fprintf(stderr, "removed sink still received logs\n");
        return 1;
    }

    printf("log_add_sink test passed\n");
    return 0;
}
