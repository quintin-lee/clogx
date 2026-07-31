/**
 * @file fuzz_config.c
 * @brief AFL / libFuzzer harness for the YAML config parser.
 *
 * Feeds arbitrary file contents to log_parse_config_file() to
 * discover parser crashes, assertion failures, and memory errors.
 */
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        return 0;
    }

    log_init(argv[1]);
    log_destroy();
    return 0;
}
