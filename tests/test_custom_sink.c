/**
 * @file test_custom_sink.c
 * @brief Regression test: custom sink API (log_sink_create_custom with callbacks).
 */

#include "log.h"
#include "log_sink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int  write_count;
    int  flush_count;
    int  destroy_count;
    char last_msg[256];
} test_custom_data_t;

static int my_custom_write(log_sink_t *sink, const char *buf, size_t len)
{
    test_custom_data_t *data = (test_custom_data_t *)custom_sink_get_private_data(sink);
    if (!data) {
        return -1;
    }
    data->write_count++;
    snprintf(data->last_msg,
             sizeof(data->last_msg),
             "%.*s",
             (int)(len < sizeof(data->last_msg) ? len : sizeof(data->last_msg) - 1),
             buf);
    return (int)len;
}

static void my_custom_flush(log_sink_t *sink)
{
    test_custom_data_t *data = (test_custom_data_t *)custom_sink_get_private_data(sink);
    if (data) {
        data->flush_count++;
    }
}

static void my_custom_destroy(log_sink_t *sink)
{
    test_custom_data_t *data = (test_custom_data_t *)custom_sink_get_private_data(sink);
    if (data) {
        data->destroy_count++;
    }
}

int main(void)
{
    test_custom_data_t user_data = {0};

    log_sink_t *custom_sink =
        custom_sink_create(my_custom_write, my_custom_flush, my_custom_destroy, &user_data);
    if (!custom_sink) {
        fprintf(stderr, "custom_sink_create failed\n");
        return 1;
    }

    if (log_init(NULL) != 0) {
        fprintf(stderr, "log_init failed\n");
        return 1;
    }

    if (log_add_sink(custom_sink) != 0) {
        fprintf(stderr, "log_add_sink custom_sink failed\n");
        return 1;
    }

    LOG_INFO("hello custom sink");
    log_flush();

    if (user_data.write_count <= 0) {
        fprintf(stderr, "custom write count is 0\n");
        return 1;
    }
    if (user_data.flush_count <= 0) {
        fprintf(stderr, "custom flush count is 0\n");
        return 1;
    }

    log_destroy();

    if (user_data.destroy_count <= 0) {
        fprintf(stderr, "custom destroy count is 0\n");
        return 1;
    }

    printf("custom sink test passed\n");
    return 0;
}
