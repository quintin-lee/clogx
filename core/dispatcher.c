#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "dispatcher.h"
#include "log_sink.h"
#include "log_formatter.h"
#include "log_config.h"

typedef struct {
    log_sink_t **sinks;
    int sink_count;
    pthread_mutex_t mutex;
} log_dispatcher_t;

static log_dispatcher_t g_dispatcher = {
    .sinks = NULL,
    .sink_count = 0,
    .mutex = PTHREAD_MUTEX_INITIALIZER
};

int log_dispatcher_add_sink(log_sink_t *sink) {
    if (!sink) return -1;

    pthread_mutex_lock(&g_dispatcher.mutex);
    log_sink_t **new_sinks = realloc(g_dispatcher.sinks, (g_dispatcher.sink_count + 1) * sizeof(log_sink_t *));
    if (!new_sinks) {
        pthread_mutex_unlock(&g_dispatcher.mutex);
        return -1;
    }
    g_dispatcher.sinks = new_sinks;
    g_dispatcher.sinks[g_dispatcher.sink_count] = sink;
    g_dispatcher.sink_count++;
    pthread_mutex_unlock(&g_dispatcher.mutex);
    return 0;
}

int log_dispatcher_remove_sink(log_sink_t *sink) {
    if (!sink) return -1;

    pthread_mutex_lock(&g_dispatcher.mutex);
    for (int i = 0; i < g_dispatcher.sink_count; i++) {
        if (g_dispatcher.sinks[i] == sink) {
            for (int j = i; j < g_dispatcher.sink_count - 1; j++) {
                g_dispatcher.sinks[j] = g_dispatcher.sinks[j + 1];
            }
            g_dispatcher.sinks[g_dispatcher.sink_count - 1] = NULL;
            g_dispatcher.sink_count--;
            if (g_dispatcher.sink_count > 0) {
                g_dispatcher.sinks = realloc(g_dispatcher.sinks, g_dispatcher.sink_count * sizeof(log_sink_t *));
            } else {
                free(g_dispatcher.sinks);
                g_dispatcher.sinks = NULL;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_dispatcher.mutex);
    return 0;
}

int log_dispatcher_dispatch(log_record_t *record) {
    if (!record) return -1;

    log_config_t *cfg = log_config_get();
    if (record->level < cfg->level) {
        return 0;
    }

    char formatted_buf[2048];
    int len = log_formatter_format(record, formatted_buf, sizeof(formatted_buf));
    if (len <= 0) {
        return -1;
    }

    pthread_mutex_lock(&g_dispatcher.mutex);
    for (int i = 0; i < g_dispatcher.sink_count; i++) {
        if (g_dispatcher.sinks[i]) {
            g_dispatcher.sinks[i]->write(g_dispatcher.sinks[i], formatted_buf, (size_t)len);
            if (formatted_buf[len - 1] != '\n') {
                g_dispatcher.sinks[i]->write(g_dispatcher.sinks[i], "\n", 1);
            }
            g_dispatcher.sinks[i]->flush(g_dispatcher.sinks[i]);
        }
    }
    pthread_mutex_unlock(&g_dispatcher.mutex);

    return 0;
}

int log_dispatcher_init(void) {
    log_config_t *cfg = log_config_get();
    log_dispatcher_destroy();

    if (cfg->console_enable) {
        log_sink_t *sink = console_sink_create();
        if (sink) {
            log_dispatcher_add_sink(sink);
        }
    }

    if (cfg->file_enable && strlen(cfg->file_path) > 0) {
        log_sink_t *sink = file_sink_create(cfg->file_path, cfg->file_max_size, cfg->file_backups);
        if (sink) {
            log_dispatcher_add_sink(sink);
        }
    }

    if (cfg->socket_enable && strlen(cfg->socket_host) > 0) {
        log_sink_t *sink = socket_sink_create(cfg->socket_host, cfg->socket_port);
        if (sink) {
            log_dispatcher_add_sink(sink);
        }
    }

    return 0;
}

void log_dispatcher_destroy(void) {
    pthread_mutex_lock(&g_dispatcher.mutex);
    for (int i = 0; i < g_dispatcher.sink_count; i++) {
        if (g_dispatcher.sinks[i]) {
            g_dispatcher.sinks[i]->destroy(g_dispatcher.sinks[i]);
        }
    }
    free(g_dispatcher.sinks);
    g_dispatcher.sinks = NULL;
    g_dispatcher.sink_count = 0;
    pthread_mutex_unlock(&g_dispatcher.mutex);
    pthread_mutex_destroy(&g_dispatcher.mutex);
}

void log_dispatcher_flush(void) {
    pthread_mutex_lock(&g_dispatcher.mutex);
    for (int i = 0; i < g_dispatcher.sink_count; i++) {
        if (g_dispatcher.sinks[i]) {
            g_dispatcher.sinks[i]->flush(g_dispatcher.sinks[i]);
        }
    }
    pthread_mutex_unlock(&g_dispatcher.mutex);
}
