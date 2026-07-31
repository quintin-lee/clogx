// Test: Macro from header is used - should produce NO warnings
#include "log.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    // Using assert macro from assert.h
    int x = 42;
    assert(x > 0);

    // Using printf from stdio.h
    printf("x = %d\n", x);

    // Using malloc from stdlib.h
    int *p = malloc(sizeof(int));
    free(p);

    // Using LOG_INFO from log.h
    LOG_INFO("Test message");

    return 0;
}
