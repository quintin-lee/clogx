#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"

int main(void) {
    if (log_init(NULL) != 0) {
        fprintf(stderr, "log_init failed\n");
        return 1;
    }

    log_set_thread_context("trace_id", "req-12345");
    log_set_thread_context("user_id", "usr-8888");

    const char *trace = log_get_thread_context("trace_id");
    const char *user = log_get_thread_context("user_id");

    if (!trace || strcmp(trace, "req-12345") != 0) {
        fprintf(stderr, "expected trace_id req-12345, got %s\n", trace ? trace : "NULL");
        return 1;
    }
    if (!user || strcmp(user, "usr-8888") != 0) {
        fprintf(stderr, "expected user_id usr-8888, got %s\n", user ? user : "NULL");
        return 1;
    }

    log_set_thread_context("user_id", NULL);
    if (log_get_thread_context("user_id") != NULL) {
        fprintf(stderr, "expected user_id to be cleared\n");
        return 1;
    }

    log_clear_thread_context();
    if (log_get_thread_context("trace_id") != NULL) {
        fprintf(stderr, "expected trace_id to be cleared after log_clear_thread_context\n");
        return 1;
    }

    log_destroy();
    printf("thread context MDC test passed\n");
    return 0;
}
