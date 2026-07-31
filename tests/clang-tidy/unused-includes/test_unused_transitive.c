// Test: Transitive unused include - should produce warning for stdio.h
// stdlib.h is used, but stdio.h is not (even though it's included)
#include <stdio.h>
#include <stdlib.h>
#include "log.h"

// Custom header that includes stdio.h (for testing transitive)
#include "test_helper.h"

int main(void) {
    // Using stdlib.h
    int *p = malloc(sizeof(int));
    free(p);

    // Using log.h
    LOG_INFO("Test message");

    // NOT using stdio.h directly
    // NOT using anything from test_helper.h

    return 0;
}
