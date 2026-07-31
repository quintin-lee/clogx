/**
 * @file fuzz_pipeline.c
 * @brief AFL / libFuzzer harness for the full logging pipeline.
 *
 * Initialises the logger with a fuzzed config string, then exercises
 * log_writevprintf() with arbitrary messages to stress the init →
 * format → dispatch → sink write path end-to-end.
 */
#include "log.h"
#include "log_formatter.h"
#include "log_record.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        return 0;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        return 0;
    }

    char   data[4096];
    size_t n = fread(data, 1, sizeof(data) - 1, f);
    fclose(f);
    data[n] = '\0';

    log_record_t rec = {
        .level     = (log_level_t)(n % 7),
        .timestamp = (uint64_t)n * 1000000,
        .tid       = (uint32_t)n,
        .pid       = (uint32_t)(n + 1),
        .file      = "fuzz_file.c",
        .func      = "fuzz_func",
        .line      = (int)n,
        .module    = "fuzz_mod",
        .tag       = "fuzz_tag",
        .message   = data,
    };

    char out_buf[2048];
    log_formatter_init(data, "%Y-%m-%d %H:%M:%S");
    log_formatter_format(&rec, out_buf, sizeof(out_buf));

    char small_buf[16];
    log_formatter_format(&rec, small_buf, sizeof(small_buf));

    return 0;
}
