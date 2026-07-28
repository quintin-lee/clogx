#include <stdio.h>
#include <string.h>
#include "clog_port.h"
#include "log_config.h"

#define CONFIG_PATH "build/config_verify_test.yaml"

static int write_config(void) {
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f)
        return -1;
    fprintf(f, "log:\n"
               "  async: false\n"
               "  queue_size: 4096\n"
               "  color: true\n"
               "  format: '[%%time] [%%level] %%msg'\n"
               "  console_enable: true\n"
               "  file_enable: true\n"
               "  file_path: logs/verify_config.log\n"
               "  max_size: 50MB\n"
               "  backups: 3\n"
               "  socket_enable: false\n");
    fclose(f);
    return 0;
}

int main(void) {
    if (write_config() != 0) {
        fprintf(stderr, "failed to write config\n");
        return 1;
    }

    if (log_config_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "log_config_init failed\n");
        return 1;
    }

    log_config_t *cfg = log_config_get();
    if (cfg->level != LOG_LEVEL_INFO || cfg->async || !cfg->color || !cfg->console_enable ||
        !cfg->file_enable || cfg->file_backups != 3 || cfg->queue_size != 4096 ||
        strcmp(cfg->file_path, "logs/verify_config.log") != 0 ||
        cfg->file_max_size != 50ULL * 1024 * 1024) {
        fprintf(stderr,
                "config mismatch: level=%d async=%d color=%d console=%d file=%d "
                "path='%s' max=%llu backups=%d queue=%d\n",
                cfg->level, cfg->async, cfg->color, cfg->console_enable, cfg->file_enable,
                cfg->file_path, (unsigned long long)cfg->file_max_size, cfg->file_backups,
                cfg->queue_size);
        return 1;
    }

    printf("verify_config test passed\n");
    return 0;
}
