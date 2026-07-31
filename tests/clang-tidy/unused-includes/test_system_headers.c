// Test: System headers behavior with IgnoreSystemHeaders option
// When IgnoreSystemHeaders=true, system headers should not produce warnings
// When IgnoreSystemHeaders=false (default), system headers should produce warnings
#include "log.h"    // Used - no warning
#include <stdio.h>  // Should warn if IgnoreSystemHeaders=false
#include <stdlib.h> // Used - no warning
#include <string.h> // Should warn if IgnoreSystemHeaders=false

int main(void)
{
    // Using stdlib.h
    int *p = malloc(sizeof(int));
    free(p);

    // Using log.h
    LOG_INFO("Test message");

    // NOT using stdio.h (no printf, fprintf, etc.)
    // NOT using string.h (no strlen, strcpy, etc.)

    return 0;
}
