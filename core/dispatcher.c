/**
 * @file dispatcher.c
 * @brief Format once, then fan-out to configured sinks (color only on console).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clog_port.h"
#include "dispatcher.h"
#include "log_sink.h"
#include "log_formatter.h"
#include "log_config.h"
#include "log_limits.h"
#include "log_record.h"
#include "log_internal.h"
#include "plugin_loader.h"

typedef struct {
    log_sink_t **sinks;
    int sink_count;
    clog_mutex_t mutex;
} log_dispatcher_t;

static log_dispatcher_t g_dispatcher = {
    .sinks = NULL, .sink_count = 0, .mutex = CLOG_MUTEX_INITIALIZER};

int log_dispatcher_add_sink(log_sink_t *restrict sink) {
    return log_dispatcher_add_sink_for(&g_default_logger, sink);
}

int log_dispatcher_add_sink_for(logger_t *logger, log_sink_t *restrict sink) {
    if (!sink)
        return -1;
    int ret = 0;
    CLOG_MUTEXGUARDED(&logger->dispatcher_mutex, {
        log_sink_t **new_sinks = (log_sink_t **)realloc(
            (void *)logger->sinks, ((size_t)logger->sink_count + 1) * sizeof(log_sink_t *));
        if (!new_sinks) {
            ret = -1;
        } else {
            logger->sinks = new_sinks;
            logger->sinks[logger->sink_count] = sink;
            logger->sink_count++;
        }
    });
    return ret;
}

int log_dispatcher_remove_sink(log_sink_t *restrict sink) {
    if (!sink)
        return -1;

    CLOG_MUTEXGUARDED(&g_dispatcher.mutex, {
        for (int i = 0; i < g_dispatcher.sink_count; i++) {
            if (g_dispatcher.sinks[i] == sink) {
                for (int j = i; j < g_dispatcher.sink_count - 1; j++) {
                    g_dispatcher.sinks[j] = g_dispatcher.sinks[j + 1];
                }
                g_dispatcher.sinks[g_dispatcher.sink_count - 1] = NULL;
                g_dispatcher.sink_count--;
                if (g_dispatcher.sink_count > 0) {
                    log_sink_t **resized = (log_sink_t **)realloc((void *)g_dispatcher.sinks,
                                                                  (size_t)g_dispatcher.sink_count *
                                                                      sizeof(log_sink_t *));
                    /* LCOV_EXCL_START - System realloc failure */
                    if (resized) {
                        g_dispatcher.sinks = resized;
                    }
                    /* LCOV_EXCL_STOP */
                } else {
                    free((void *)g_dispatcher.sinks);
                    g_dispatcher.sinks = NULL;
                }
                break;
            }
        }
    });
    return 0;
}

int log_dispatcher_remove_sink_for(logger_t *logger, log_sink_t *restrict sink) {
    if (!sink)
        return -1;
    CLOG_MUTEXGUARDED(&logger->dispatcher_mutex, {
        for (int i = 0; i < logger->sink_count; i++) {
            if (logger->sinks[i] == sink) {
                for (int j = i; j < logger->sink_count - 1; j++) {
                    logger->sinks[j] = logger->sinks[j + 1];
                }
                logger->sinks[logger->sink_count - 1] = NULL;
                logger->sink_count--;
                if (logger->sink_count > 0) {
                    log_sink_t **resized = (log_sink_t **)realloc(
                        (void *)logger->sinks, (size_t)logger->sink_count * sizeof(log_sink_t *));
                    if (resized) {
                        logger->sinks = resized;
                    }
                } else {
                    free((void *)logger->sinks);
                    logger->sinks = NULL;
                }
                break;
            }
        }
    });
    return 0;
}

