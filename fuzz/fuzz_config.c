#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "log.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        return 0;
    }

    log_init(argv[1]);
    log_destroy();
    return 0;
}
