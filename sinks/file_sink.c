/**
 * @file file_sink.c
 * @brief File sink with size-based rotation and parent-directory creation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "clog_port.h"
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

/** @brief mkdir -p for the parent directory of @p path. */
static int ensure_parent_dirs(const char *path) {
    char dir[1024];
    size_t len;

    if (!path || !*path)
        return -1;

    len = strlen(path);
    if (len >= sizeof(dir))
        return -1;
    memcpy(dir, path, len + 1);

    char *slash1 = strrchr(dir, '/');
    char *slash2 = strrchr(dir, '\\');
    char *slash = (slash1 > slash2) ? slash1 : slash2;
    if (!slash || slash == dir)
        return 0;
    *slash = '\0';

    for (char *p = dir + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char orig = *p;
            *p = '\0';
            if (clog_mkdir(dir) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = orig;
        }
    }

    if (clog_mkdir(dir) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int file_write(log_sink_t *sink, const char *buf, size_t len) {
    file_sink_data_t *data = (file_sink_data_t *)sink->private_data;
    if (!data || !data->file || !buf)
        return -1;

    size_t written = 0;
    while (written < len) {
        size_t n = fwrite(buf + written, 1, len - written, data->file);
        if (n == 0) {
            if (ferror(data->file)) {
                clearerr(data->file);
                return -1;
            }
            break;
        }
        written += n;
    }

    data->current_size += written;

    if (data->max_size > 0 && data->current_size >= data->max_size) {
        if (fflush(data->file) != 0) {
            return -1;
        }
        fclose(data->file);
        data->file = NULL;

        file_rotate_file(data->path, data->backups);

        data->file = fopen(data->path, "a");
        if (!data->file)
            return -1;
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

static void file_atfork_child(log_sink_t *sink) {
    if (!sink || !sink->private_data)
        return;

    file_sink_data_t *data = (file_sink_data_t *)sink->private_data;
    if (data->file) {
        fclose(data->file);
        data->file = NULL;
    }

    data->file = fopen(data->path, "a");
    if (data->file) {
        struct stat st;
        if (fstat(fileno(data->file), &st) == 0 && st.st_size > 0) {
            data->current_size = (uint64_t)st.st_size;
        }
    }
}

log_sink_t *file_sink_create(const char *path, uint64_t max_size, int backups) {
    if (!path || strlen(path) == 0)
        return NULL;

    log_sink_t *sink = malloc(sizeof(log_sink_t));
    if (!sink)
        return NULL;

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

    if (ensure_parent_dirs(path) != 0) {
        free(data->path);
        free(data);
        free(sink);
        return NULL;
    }

    data->file = fopen(path, "a");
    if (!data->file) {
        perror("Failed to open file for logging");
        free(data->path);
        free(data);
        free(sink);
        return NULL;
    }

    /* Append mode ignores fseek; use fstat for initial size. */
    {
        struct stat st;
        if (fstat(fileno(data->file), &st) == 0 && st.st_size > 0) {
            data->current_size = (uint64_t)st.st_size;
        }
    }

    sink->write = file_write;
    sink->flush = file_flush;
    sink->destroy = file_destroy;
    sink->atfork_child = file_atfork_child;
    sink->private_data = data;
    sink->min_level = LOG_LEVEL_TRACE;
    return sink;
}
