/**
 * @file syslog_sink.c
 * @brief POSIX syslog sink implementation.
 *
 * ## Design
 *
 * Forwards formatted log lines to the system syslog daemon via the
 * POSIX `syslog()` API. Log levels are mapped to syslog priorities:
 *
 * | clogx Level | syslog Priority |
 * |-------------|-----------------|
 * | TRACE       | LOG_DEBUG       |
 * | DEBUG       | LOG_DEBUG       |
 * | INFO        | LOG_INFO        |
 * | WARN        | LOG_WARNING     |
 * | ERROR       | LOG_ERR         |
 * | FATAL       | LOG_CRIT        |
 *
 * ## Facility
 *
 * The syslog facility (e.g. `LOG_USER`, `LOG_LOCAL0`) is configurable
 * at sink creation time and defaults to `LOG_USER`.
 *
 * ## Thread Safety
 *
 * The POSIX `syslog()` function is required to be thread-safe by the
 * specification. The sink passes the ident string pointer to `openlog()`
 * which must remain valid for the lifetime of the sink.
 *
 * ## Windows
 *
 * On Windows, `syslog_sink_create` returns `NULL` since POSIX syslog
 * is not available. Callers should fall back to `file_sink` or
 * `socket_sink` on Windows.
 *
 * ## Plugin Interface
 *
 * Implements the `clogx_plugin_v1` ABI.
 */

#include "clogx_plugin.h"
#include "log_sink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <syslog.h>

typedef struct {
    char *ident;
    int   facility;
} syslog_sink_data_t;

static int syslog_write(log_sink_t *sink, const char *buf, size_t len)
{
    (void)len;
    syslog_sink_data_t *data = (syslog_sink_data_t *)sink->private_data;
    if (!data || !buf) {
        return -1;
    }

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

static void syslog_flush(log_sink_t *sink)
{
    (void)sink;
}

static void syslog_destroy(log_sink_t *sink)
{
    if (!sink) {
        return;
    }
    syslog_sink_data_t *data = (syslog_sink_data_t *)sink->private_data;
    if (data) {
        closelog();
        free(data->ident);
        free(data);
    }
    free(sink);
}

static void syslog_atfork_child(log_sink_t *sink)
{
    if (!sink || !sink->private_data) {
        return;
    }
    syslog_sink_data_t *data = (syslog_sink_data_t *)sink->private_data;
    closelog();
    openlog(data->ident, LOG_PID | LOG_NDELAY, data->facility);
}

/**
 * @brief Create a POSIX syslog sink.
 *
 * Opens the syslog connection with the given ident and facility.
 * On Windows, returns NULL since POSIX syslog is unavailable.
 *
 * @param ident     Syslog ident string (NULL defaults to "clogx").
 * @param facility  Syslog facility (e.g. LOG_USER, LOG_LOCAL0).
 * @return New sink, or NULL on Windows or allocation failure.
 */
log_sink_t *syslog_sink_create(const char *ident, int facility)
{
    log_sink_t *sink = malloc(sizeof(log_sink_t));
    if (!sink) {
        return NULL;
    }

    syslog_sink_data_t *data = malloc(sizeof(syslog_sink_data_t));
    if (!data) {
        free(sink);
        return NULL;
    }

    data->ident    = ident ? strdup(ident) : strdup("clogx");
    data->facility = facility;

    openlog(data->ident, LOG_PID | LOG_NDELAY, data->facility);

    sink->abi_version  = CLOGX_PLUGIN_ABI_VERSION;
    sink->write        = syslog_write;
    sink->flush        = syslog_flush;
    sink->destroy      = syslog_destroy;
    sink->atfork_child = syslog_atfork_child;
    sink->private_data = data;
    sink->min_level    = LOG_LEVEL_TRACE;

    return sink;
}

#else

log_sink_t *syslog_sink_create(const char *ident, int facility)
{
    (void)ident;
    (void)facility;
    return NULL;
}

#endif
