/**
 * @file console_sink.c
 * @brief Console sinks (stdout/stderr); color preference queried by dispatcher.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "log_sink.h"
#include "log_record.h"

typedef struct {
    FILE *stream;
    int use_color;
} console_sink_data_t;

static int console_write(log_sink_t *sink, const char *buf, size_t len) {
    console_sink_data_t *data = (console_sink_data_t *)sink->private_data;

    if (!data || !data->stream) return -1;

    size_t written = fwrite(buf, 1, len, data->stream);
    fflush(data->stream);

    return (int)written;
}

static void console_flush(log_sink_t *sink) {
    console_sink_data_t *data = (console_sink_data_t *)sink->private_data;
    if (data && data->stream) {
        fflush(data->stream);
    }
}

static void console_destroy(log_sink_t *sink) {
    console_sink_data_t *data = (console_sink_data_t *)sink->private_data;
    if (data) {
        if (data->stream != stdout && data->stream != stderr) {
            fclose(data->stream);
        }
        free(data);
    }
    free(sink);
}

log_sink_t *console_sink_create(bool use_color) {
    log_sink_t *sink = malloc(sizeof(log_sink_t));
    if (!sink) return NULL;

    console_sink_data_t *data = malloc(sizeof(console_sink_data_t));
    if (!data) {
        free(sink);
        return NULL;
    }

    data->stream = stdout;
    data->use_color = use_color;

    sink->write = console_write;
    sink->flush = console_flush;
    sink->destroy = console_destroy;
    sink->private_data = data;

    return sink;
}

log_sink_t *console_sink_create_stderr(bool use_color) {
    log_sink_t *sink = malloc(sizeof(log_sink_t));
    if (!sink) return NULL;

    console_sink_data_t *data = malloc(sizeof(console_sink_data_t));
    if (!data) {
        free(sink);
        return NULL;
    }

    data->stream = stderr;
    data->use_color = use_color;

    sink->write = console_write;
    sink->flush = console_flush;
    sink->destroy = console_destroy;
    sink->private_data = data;

    return sink;
}

bool console_sink_is_color_enabled(log_sink_t *sink) {
    if (!sink || sink->write != console_write) return false;
    console_sink_data_t *data = (console_sink_data_t *)sink->private_data;
    return data && data->use_color;
}
