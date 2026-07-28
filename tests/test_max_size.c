#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "log_config.h"

#define CONFIG_PATH "build/config_max_size_test.yaml"

static int write_config(const char *max_size) {
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f)
        return -1;
    fprintf(f,
            "log:\n"
            "  async: false\n"
            "  color: false\n"
            "  format: '%%msg'\n"
            "  console_enable: true\n"
            "  file_enable: false\n"
            "  max_size: %s\n"
            "  socket_enable: false\n",
            max_size);
    fclose(f);
    return 0;
}

static int expect_size(const char *text, uint64_t want) {
    if (write_config(text) != 0)
        return -1;
    if (log_config_init(CONFIG_PATH) != 0) {
        fprintf(stderr, "init failed for max_size=%s\n", text);
        return -1;
    }
    uint64_t got = log_config_get()->file_max_size;
    if (got != want) {
        fprintf(stderr, "max_size=%s expected %llu got %llu\n", text, (unsigned long long)want,
                (unsigned long long)got);
        return -1;
    }
    return 0;
}

int main(void) {
    if (expect_size("1024", 1024) != 0)
        return 1;
    if (expect_size("2KB", 2ULL * 1024) != 0)
        return 1;
    if (expect_size("3kb", 3ULL * 1024) != 0)
        return 1;
    if (expect_size("4K", 4ULL * 1024) != 0)
        return 1;
    if (expect_size("5MB", 5ULL * 1024 * 1024) != 0)
        return 1;
    if (expect_size("6m", 6ULL * 1024 * 1024) != 0)
        return 1;
    if (expect_size("1GB", 1ULL * 1024 * 1024 * 1024) != 0)
        return 1;
    if (expect_size("2G", 2ULL * 1024 * 1024 * 1024) != 0)
        return 1;

    if (write_config("12XB") != 0)
        return 1;
    if (log_config_init(CONFIG_PATH) == 0) {
        fprintf(stderr, "expected invalid max_size (12XB) to fail\n");
        return 1;
    }

    /* Overflow: 20000000000GB > UINT64_MAX */
    if (write_config("20000000000GB") != 0)
        return 1;
    if (log_config_init(CONFIG_PATH) == 0) {
        fprintf(stderr, "expected overflow max_size to fail\n");
        return 1;
    }

    printf("max_size parse test passed\n");
    return 0;
}
