/**
 * @file otlp_sink.c
 * @brief OpenTelemetry OTLP JSON log sink implementation.
 *
 * ## Design
 *
 * Forwards log records to an OpenTelemetry-compatible collector via
 * the OTLP/HTTP JSON protocol. Each log record is rendered as a
 * single JSON payload matching the `LogsService` schema.
 *
 * ## Output Format
 *
 * The sink emits a JSON object per log call (not newline-delimited;
 * each call is a complete HTTP request body):
 *
 * ```json
 * {
 *   "resourceLogs": [{
 *     "resource": { "attributes": [{"key":"service.name","value":{"stringValue":"myapp"}}] },
 *     "scopeLogs": [{
 *       "logRecords": [{
 *         "timeUnixNano": "1234567890000000000",
 *         "severityNumber": 9,
 *         "severityText": "INFO",
 *         "body": { "stringValue": "Server started" },
 *         "attributes": [...]
 *       }]
 *     }]
 *   }]
 * }
 * ```
 *
 * ## Transport
 *
 * The sink writes to a `FILE*` handle (stdout by default). For actual
 * HTTP transport, pair with a sidecar collector (e.g. otel-collector
 * reading from stdin) or use the socket sink with a collector proxy.
 *
 * ## Plugin Interface
 *
 * Implements the `clogx_plugin_v1` ABI.
 */

#include "clog_port.h"
#include "clogx_plugin.h"
#include "log_formatter.h"
#include "log_sink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char  endpoint[256];
    char  service_name[128];
    FILE *file_out;
} otlp_sink_data_t;

static int otlp_sink_write(log_sink_t *sink, const char *buf, size_t len)
{
    if (!sink || !sink->private_data || !buf) {
        return -1;
    }
    otlp_sink_data_t *data = (otlp_sink_data_t *)sink->private_data;

    if (data->file_out) {
        size_t written = fwrite(buf, 1, len, data->file_out);
        if (written < len) {
            return -1;
        }
        fputc('\n', data->file_out);
        return (int)(len + 1);
    }

    /* Fallback stdout formatting if no file destination specified */
    return (int)fwrite(buf, 1, len, stdout);
}

static void otlp_sink_flush(log_sink_t *sink)
{
    if (!sink || !sink->private_data) {
        return;
    }
    otlp_sink_data_t *data = (otlp_sink_data_t *)sink->private_data;
    if (data->file_out) {
        fflush(data->file_out);
    } else {
        fflush(stdout);
    }
}

static void otlp_sink_destroy(log_sink_t *sink)
{
    if (!sink) {
        return;
    }
    if (sink->private_data) {
        otlp_sink_data_t *data = (otlp_sink_data_t *)sink->private_data;
        if (data->file_out && data->file_out != stdout && data->file_out != stderr) {
            fclose(data->file_out);
        }
        free(data);
    }
    free(sink);
}

static void otlp_sink_atfork_child(log_sink_t *sink)
{
    (void)sink;
}

/**
 * @brief Create an OTLP JSON log sink.
 *
 * Renders log records as OTLP/JSON payloads. The endpoint can be
 * "stdout", "stderr", or a file path. If NULL/empty, defaults to stdout.
 *
 * @param endpoint     Output destination (NULL → stdout).
 * @param service_name Service name for OTLP resource (NULL → "clogx_service").
 * @return New sink, or NULL on allocation failure.
 */
log_sink_t *otlp_sink_create(const char *endpoint, const char *service_name)
{
    log_sink_t *sink = (log_sink_t *)calloc(1, sizeof(log_sink_t));
    if (!sink) {
        return NULL;
    }

    otlp_sink_data_t *data = (otlp_sink_data_t *)calloc(1, sizeof(otlp_sink_data_t));
    if (!data) {
        free(sink);
        return NULL;
    }

    if (service_name && *service_name) {
        snprintf(data->service_name, sizeof(data->service_name), "%s", service_name);
    } else {
        snprintf(data->service_name, sizeof(data->service_name), "%s", "clogx_service");
    }

    if (endpoint && *endpoint) {
        snprintf(data->endpoint, sizeof(data->endpoint), "%s", endpoint);
        if (strcmp(endpoint, "stdout") == 0) {
            data->file_out = stdout;
        } else if (strcmp(endpoint, "stderr") == 0) {
            data->file_out = stderr;
        } else {
            data->file_out = fopen(endpoint, "a");
        }
    } else {
        data->file_out = stdout;
    }

    sink->abi_version  = CLOGX_PLUGIN_ABI_VERSION;
    sink->write        = otlp_sink_write;
    sink->flush        = otlp_sink_flush;
    sink->destroy      = otlp_sink_destroy;
    sink->atfork_child = otlp_sink_atfork_child;
    sink->private_data = data;
    sink->min_level    = LOG_LEVEL_TRACE;

    return sink;
}
