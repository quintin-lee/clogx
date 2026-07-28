#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "log.h"
#include "log_config.h"

int main(int argc, char **argv) {
    const char *filepath = "fuzz_tmp_config.yaml";
    if (argc > 1) {
        filepath = argv[1];
    } else {
        FILE *f = fopen(filepath, "wb");
        if (!f)
            return 0;
        char buf[1024];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) {
            fwrite(buf, 1, n, f);
        }
        fclose(f);
    }

    log_init(filepath);
    log_destroy();

    if (argc <= 1) {
        unlink(filepath);
    }
    return 0;
}
