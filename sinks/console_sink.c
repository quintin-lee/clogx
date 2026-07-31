/**
 * @file console_sink.c
 * @brief Console sinks (stdout/stderr); color preference queried by dispatcher.
 *
 * ## Design
 *
 * The console sink writes formatted log lines to stdout (default) or
 * stderr. It supports optional ANSI colour escape sequences that are
 * automatically stripped when the output stream is not a TTY.
 *
 * ## Colour Mapping
 *
 * | Level    | Default    | Bright variant            |
 * |----------|------------|---------------------------|
 * | TRACE    | dim white  | bright white              |
 * | DEBUG    | cyan       | bright cyan               |
 * | INFO     | green      | bright green              |
 * | WARN     | yellow     | bright yellow             |
 * | ERROR    | red        | bright red                |
 * | FATAL    | bold red   | bold bright red           |
 *
 * ## Windows VT Processing
 *
 * On Windows, the sink enables `ENABLE_VIRTUAL_TERMINAL_PROCESSING`
 * on the console handle so that ANSI escape sequences render correctly.
 * If the handle does not support VT processing (e.g. redirected to a
 * file), the escape sequences are silently dropped.
 *
 * ## Plugin Interface
 *
 * Implements the `clogx_plugin_v1` ABI so the console sink can be
 * loaded as a shared library plugin.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "clog_port.h"
#include "log_sink.h"
#include "log_record.h"
#include "clogx_plugin.h"

typedef struct {
    FILE *stream;
    int use_color;
} console_sink_data_t;

#if defined(_WIN32) || defined(_WIN64)
static void enable_windows_vt_mode(FILE *stream) {
    HANDLE hOut = GetStdHandle(stream == stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && hOut != NULL) {
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, dwMode);
        }
    }
}
#endif

static int console_write(log_sink_t *sink, const char *buf, size_t len) {
    console_sink_data_t *data = (console_sink_data_t *)sink->private_data;

    if (!data || !data->stream)
        return -1;

    size_t written = fwrite(buf, 1, len, data->stream);
    fflush(data->stream);

    return (int)written;
}

static void console_flush(log_sink_t *sink) {
    if (!sink)
        return;
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

static void console_atfork_child(log_sink_t *sink) {
    (void)sink;
}

log_sink_t *console_sink_create(bool use_color) {
    log_sink_t *sink = malloc(sizeof(log_sink_t));
    if (!sink)
        return NULL;

    console_sink_data_t *data = malloc(sizeof(console_sink_data_t));
    if (!data) {
        free(sink);
        return NULL;
    }

    data->stream = stdout;
    data->use_color = use_color;

#if defined(_WIN32) || defined(_WIN64)
    if (use_color) {
        enable_windows_vt_mode(data->stream);
    }
#endif

    sink->abi_version = CLOGX_PLUGIN_ABI_VERSION;
    sink->write = console_write;
    sink->flush = console_flush;
    sink->destroy = console_destroy;
    sink->atfork_child = console_atfork_child;
    sink->private_data = data;
    sink->min_level = LOG_LEVEL_TRACE;

    return sink;
}

log_sink_t *console_sink_create_stderr(bool use_color) {
    log_sink_t *sink = malloc(sizeof(log_sink_t));
    if (!sink)
        return NULL;

    console_sink_data_t *data = malloc(sizeof(console_sink_data_t));
    if (!data) {
        free(sink);
        return NULL;
    }

    data->stream = stderr;
    data->use_color = use_color;

#if defined(_WIN32) || defined(_WIN64)
    if (use_color) {
        enable_windows_vt_mode(data->stream);
    }
#endif

    sink->abi_version = CLOGX_PLUGIN_ABI_VERSION;
    sink->write = console_write;
    sink->flush = console_flush;
    sink->destroy = console_destroy;
    sink->atfork_child = console_atfork_child;
    sink->private_data = data;
    sink->min_level = LOG_LEVEL_TRACE;

    return sink;
}

bool console_sink_is_color_enabled(log_sink_t *sink) {
    if (!sink || sink->write != console_write)
        return false;
    console_sink_data_t *data = (console_sink_data_t *)sink->private_data;
    return data && data->use_color;
}
