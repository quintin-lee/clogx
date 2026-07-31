/**
 * @test test_mutex_guard_raii.c
 * @brief Test CLOG_MUTEXGUARDED RAII macro functionality.
 *
 * Verifies the macro acquires the mutex on entry and automatically releases
 * it when the code block exits via any control flow path.
 */

#include "clog_port.h"
#include <stdio.h>
#include <stdlib.h>

static clog_mutex_t g_mutex = CLOG_MUTEX_INITIALIZER;

/* Test 1: early return inside guard - macro must release on scope exit */
static int test_guard_early_return(void)
{
    CLOG_MUTEXGUARDED(&g_mutex, { return 42; });
    return -1; /* Should not reach */
}

/* Test 2: normal fall-through exit */
static int test_guard_normal_exit(void)
{
    int result = 0;
    CLOG_MUTEXGUARDED(&g_mutex, { result = 1; });
    /* Lock should be released here */
    return result;
}

/* Test 3: nested if-else with multiple exit paths */
static int test_guard_multiple_exits(void)
{
    CLOG_MUTEXGUARDED(&g_mutex, {
        if (1) {
            return 10;
        } else {
            return 20;
        }
    });
    return -1;
}

int main(void)
{
    printf("Testing CLOG_MUTEXGUARDED RAII macro...\n");

    if (clog_mutex_init(&g_mutex) != 0) {
        fprintf(stderr, "FAIL: mutex init\n");
        return 1;
    }

    int failures = 0;

    /* Test 1 */
    int r1 = test_guard_early_return();
    /* Re-acquire mutex to verify previous call released it */
    clog_mutex_lock(&g_mutex);
    clog_mutex_unlock(&g_mutex);
    if (r1 != 42) {
        fprintf(stderr, "FAIL: test_guard_early_return returned %d\n", r1);
        failures++;
    }

    /* Test 2 */
    int r2 = test_guard_normal_exit();
    clog_mutex_lock(&g_mutex);
    clog_mutex_unlock(&g_mutex);
    if (r2 != 1) {
        fprintf(stderr, "FAIL: test_guard_normal_exit returned %d\n", r2);
        failures++;
    }

    /* Test 3 */
    int r3 = test_guard_multiple_exits();
    clog_mutex_lock(&g_mutex);
    clog_mutex_unlock(&g_mutex);
    if (r3 != 10) {
        fprintf(stderr, "FAIL: test_guard_multiple_exits returned %d\n", r3);
        failures++;
    }

    clog_mutex_destroy(&g_mutex);

    if (failures == 0) {
        printf("All CLOG_MUTEXGUARDED tests PASSED\n");
        return 0;
    } else {
        printf("%d test(s) FAILED\n", failures);
        return 1;
    }
}
