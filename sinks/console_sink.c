#include <stdio.h>
#include <stdlib.h>
#include "log_sink.h"
#include "log_record.h"

typedef struct {
    FILE *stream;              // stdout or stderr
    int use_color;             // Whether to use color output
} console_sink_data_t;

// Console write function
static int console_write(log_sink_t *sink, const char *buf, size_t len) {
    console_sink_data_t *data = (console_sink_data_t *)sink->private_data;

    if (!data || !data->stream) return -1;

    // If colors are enabled, we might need to add escape sequences
    // For simplicity, just write the buffer directly
    size_t written = fwrite(buf, 1, len, data->stream);

    fflush(data->stream);

    return (int)written;
}

// Console flush function
static void console_flush(log_sink_t *sink) {
    console_sink_data_t *data = (console_sink_data_t *)sink->private_data;
    if (data && data->stream) {
        fflush(data->stream);
    }
}

// Console destroy function
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

// Create a console sink (writes to stdout)
log_sink_t *console_sink_create(void) {
    log_sink_t *sink = malloc(sizeof(log_sink_t));
    if (!sink) return NULL;

    console_sink_data_t *data = malloc(sizeof(console_sink_data_t));
    if (!data) {
        free(sink);
        return NULL;
    }

    data->stream = stdout;
    data->use_color = 1; // Enable color by default

    sink->write = console_write;
    sink->flush = console_flush;
    sink->destroy = console_destroy;
    sink->private_data = data;

    return sink;
}

// Create a console sink that writes to stderr instead
log_sink_t *console_sink_create_stderr(void) {
    log_sink_t *sink = malloc(sizeof(log_sink_t));
    if (!sink) return NULL;

    console_sink_data_t *data = malloc(sizeof(console_sink_data_t));
    if (!data) {
        free(sink);
        return NULL;
    }

    data->stream = stderr;
    data->use_color = 1;

    sink->write = console_write;
    sink->flush = console_flush;
    sink->destroy = console_destroy;
    sink->private_data = data;

    return sink;
}
