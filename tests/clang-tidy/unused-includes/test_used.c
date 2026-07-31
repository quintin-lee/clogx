// Test: All includes are used - should produce NO warnings
#include <stdio.h>
#include <stdlib.h>
#include "log.h"

int main(void) {
    // Using stdio.h
    printf("Hello\n");

    // Using stdlib.h
    int *p = malloc(sizeof(int));
    free(p);

    // Using log.h
    LOG_INFO("Test message");

    return 0;
}
