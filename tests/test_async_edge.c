/**
 * @file test_async_edge.c
 * @brief Edge-case tests for async atfork and error paths.
 */

#include "clog_port.h"
#include "log.h"
#include "log_async.h"
#include "log_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

static void test_async_atfork_child_null_logger(void)
{
    log_async_atfork_child_for(NULL);
    printf("test_async_atfork_child_null_logger passed\n");
}

static void test_async_atfork_child_not_running(void)
{
    logger_t logger;
    memset(&logger, 0, sizeof(logger));
    logger.async_running = 0;
    log_async_atfork_child_for(&logger);
    assert(logger.async_running == 0);
    printf("test_async_atfork_child_not_running passed\n");
}

static void test_async_atfork_child_null_queue(void)
{
    logger_t logger;
    memset(&logger, 0, sizeof(logger));
    logger.async_running = 1;
    log_async_atfork_child_for(&logger);
    assert(logger.async_running == 1);
    printf("test_async_atfork_child_null_queue passed\n");
}

int main(void)
{
    test_async_atfork_child_null_logger();
    test_async_atfork_child_not_running();
    test_async_atfork_child_null_queue();
    printf("all async edge tests passed!\n");
    return 0;
}
