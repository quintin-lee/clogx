// Test: Direct unused include - should produce warning for stdio.h
#include "log.h"
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    // NOT using stdio.h (no printf, fprintf, etc.)

    // Using stdlib.h
    int *p = malloc(sizeof(int));
    free(p);

    // Using log.h
    LOG_INFO("Test message");

    return 0;
}