int log_dispatcher_dispatch(log_record_t *record) {
    if (!record)
        return -1;

    if (record->level < log_get_level()) {
        return 0;
    }

    /* Format and pre-compute colored output OUTSIDE the dispatcher lock.
     * Formatter has its own mutex for the format string; config reads use
     * an rwlock.  This keeps the dispatcher critical section short (I/O only). */
    char formatted_buf[CLOG_MAX_FORMATTED_SIZE];
    int len = log_formatter_format(record, formatted_buf, sizeof(formatted_buf));
    if (len <= 0) {
        return -1;
    }

    static const char *ansi_codes[] = {
        "\x1b[30m", /* COLOR_NONE / fallback */
        "\x1b[30m", /* COLOR_BLACK */
        "\x1b[31m", /* COLOR_RED */
        "\x1b[32m", /* COLOR_GREEN */
        "\x1b[33m", /* COLOR_YELLOW */
        "\x1b[34m", /* COLOR_BLUE */
        "\x1b[35m", /* COLOR_PURPLE */
        "\x1b[36m", /* COLOR_CYAN */
        "\x1b[37m"  /* COLOR_WHITE */
    };
    const char *reset_code = "\x1b[0m";
    bool color_enabled = log_config_color_enabled();

    char colored_buf[CLOG_MAX_COLORED_SIZE];
    int colored_len = -1;

    if (color_enabled) {
        log_color_t color = get_log_color(record->level);
        size_t ansi_count = sizeof(ansi_codes) / sizeof(ansi_codes[0]);
        int color_idx = (size_t)color < ansi_count ? (int)color : 0;
        int ret = snprintf(colored_buf, sizeof(colored_buf), "%s%s%s", ansi_codes[color_idx],
                           formatted_buf, reset_code);
        if (ret > 0 && ret < (int)sizeof(colored_buf)) {
            colored_len = ret;
        }
    }

    /* Short critical section: iterate sink array and write. */
    CLOG_MUTEXGUARDED(&g_dispatcher.mutex, {
        for (int i = 0; i < g_dispatcher.sink_count; i++) {
            log_sink_t *sink = g_dispatcher.sinks[i];
            if (!sink)
                continue;

            /* Per-sink level gate: skip if the record is below this sink's
             * minimum level.  The global level has already been checked above. */
            if ((int)record->level < (int)sink->min_level)
                continue;

            const char *write_buf = formatted_buf;
            size_t write_len = (size_t)len;

            if (colored_len > 0 && console_sink_is_color_enabled(sink)) {
                write_buf = colored_buf;
                write_len = (size_t)colored_len;
            }

            sink->write(sink, write_buf, write_len);
            if (write_len > 0 && write_buf[write_len - 1] != '\n') {
                sink->write(sink, "\n", 1);
            }
        }
    });

    return 0;
}

int log_dispatcher_dispatch_for(logger_t *logger, log_record_t *record) {
    if (record->level < logger->config.level) {
        return 0;
    }

    char formatted_buf[CLOG_MAX_FORMATTED_SIZE];
    int len = log_formatter_format_for(logger, record, formatted_buf, sizeof(formatted_buf));
    if (len <= 0) {
        return -1;
    }

    static const char *ansi_codes[] = {"\x1b[30m", "\x1b[30m", "\x1b[31m", "\x1b[32m", "\x1b[33m",
                                       "\x1b[34m", "\x1b[35m", "\x1b[36m", "\x1b[37m"};
    const char *reset_code = "\x1b[0m";
    bool color_enabled = logger->config.color;

    char colored_buf[CLOG_MAX_COLORED_SIZE];
    int colored_len = -1;

    if (color_enabled) {
        log_color_t color = get_log_color(record->level);
        size_t ansi_count = sizeof(ansi_codes) / sizeof(ansi_codes[0]);
        int color_idx = (size_t)color < ansi_count ? (int)color : 0;
        int ret = snprintf(colored_buf, sizeof(colored_buf), "%s%s%s", ansi_codes[color_idx],
                           formatted_buf, reset_code);
        if (ret > 0 && ret < (int)sizeof(colored_buf)) {
            colored_len = ret;
        }
    }

    CLOG_MUTEXGUARDED(&logger->dispatcher_mutex, {
        for (int i = 0; i < logger->sink_count; i++) {
            log_sink_t *sink = logger->sinks[i];
            if (!sink)
                continue;
            if ((int)record->level < (int)sink->min_level)
                continue;
            const char *write_buf = formatted_buf;
            size_t write_len = (size_t)len;
            if (colored_len > 0 && console_sink_is_color_enabled(sink)) {
                write_buf = colored_buf;
                write_len = (size_t)colored_len;
            }
            sink->write(sink, write_buf, write_len);
            if (write_len > 0 && write_buf[write_len - 1] != '\n') {
                sink->write(sink, "\n", 1);
            }
        }
    });

    return 0;
}

