#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log_formatter.h"
#include "log_record.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        return 0;
    }

    FILE *f = fopen(argv[1], "r");
    if (!f) {
        return 0;
    }

    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    log_formatter_init(buf, "%Y-%m-%d %H:%M:%S");

    log_record_t rec = {.level = LOG_LEVEL_INFO,
                        .timestamp = 1700000000000000ULL,
                        .tid = 1001,
                        .pid = 2002,
                        .file = "test.c",
                        .func = "fuzz_func",
                        .line = 42,
                        .module = "fuzz_mod",
                        .tag = "fuzz_tag",
                        .message = "fuzz test message"};

    char out_buf[4096];
    log_formatter_format(&rec, out_buf, sizeof(out_buf));

    return 0;
}
