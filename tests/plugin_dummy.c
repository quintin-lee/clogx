/**
 * @file plugin_dummy.c
 * @brief Minimal test plugin .so for the plugin ABI tests.
 *
 * Exports clogx_plugin_desc and clogx_plugin_create.  The sink
 * tracks write/flush/destroy calls so the test can verify them.
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "clogx_plugin.h"

/* ------------------------------------------------------------------ */
/*  Sink implementation                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int write_count;
    int flush_count;
    int destroy_called;
    char last_buf[256];
} dummy_data_t;

static int dummy_write(log_sink_t *sink, const char *buf, size_t len) {
    dummy_data_t *d = (dummy_data_t *)sink->private_data;
    if (!d)
        return -1;
    d->write_count++;
    size_t copy = len < sizeof(d->last_buf) - 1 ? len : sizeof(d->last_buf) - 1;
    memcpy(d->last_buf, buf, copy);
    d->last_buf[copy] = '\0';
    return (int)len;
}

static void dummy_flush(log_sink_t *sink) {
    dummy_data_t *d = (dummy_data_t *)sink->private_data;
    if (d)
        d->flush_count++;
}

static void dummy_destroy(log_sink_t *sink) {
    if (!sink)
        return;
    dummy_data_t *d = (dummy_data_t *)sink->private_data;
    if (d) {
        d->destroy_called = 1;
        free(d);
    }
    free(sink);
}

/* ------------------------------------------------------------------ */
/*  Plugin ABI entry points                                           */
/* ------------------------------------------------------------------ */

static const clogx_plugin_t desc = {
    .abi_version = CLOGX_PLUGIN_ABI_VERSION,
    .plugin_version = 1,
    .caps = CLOGX_PLUGIN_CAP_NONE,
    .name = "dummy",
    .description = "Test dummy sink for plugin ABI tests",
};

__attribute__((used, visibility("default"))) const clogx_plugin_t *clogx_plugin_desc(void) {
    return &desc;
}

__attribute__((used, visibility("default"))) log_sink_t *
clogx_plugin_create(const char *params_json) {
    (void)params_json;

    log_sink_t *sink = (log_sink_t *)malloc(sizeof(log_sink_t));
    if (!sink)
        return NULL;

    dummy_data_t *d = (dummy_data_t *)malloc(sizeof(dummy_data_t));
    if (!d) {
        free(sink);
        return NULL;
    }

    d->write_count = 0;
    d->flush_count = 0;
    d->destroy_called = 0;
    d->last_buf[0] = '\0';

    sink->abi_version = CLOGX_PLUGIN_ABI_VERSION;
    sink->write = dummy_write;
    sink->flush = dummy_flush;
    sink->destroy = dummy_destroy;
    sink->atfork_child = NULL;
    sink->private_data = d;
    sink->min_level = LOG_LEVEL_TRACE;

    return sink;
}