/* ── Sink creation helpers (used by both old and _for variants) ── */

static int create_sinks_from_cfg(const log_config_t *cfg, log_sink_t **sinks, int max_sinks) {
    int count = 0;
    if (cfg->console_enable) {
        log_sink_t *sink = cfg->console_stderr ? console_sink_create_stderr(cfg->color)
                                               : console_sink_create(cfg->color);
        if (sink && count < max_sinks)
            sinks[count++] = sink;
    }
    if (cfg->file_enable && strlen(cfg->file_path) > 0) {
        log_sink_t *sink = file_sink_create(cfg->file_path, cfg->file_max_size, cfg->file_backups);
        if (sink && count < max_sinks)
            sinks[count++] = sink;
    }
    if (cfg->socket_enable && strlen(cfg->socket_host) > 0) {
        log_sink_t *sink =
            socket_sink_create_tls(cfg->socket_host, cfg->socket_port, cfg->socket_tls,
                                   cfg->socket_tls_ca_file, cfg->socket_tls_skip_verify);
        if (sink && count < max_sinks)
            sinks[count++] = sink;
    }
    return count;
}

/* ── Singleton wrappers (delegate to _for variants) ── */

int log_dispatcher_init(void) {
    return log_dispatcher_init_for(&g_default_logger);
}

int log_dispatcher_build_snapshot(log_config_t *restrict cfg,
                                  log_dispatcher_snapshot_t *restrict snap) {
    return log_dispatcher_build_snapshot_for(&g_default_logger, cfg, snap);
}

void log_dispatcher_destroy_snapshot(log_dispatcher_snapshot_t *restrict snap) {
    log_dispatcher_destroy_snapshot_for(&g_default_logger, snap);
}

void log_dispatcher_commit_snapshot(log_dispatcher_snapshot_t *restrict snap) {
    log_dispatcher_commit_snapshot_for(&g_default_logger, snap);
}

void log_dispatcher_destroy(void) {
    log_dispatcher_destroy_for(&g_default_logger);
}

void log_dispatcher_flush(void) {
    log_dispatcher_flush_for(&g_default_logger);
}

void log_dispatcher_atfork_prepare(void) {
    log_dispatcher_atfork_prepare_for(&g_default_logger);
}

void log_dispatcher_atfork_parent(void) {
    log_dispatcher_atfork_parent_for(&g_default_logger);
}

void log_dispatcher_atfork_child(void) {
    log_dispatcher_atfork_child_for(&g_default_logger);
}

/* ── Instance variants (_for) ── */

int log_dispatcher_init_for(logger_t *logger) {
    log_config_t *cfg = &logger->config;

    log_sink_t *sinks[8] = {0};
    int count = create_sinks_from_cfg(cfg, sinks, 8);

    /* Load plugin sinks from config. */
    {
        log_sink_t *plugin_sinks[CLOG_MAX_PLUGINS];
        int n = log_plugin_create_sinks_from_config(cfg, plugin_sinks, CLOG_MAX_PLUGINS);
        for (int i = 0; i < n && count < (int)(sizeof(sinks) / sizeof(sinks[0])); i++) {
            sinks[count++] = plugin_sinks[i];
        }
    }

    if (count == 0) {
        fprintf(stderr, "No sinks configured; logging will be dropped\n");
        return -1;
    }

    log_dispatcher_destroy_for(logger);

    int ret = 0;
    CLOG_MUTEXGUARDED(&logger->dispatcher_mutex, {
        logger->sinks = (log_sink_t **)malloc((size_t)count * sizeof(log_sink_t *));
        if (!logger->sinks) {
            for (int i = 0; i < count; i++)
                sinks[i]->destroy(sinks[i]);
            ret = -1;
        } else {
            for (int i = 0; i < count; i++)
                logger->sinks[i] = sinks[i];
            logger->sink_count = count;
        }
    });
    return ret;
}

