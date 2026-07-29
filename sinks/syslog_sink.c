/**
 * @file syslog_sink.c
 * @brief POSIX syslog sink implementation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log_sink.h"

#ifndef _WIN32
#include <syslog.h>

typedef struct {
    char *ident;
    int facility;
} syslog_sink_data_t;

static int syslog_write(log_sink_t *sink, const char *buf, size_t len) {
    (void)len;
    syslog_sink_data_t *data = (syslog_sink_data_t *)sink->private_data;
    if (!data)
        return -1;

    /* Parse priority from prefix if available, or default to LOG_INFO */
    int priority = LOG_INFO;
    if (strstr(buf, "[TRACE]") || strstr(buf, "[DEBUG]")) {
        priority = LOG_DEBUG;
    } else if (strstr(buf, "[WARN]") || strstr(buf, "[WARNING]")) {
        priority = LOG_WARNING;
    } else if (strstr(buf, "[ERROR]")) {
        priority = LOG_ERR;
    } else if (strstr(buf, "[FATAL]")) {
        priority = LOG_CRIT;
    }

    syslog(priority, "%s", buf);
    return (int)len;
}

static void syslog_flush(log_sink_t *sink) {
    (void)sink;
}

static void syslog_destroy(log_sink_t *sink) {
    if (!sink)
        return;
    syslog_sink_data_t *data = (syslog_sink_data_t *)sink->private_data;
    if (data) {
        closelog();
        free(data->ident);
        free(data);
    }
    free(sink);
}

static void syslog_atfork_child(log_sink_t *sink) {
    if (!sink || !sink->private_data)
        return;
    syslog_sink_data_t *data = (syslog_sink_data_t *)sink->private_data;
    closelog();
    openlog(data->ident, LOG_PID | LOG_NDELAY, data->facility);
}

log_sink_t *syslog_sink_create(const char *ident, int facility) {
    log_sink_t *sink = malloc(sizeof(log_sink_t));
    if (!sink)
        return NULL;

    syslog_sink_data_t *data = malloc(sizeof(syslog_sink_data_t));
    if (!data) {
        free(sink);
        return NULL;
    }

    data->ident = ident ? strdup(ident) : strdup("clogx");
    data->facility = facility;

    openlog(data->ident, LOG_PID | LOG_NDELAY, data->facility);

    sink->write = syslog_write;
    sink->flush = syslog_flush;
    sink->destroy = syslog_destroy;
    sink->atfork_child = syslog_atfork_child;
    sink->private_data = data;
    sink->min_level = LOG_LEVEL_TRACE;

    return sink;
}

#else

log_sink_t *syslog_sink_create(const char *ident, int facility) {
    (void)ident;
    (void)facility;
    return NULL;
}

#endif
