#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include "log_sink.h"
#include "log_record.h"
#include "rotate.h"

typedef struct {
    FILE *file;
    const char *path;
    uint64_t max_size;
    int backups;
} file_sink_data_t;

static int file_write(log_sink_t *sink, const char *buf, size_t len) {
    file_sink_data_t *data = (file_sink_data_t *)sink->private_data;
    if (!data || !data->file) return -1;
    size_t written = fwrite(buf, 1, len, data->file);
    if (written == len) {
        fseek(data->file, 0, SEEK_END);
        long current_size = ftell(data->file);
        fseek(data->file, 0, SEEK_SET);
        if (current_size >= (long)data->max_size && data->max_size > 0) {
            fclose(data->file);
            file_rotate_file(data->path, data->backups);
            data->file = fopen(data->path, "a");
            if (!data->file) return -1;
        } else {
            fflush(data->file);
        }
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
        free((char *)data->path);
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
    data->file = NULL;
    data->file = fopen(path, "a");
    if (!data->file) {
        perror("Failed to open file for logging");
        free((char *)data->path);
        free(data);
        free(sink);
        return NULL;
    }
    sink->write = file_write;
    sink->flush = file_flush;
    sink->destroy = file_destroy;
    sink->private_data = data;
    return sink;
}