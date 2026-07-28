#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log_formatter.h"
#include "log_record.h"

int main(void) {
    char fmt[1024];
    if (!fgets(fmt, sizeof(fmt), stdin)) {
        return 0;
    }

    log_formatter_init(fmt, "%Y-%m-%d %H:%M:%S");

    log_record_t rec = {.level = LOG_LEVEL_INFO,
                        .timestamp = 1600000000000000ULL,
                        .tid = 1234,
                        .pid = 5678,
                        .file = "fuzz_test.c",
                        .func = "fuzz_func",
                        .line = 42,
                        .module = "fuzz_mod",
                        .tag = "fuzz_tag",
                        .message = "fuzz_message_content"};

    char out[8192];
    log_formatter_format(&rec, out, sizeof(out));
    log_formatter_reset();

    return 0;
}
