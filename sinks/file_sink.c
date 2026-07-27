#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "log_sink.h"
#include "log_record.h"
#include "rotate.h"

typedef struct {
    FILE *file;
    char *path;
    uint64_t max_size;
    uint64_t current_size;
    int backups;
} file_sink_data_t;

static int file_write(log_sink_t *sink, const char *buf, size_t len) {
    file_sink_data_t *data = (file_sink_data_t *)sink->private_data;
    if (!data || !data->file || !buf) return -1;

    size_t written = fwrite(buf, 1, len, data->file);
    if (written != len) return -1;

    data->current_size += (uint64_t)written;

    if (data->max_size > 0 && data->current_size >= data->max_size) {
        fflush(data->file);
        fclose(data->file);
        data->file = NULL;

        file_rotate_file(data->path, data->backups);

        data->file = fopen(data->path, "a");
        if (!data->file) return -1;
        data->current_size = 0;
    }

    return (int)written;
}

static void file_flush(log_sink_t *sink) {
    file_sink_data_t *data = (file_sink_data_t *)sink->private_data;
    if (data && data->file) {
        fflush(data->file);
    }
}

static void file_destroy(log_sink_t *sink) {
    file_sink_data_t *data = (file_sink_data_t *)sink->private_data;
    if (data) {
        if (data->file) {
            fclose(data->file);
        }
        free(data->path);
        free(data);
    }
    free(sink);
}

log_sink_t *file_sink_create(const char *path, uint64_t max_size, int backups) {
    if (!path || strlen(path) == 0) return NULL;

    log_sink_t *sink = malloc(sizeof(log_sink_t));
    if (!sink) return NULL;

    file_sink_data_t *data = malloc(sizeof(file_sink_data_t));
    if (!data) {
        free(sink);
        return NULL;
    }

    data->path = strdup(path);
    if (!data->path) {
        free(data);
        free(sink);
        return NULL;
    }

    data->max_size = max_size;
    data->backups = backups;
    data->current_size = 0;
    data->file = fopen(path, "a");
    if (!data->file) {
        perror("Failed to open file for logging");
        free(data->path);
        free(data);
        free(sink);
        return NULL;
    }

    if (fseek(data->file, 0, SEEK_END) == 0) {
        long sz = ftell(data->file);
        if (sz > 0) {
            data->current_size = (uint64_t)sz;
        }
    }

    sink->write = file_write;
    sink->flush = file_flush;
    sink->destroy = file_destroy;
    sink->private_data = data;
    return sink;
}
