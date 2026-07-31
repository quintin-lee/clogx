/**
 * @file custom_sink.c
 * @brief User-defined custom sink plugin implementation.
 *
 * ## Design
 *
 * The custom sink is a thin adapter that delegates all operations to
 * user-supplied callback functions. This allows application code to
 * integrate clogx with any output destination (e.g. in-memory ring
 * buffer, database, message queue) without modifying the library.
 *
 * ## Callback Interface
 *
 * ```c
 * typedef struct {
 *     int  (*write)(log_sink_t *sink, const char *buf, size_t len);
 *     void (*flush)(log_sink_t *sink);
 *     void (*destroy)(log_sink_t *sink);
 *     void *private_data;
 * } custom_sink_config_t;
 * ```
 *
 * - `write`: required. Called for each formatted log line.
 * - `flush`: optional. Called on `log_flush()`.
 * - `destroy`: optional. Called when the sink is removed.
 * - `private_data`: opaque pointer passed through `sink->private_data`.
 *
 * ## Usage
 *
 * ```c
 * custom_sink_config_t cfg = {
 *     .write = my_write_fn,
 *     .flush = my_flush_fn,
 *     .destroy = my_destroy_fn,
 *     .private_data = my_ctx,
 * };
 * log_sink_t *sink = custom_sink_create(&cfg);
 * log_add_sink(sink);
 * ```
 *
 * ## Plugin Interface
 *
 * Implements the `clogx_plugin_v1` ABI.
 */
#include <stdlib.h>
#include "log_sink.h"
#include "clogx_plugin.h"

typedef struct {
    int (*user_write)(log_sink_t *sink, const char *buf, size_t len);
    void (*user_flush)(log_sink_t *sink);
    void (*user_destroy)(log_sink_t *sink);
    void *user_private;
} custom_sink_data_t;

static int custom_write(log_sink_t *sink, const char *buf, size_t len) {
    if (!sink || !sink->private_data)
        return -1;
    custom_sink_data_t *data = (custom_sink_data_t *)sink->private_data;
    if (data->user_write) {
        return data->user_write(sink, buf, len);
    }
    return -1;
}

static void custom_flush(log_sink_t *sink) {
    if (!sink || !sink->private_data)
        return;
    custom_sink_data_t *data = (custom_sink_data_t *)sink->private_data;
    if (data->user_flush) {
        data->user_flush(sink);
    }
}

static void custom_destroy(log_sink_t *sink) {
    if (!sink)
        return;
    custom_sink_data_t *data = (custom_sink_data_t *)sink->private_data;
    if (data) {
        if (data->user_destroy) {
            data->user_destroy(sink);
        }
        free(data);
    }
    free(sink);
}

log_sink_t *custom_sink_create(int (*write_fn)(log_sink_t *sink, const char *buf, size_t len),
                               void (*flush_fn)(log_sink_t *sink),
                               void (*destroy_fn)(log_sink_t *sink), void *private_data) {
    if (!write_fn)
        return NULL;

    log_sink_t *sink = malloc(sizeof(log_sink_t));
    if (!sink)
        return NULL;

    custom_sink_data_t *data = malloc(sizeof(custom_sink_data_t));
    if (!data) {
        free(sink);
        return NULL;
    }

    data->user_write = write_fn;
    data->user_flush = flush_fn;
    data->user_destroy = destroy_fn;
    data->user_private = private_data;

    sink->abi_version = CLOGX_PLUGIN_ABI_VERSION;
    sink->write = custom_write;
    sink->flush = custom_flush;
    sink->destroy = custom_destroy;
    sink->atfork_child = NULL;
    sink->private_data = data;
    sink->min_level = LOG_LEVEL_TRACE;

    return sink;
}

void *custom_sink_get_private_data(log_sink_t *sink) {
    if (!sink || sink->write != custom_write)
        return NULL;
    custom_sink_data_t *data = (custom_sink_data_t *)sink->private_data;
    return data ? data->user_private : NULL;
}