int log_dispatcher_build_snapshot_for(logger_t *logger, log_config_t *restrict cfg,
                                      log_dispatcher_snapshot_t *restrict snap) {
    (void)logger;
    log_sink_t *sinks[8 + CLOG_MAX_PLUGINS] = {0};
    int count = create_sinks_from_cfg(cfg, sinks, 8);

    /* Load plugin sinks from config. */
    {
        int max_plugin_sinks = (int)(sizeof(sinks) / sizeof(sinks[0])) - count;
        if (max_plugin_sinks > CLOG_MAX_PLUGINS)
            max_plugin_sinks = CLOG_MAX_PLUGINS;
        if (max_plugin_sinks > 0) {
            log_sink_t *plugin_sinks[CLOG_MAX_PLUGINS];
            int n = log_plugin_create_sinks_from_config(cfg, plugin_sinks, max_plugin_sinks);
            for (int i = 0; i < n && count < (int)(sizeof(sinks) / sizeof(sinks[0])); i++) {
                sinks[count++] = plugin_sinks[i];
            }
        }
    }

    if (count == 0) {
        fprintf(stderr, "No sinks configured; logging will be dropped\n");
        return -1;
    }

    snap->sinks = (log_sink_t **)malloc((size_t)count * sizeof(log_sink_t *));
    if (!snap->sinks) {
        for (int i = 0; i < count; i++)
            sinks[i]->destroy(sinks[i]);
        return -1;
    }
    for (int i = 0; i < count; i++)
        snap->sinks[i] = sinks[i];
    snap->sink_count = count;
    return 0;
}

void log_dispatcher_destroy_snapshot_for(logger_t *logger,
                                         log_dispatcher_snapshot_t *restrict snap) {
    (void)logger;
    if (!snap)
        return;
    for (int i = 0; i < snap->sink_count; i++) {
        if (snap->sinks[i])
            snap->sinks[i]->destroy(snap->sinks[i]);
    }
    free((void *)snap->sinks);
    snap->sinks = NULL;
    snap->sink_count = 0;
}

void log_dispatcher_commit_snapshot_for(logger_t *logger,
                                        log_dispatcher_snapshot_t *restrict snap) {
    log_sink_t **old_sinks = NULL;
    int old_count = 0;

    CLOG_MUTEXGUARDED(&logger->dispatcher_mutex, {
        old_sinks = logger->sinks;
        old_count = logger->sink_count;
        logger->sinks = snap->sinks;
        logger->sink_count = snap->sink_count;
    });

    for (int i = 0; i < old_count; i++) {
        if (old_sinks[i])
            old_sinks[i]->destroy(old_sinks[i]);
    }
    free((void *)old_sinks);
    snap->sinks = NULL;
    snap->sink_count = 0;
}

void log_dispatcher_destroy_for(logger_t *logger) {
    CLOG_MUTEXGUARDED(&logger->dispatcher_mutex, {
        for (int i = 0; i < logger->sink_count; i++) {
            if (logger->sinks[i])
                logger->sinks[i]->destroy(logger->sinks[i]);
        }
        free((void *)logger->sinks);
        logger->sinks = NULL;
        logger->sink_count = 0;
    });
}

void log_dispatcher_flush_for(logger_t *logger) {
    CLOG_MUTEXGUARDED(&logger->dispatcher_mutex, {
        for (int i = 0; i < logger->sink_count; i++) {
            if (logger->sinks[i])
                logger->sinks[i]->flush(logger->sinks[i]);
        }
    });
}

void log_dispatcher_atfork_prepare_for(logger_t *logger) {
    clog_mutex_lock(&logger->dispatcher_mutex);
}

void log_dispatcher_atfork_parent_for(logger_t *logger) {
    clog_mutex_unlock(&logger->dispatcher_mutex);
}

void log_dispatcher_atfork_child_for(logger_t *logger) {
    for (int i = 0; i < logger->sink_count; i++) {
        if (logger->sinks[i] && logger->sinks[i]->atfork_child)
            logger->sinks[i]->atfork_child(logger->sinks[i]);
    }
    clog_mutex_unlock(&logger->dispatcher_mutex);
}
