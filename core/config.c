/**
 * @file config.c
 * @brief YAML config parser via libyaml event API, with defaults, rwlock, and reload path memory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "clog_port.h"
#include <yaml.h>
#include "log_config.h"
#include "log_formatter.h"
#include "log_internal.h"

typedef enum {
    HANDLER_ASYNC,
    HANDLER_BACKUP,
    HANDLER_CATCH_SIGNALS,
    HANDLER_COLOR,
    HANDLER_CONSOLE_ENABLE,
    HANDLER_CONSOLE_STDERR,
    HANDLER_FILE_ENABLE,
    HANDLER_FILE_PATH,
    HANDLER_FORMAT,
    HANDLER_HOST,
    HANDLER_LEVEL,
    HANDLER_MAX_SIZE,
    HANDLER_PORT,
    HANDLER_QUEUE_SIZE,
    HANDLER_RATE_LIMIT_BURST,
    HANDLER_RATE_LIMIT_ENABLE,
    HANDLER_RATE_LIMIT_MAX_PER_SEC,
    HANDLER_SOCKET_ENABLE,
    HANDLER_SOCKET_TLS,
    HANDLER_SOCKET_TLS_CA_FILE,
    HANDLER_SOCKET_TLS_SKIP_VERIFY,
    HANDLER_TIME_FORMAT,
    HANDLER_PROMETHEUS_ENABLE,
    HANDLER_PROMETHEUS_PORT,
} config_handler_t;

typedef struct {
    const char *key;
    config_handler_t handler;
} config_key_t;

static int compare_config_keys(const void *a, const void *b) {
    const config_key_t *ka = (const config_key_t *)a;
    const config_key_t *kb = (const config_key_t *)b;
    return strcmp(ka->key, kb->key);
}

static const config_key_t g_config_keys[] = {
    {"async", HANDLER_ASYNC},
    {"backup", HANDLER_BACKUP},
    {"backups", HANDLER_BACKUP},
    {"catch_signals", HANDLER_CATCH_SIGNALS},
    {"color", HANDLER_COLOR},
    {"console_enable", HANDLER_CONSOLE_ENABLE},
    {"console_stderr", HANDLER_CONSOLE_STDERR},
    {"file_enable", HANDLER_FILE_ENABLE},
    {"file_path", HANDLER_FILE_PATH},
    {"format", HANDLER_FORMAT},
    {"host", HANDLER_HOST},
    {"level", HANDLER_LEVEL},
    {"max_size", HANDLER_MAX_SIZE},
    {"path", HANDLER_FILE_PATH},
    {"port", HANDLER_PORT},
    {"prometheus_enable", HANDLER_PROMETHEUS_ENABLE},
    {"prometheus_port", HANDLER_PROMETHEUS_PORT},
    {"queue_size", HANDLER_QUEUE_SIZE},
    {"rate_limit_burst", HANDLER_RATE_LIMIT_BURST},
    {"rate_limit_enable", HANDLER_RATE_LIMIT_ENABLE},
    {"rate_limit_max_per_sec", HANDLER_RATE_LIMIT_MAX_PER_SEC},
    {"socket_enable", HANDLER_SOCKET_ENABLE},
    {"socket_tls", HANDLER_SOCKET_TLS},
    {"socket_tls_ca_file", HANDLER_SOCKET_TLS_CA_FILE},
    {"socket_tls_skip_verify", HANDLER_SOCKET_TLS_SKIP_VERIFY},
    {"time_format", HANDLER_TIME_FORMAT},
    {"tls_ca_file", HANDLER_SOCKET_TLS_CA_FILE},
    {"tls_enable", HANDLER_SOCKET_TLS},
    {"tls_skip_verify", HANDLER_SOCKET_TLS_SKIP_VERIFY},
};

static char g_config_format[512] = "";
static char g_config_time_format[64] = "";

/**
 * @brief Parse a YAML config file using libyaml's event-based API.
 *
 * Looks for a top-level "log" mapping and processes all scalar key:value
 * pairs inside it (depth 2).  If no "log" mapping exists, falls back to
 * processing top-level scalar pairs (depth 1) for backward compatibility.
 * Nested mappings, sequences, and unknown keys are silently skipped so that
 * a config can contain YAML structure other sinks/providers may want.
 */
static int parse_config_file(const char *filepath, log_config_t *cfg) {
    FILE *f = fopen(filepath, "r");
    if (!f) {
        perror("Failed to open config file");
        return -1;
    }

    yaml_parser_t parser;
    if (!yaml_parser_initialize(&parser)) {
        fprintf(stderr, "Failed to initialize YAML parser\n");
        fclose(f);
        return -1;
    }
    yaml_parser_set_input_file(&parser, f);

    int has_errors = 0;
    int depth = 0;             /* mapping nesting depth */
    int expect_key = 1;        /* 1 = expect key, 0 = expect value at current depth */
    int in_log_section = 0;    /* 1 when we're inside the top-level "log" mapping */
    int found_log_section = 0; /* 1 if a top-level "log:" mapping was found */
    char current_key[128] = "";
    yaml_event_t event;

    /* Plugin parsing state */
    int in_plugins = 0;       /* inside plugins: sequence */
    int in_plugin_entry = 0;  /* inside a single plugin entry mapping */
    int in_plugin_config = 0; /* inside a plugin's config: mapping */
    int plugin_index = 0;     /* current plugin entry index */
    char plugin_path[CLOG_MAX_PATH_SIZE] = "";
    char plugin_json[CLOG_MAX_PLUGIN_CONFIG_SIZE] = "";
    /* Start with empty JSON object: "{" */
    int plugin_json_len = 0;
    char plugin_cfg_key[128] = "";

    while (1) {
        if (!yaml_parser_parse(&parser, &event)) {
            fprintf(stderr, "YAML parse error: %s\n", filepath);
            has_errors = 1;
            break;
        }

        switch (event.type) {
        case YAML_SEQUENCE_START_EVENT:
            depth++;
            if (strcmp(current_key, "plugins") == 0 && (in_log_section || found_log_section == 0)) {
                in_plugins = 1;
                plugin_index = 0;
            }
            expect_key = 1;
            break;

        case YAML_SEQUENCE_END_EVENT:
            depth--;
            in_plugins = 0;
            in_plugin_entry = 0;
            in_plugin_config = 0;
            expect_key = 1;
            break;

        case YAML_SCALAR_EVENT: {
            const char *val = (const char *)event.data.scalar.value;

            /* Plugin section scalars bypass the in_log_section guard. */
            if (in_plugin_entry) {
                if (in_plugin_config) {
                    if (expect_key) {
                        snprintf(plugin_cfg_key, sizeof(plugin_cfg_key), "%s", val);
                        expect_key = 0;
                    } else {
                        /* Append "key":"escaped_value" to the JSON buffer. */
                        int rem = (int)sizeof(plugin_json) - plugin_json_len;
                        if (rem > 0 && plugin_json_len > 0) {
                            int written =
                                snprintf(plugin_json + plugin_json_len, (size_t)rem, "%s\"%s\":\"",
                                         plugin_json_len > 1 ? "," : "", plugin_cfg_key);
                            if (written > 0 && written < rem) {
                                plugin_json_len += written;
                                rem -= written;
                                /* JSON-escape the value. */
                                for (const char *p = val; *p && rem > 2; p++) {
                                    if (*p == '"' || *p == '\\') {
                                        plugin_json[plugin_json_len++] = '\\';
                                        rem--;
                                    }
                                    plugin_json[plugin_json_len++] = *p;
                                    rem--;
                                }
                                if (rem > 0) {
                                    plugin_json[plugin_json_len++] = '"';
                                    plugin_json[plugin_json_len] = '\0';
                                }
                            }
                        }
                        expect_key = 1;
                    }
                } else if (expect_key) {
                    snprintf(current_key, sizeof(current_key), "%s", val);
                    expect_key = 0;
                } else if (strcmp(current_key, "path") == 0) {
                    snprintf(plugin_path, sizeof(plugin_path), "%s", val);
                    expect_key = 1;
                } else {
                    expect_key = 1;
                }
                break;
            }

            if (!in_log_section && !(found_log_section == 0 && depth == 1))
                break;

            if (expect_key) {
                snprintf(current_key, sizeof(current_key), "%s", val);
                expect_key = 0;
            } else {
                expect_key = 1; /* ready for next key */

                config_key_t key = {current_key, 0};
                const config_key_t *found =
                    bsearch(&key, g_config_keys, sizeof(g_config_keys) / sizeof(g_config_keys[0]),
                            sizeof(g_config_keys[0]), compare_config_keys);
                if (found) {
                    switch (found->handler) {
                    case HANDLER_LEVEL: {
                        if (strcmp(val, "TRACE") == 0)
                            cfg->level = LOG_LEVEL_TRACE;
                        else if (strcmp(val, "DEBUG") == 0)
                            cfg->level = LOG_LEVEL_DEBUG;
                        else if (strcmp(val, "INFO") == 0)
                            cfg->level = LOG_LEVEL_INFO;
                        else if (strcmp(val, "WARN") == 0)
                            cfg->level = LOG_LEVEL_WARN;
                        else if (strcmp(val, "ERROR") == 0)
                            cfg->level = LOG_LEVEL_ERROR;
                        else if (strcmp(val, "FATAL") == 0)
                            cfg->level = LOG_LEVEL_FATAL;
                        else {
                            fprintf(stderr, "Unknown log level: %s\n", val);
                            has_errors = 1;
                        }
                        break;
                    }
                    case HANDLER_ASYNC:
                        cfg->async = (strcmp(val, "true") == 0);
                        break;
                    case HANDLER_QUEUE_SIZE: {
                        char *end = NULL;
                        errno = 0;
                        long qs = strtol(val, &end, 10);
                        if (end == val || *end != '\0' || errno == ERANGE || qs <= 0) {
                            fprintf(stderr, "Invalid queue_size: %s (must be a positive integer)\n",
                                    val);
                            has_errors = 1;
                        } else {
                            cfg->queue_size = (int)qs;
                        }
                        break;
                    }
                    case HANDLER_CATCH_SIGNALS:
                        cfg->catch_signals = (strcmp(val, "true") == 0);
                        break;
                    case HANDLER_COLOR:
                        cfg->color = (strcmp(val, "true") == 0);
                        break;
                    case HANDLER_FORMAT:
                        snprintf(g_config_format, sizeof(g_config_format), "%s", val);
                        cfg->format = g_config_format;
                        if (strcmp(val, "json") == 0 || strcmp(val, "JSON") == 0) {
                            cfg->format_type = LOG_FORMAT_JSON;
                        } else {
                            cfg->format_type = LOG_FORMAT_TEXT;
                        }
                        break;
                    case HANDLER_TIME_FORMAT:
                        snprintf(g_config_time_format, sizeof(g_config_time_format), "%s", val);
                        cfg->time_format = g_config_time_format;
                        break;
                    case HANDLER_CONSOLE_ENABLE:
                        cfg->console_enable = (strcmp(val, "true") == 0);
                        break;
                    case HANDLER_CONSOLE_STDERR:
                        cfg->console_stderr = (strcmp(val, "true") == 0);
                        break;
                    case HANDLER_FILE_ENABLE:
                        cfg->file_enable = (strcmp(val, "true") == 0);
                        break;
                    case HANDLER_FILE_PATH:
                        snprintf(cfg->file_path, sizeof(cfg->file_path), "%s", val);
                        cfg->file_enable = 1;
                        break;
                    case HANDLER_MAX_SIZE: {
                        size_t vlen = strlen(val);
                        char size_buf[64];
                        char *end = NULL;
                        unsigned long long n;
                        uint64_t mult = 1;

                        if (vlen == 0 || vlen >= sizeof(size_buf)) {
                            fprintf(stderr, "Invalid max_size: %s\n", val);
                            has_errors = 1;
                            break;
                        }
                        memcpy(size_buf, val, vlen + 1);

                        if (vlen >= 2) {
                            char u0 = size_buf[vlen - 2];
                            char u1 = size_buf[vlen - 1];
                            if ((u0 == 'K' || u0 == 'k') && (u1 == 'B' || u1 == 'b')) {
                                size_buf[vlen - 2] = '\0';
                                mult = 1024ULL;
                            } else if ((u0 == 'M' || u0 == 'm') && (u1 == 'B' || u1 == 'b')) {
                                size_buf[vlen - 2] = '\0';
                                mult = 1024ULL * 1024ULL;
                            } else if ((u0 == 'G' || u0 == 'g') && (u1 == 'B' || u1 == 'b')) {
                                size_buf[vlen - 2] = '\0';
                                mult = 1024ULL * 1024ULL * 1024ULL;
                            }
                        }
                        if (mult == 1 && vlen >= 1) {
                            char u = size_buf[vlen - 1];
                            if (u == 'K' || u == 'k') {
                                size_buf[vlen - 1] = '\0';
                                mult = 1024ULL;
                            } else if (u == 'M' || u == 'm') {
                                size_buf[vlen - 1] = '\0';
                                mult = 1024ULL * 1024ULL;
                            } else if (u == 'G' || u == 'g') {
                                size_buf[vlen - 1] = '\0';
                                mult = 1024ULL * 1024ULL * 1024ULL;
                            }
                        }

                        errno = 0;
                        n = strtoull(size_buf, &end, 10);
                        if (end == size_buf || *end != '\0' || errno == ERANGE) {
                            fprintf(stderr, "Invalid max_size: %s\n", val);
                            has_errors = 1;
                        } else if (mult > 1 && n > UINT64_MAX / mult) {
                            fprintf(stderr, "max_size overflow: %s (too large)\n", val);
                            has_errors = 1;
                        } else {
                            cfg->file_max_size = n * mult;
                        }
                        break;
                    }
                    case HANDLER_BACKUP: {
                        char *end = NULL;
                        errno = 0;
                        long bk = strtol(val, &end, 10);
                        if (end == val || *end != '\0' || errno == ERANGE || bk < 0) {
                            fprintf(stderr,
                                    "Invalid backups: %s (must be a non-negative integer)\n", val);
                            has_errors = 1;
                        } else {
                            cfg->file_backups = (int)bk;
                        }
                        break;
                    }
                    case HANDLER_HOST:
                        snprintf(cfg->socket_host, sizeof(cfg->socket_host), "%s", val);
                        cfg->socket_enable = 1;
                        break;
                    case HANDLER_PORT: {
                        char *end = NULL;
                        errno = 0;
                        long p = strtol(val, &end, 10);
                        if (end == val || *end != '\0' || errno == ERANGE || p <= 0 || p > 65535) {
                            fprintf(stderr, "Invalid port: %s (must be 1..65535)\n", val);
                            has_errors = 1;
                        } else {
                            cfg->socket_port = (int)p;
                        }
                        break;
                    }
                    case HANDLER_SOCKET_ENABLE:
                        cfg->socket_enable = (strcmp(val, "true") == 0);
                        break;
                    case HANDLER_SOCKET_TLS:
                        cfg->socket_tls = (strcmp(val, "true") == 0);
                        break;
                    case HANDLER_SOCKET_TLS_CA_FILE:
                        snprintf(cfg->socket_tls_ca_file, sizeof(cfg->socket_tls_ca_file), "%s",
                                 val);
                        break;
                    case HANDLER_SOCKET_TLS_SKIP_VERIFY:
                        cfg->socket_tls_skip_verify = (strcmp(val, "true") == 0);
                        break;
                    case HANDLER_RATE_LIMIT_ENABLE:
                        cfg->rate_limit_enable = (strcmp(val, "true") == 0);
                        break;
                    case HANDLER_RATE_LIMIT_MAX_PER_SEC: {
                        char *end = NULL;
                        errno = 0;
                        long v = strtol(val, &end, 10);
                        if (end == val || *end != '\0' || errno == ERANGE || v <= 0) {
                            fprintf(stderr, "Invalid rate_limit_max_per_sec: %s\n", val);
                            has_errors = 1;
                        } else {
                            cfg->rate_limit_max_per_sec = (int)v;
                        }
                        break;
                    }
                    case HANDLER_RATE_LIMIT_BURST: {
                        char *end = NULL;
                        errno = 0;
                        long v = strtol(val, &end, 10);
                        if (end == val || *end != '\0' || errno == ERANGE || v <= 0) {
                            fprintf(stderr, "Invalid rate_limit_burst: %s\n", val);
                            has_errors = 1;
                        } else {
                            cfg->rate_limit_burst = (int)v;
                        }
                        break;
                    }
                    case HANDLER_PROMETHEUS_ENABLE:
                        cfg->prometheus_enable = (strcmp(val, "true") == 0);
                        break;
                    case HANDLER_PROMETHEUS_PORT: {
                        char *end = NULL;
                        errno = 0;
                        long p = strtol(val, &end, 10);
                        if (end == val || *end != '\0' || errno == ERANGE || p <= 0 || p > 65535) {
                            fprintf(stderr, "Invalid prometheus_port: %s (must be 1..65535)\n",
                                    val);
                            has_errors = 1;
                        } else {
                            cfg->prometheus_port = (int)p;
                        }
                        break;
                    }
                    }
                }
                /* unknown keys are silently skipped */
            }
            break;
        }
        case YAML_MAPPING_START_EVENT:
            depth++;
            if (in_plugins && in_plugin_entry && !in_plugin_config) {
                /* Entering a plugin's config: mapping. */
                in_plugin_config = 1;
                plugin_json[0] = '{';
                plugin_json[1] = '\0';
                plugin_json_len = 1;
                expect_key = 1;
                break;
            }
            if (in_plugins && !in_plugin_entry) {
                /* Entering a plugin entry mapping inside the plugins: sequence. */
                in_plugin_entry = 1;
                plugin_path[0] = '\0';
                expect_key = 1;
                break;
            }
            if (depth == 1) {
                in_log_section = 0;
                if (strcmp(current_key, "log") == 0) {
                    in_log_section = 1;
                    found_log_section = 1;
                }
            } else if (depth == 2 && strcmp(current_key, "log") == 0) {
                in_log_section = 1;
            } else {
                in_log_section = 0;
            }
            expect_key = 1;
            break;
        case YAML_MAPPING_END_EVENT:
            if (in_plugin_config) {
                /* Finalize JSON object. */
                if (plugin_json_len > 0 && plugin_json[plugin_json_len - 1] == '{') {
                    plugin_json[plugin_json_len] = '}';
                    plugin_json[plugin_json_len + 1] = '\0';
                } else {
                    plugin_json[plugin_json_len++] = '}';
                    plugin_json[plugin_json_len] = '\0';
                }
                in_plugin_config = 0;
                expect_key = 1;
                break;
            }
            if (in_plugin_entry) {
                /* Finalize this plugin entry: store path + JSON. */
                if (plugin_index < CLOG_MAX_PLUGINS && strlen(plugin_path) > 0) {
                    snprintf(cfg->plugin_so_paths[plugin_index], sizeof(cfg->plugin_so_paths[0]),
                             "%s", plugin_path);
                    snprintf(cfg->plugin_params_json[plugin_index],
                             sizeof(cfg->plugin_params_json[0]), "%s", plugin_json);
                    plugin_index++;
                    cfg->plugin_count = plugin_index;
                }
                in_plugin_entry = 0;
                expect_key = 1;
                break;
            }
            if (depth <= 2) {
                in_log_section = 0;
            }
            depth--;
            expect_key = 1;
            break;
        default:
            /* STREAM_START, DOCUMENT_START, DOCUMENT_END, STREAM_END, sequences — no action */
            break;
        }

        int is_end = (event.type == YAML_STREAM_END_EVENT);
        yaml_event_delete(&event);

        if (is_end)
            break;
    }

    yaml_parser_delete(&parser);
    fclose(f);
    return has_errors ? -1 : 0;
}


/* ── Instance API ── */

int log_config_load_into(logger_t *logger, const char *yaml_path) {
    char local_format[CLOG_MAX_FORMAT_SIZE] = "";
    char local_time_format[64] = "";

    logger->config.level = LOG_LEVEL_INFO;
    logger->config.async = false;
    logger->config.queue_size = 8192;
    logger->config.color = true;
    logger->config.console_enable = 1;
    logger->config.console_stderr = 0;
    logger->config.file_enable = 0;
    logger->config.file_path[0] = '\0';
    logger->config.file_max_size = (uint64_t)100 * 1024 * 1024;
    logger->config.file_backups = 10;
    logger->config.socket_enable = 0;
    logger->config.socket_host[0] = '\0';
    logger->config.socket_port = 0;
    logger->config.socket_tls = false;
    logger->config.socket_tls_ca_file[0] = '\0';
    logger->config.socket_tls_skip_verify = false;
    logger->config.rate_limit_enable = false;
    logger->config.rate_limit_max_per_sec = 0;
    logger->config.rate_limit_burst = 0;
    logger->config.catch_signals = true;

    snprintf(local_format, sizeof(local_format), "%s", "[%time] [%level] %msg");
    snprintf(local_time_format, sizeof(local_time_format), "%s", "%Y-%m-%d %H:%M:%S");

    /* Point format/time_format at logger-owned storage immediately — never leave
     * them pointing at a stack local, even on early-error paths. */
    snprintf(logger->format_str, sizeof(logger->format_str), "%s", local_format);
    snprintf(logger->time_format_str, sizeof(logger->time_format_str), "%s", local_time_format);
    logger->config.format = logger->format_str;
    logger->config.time_format = logger->time_format_str;

    if (yaml_path && yaml_path != logger->config_path && strlen(yaml_path) > 0) {
        snprintf(logger->config_path, sizeof(logger->config_path), "%s", yaml_path);
    } else if (!yaml_path || yaml_path[0] == '\0') {
        logger->config_path[0] = '\0';
    }

    if (logger->config_path[0] != '\0' && clog_access(logger->config_path, R_OK) == 0) {
        /* parse_config_file writes to cfg->format / cfg->time_format — these will
         * point into g_config_format from within YAML_HEAD .c. After parse we
         * re-copy back into logger-owned storage below. */
        int ret = parse_config_file(logger->config_path, &logger->config);
        if (ret != 0)
            return ret;
        const char *final_fmt = logger->config.format ? logger->config.format : local_format;
        const char *final_tf =
            logger->config.time_format ? logger->config.time_format : local_time_format;
        snprintf(logger->format_str, sizeof(logger->format_str), "%s", final_fmt);
        snprintf(logger->time_format_str, sizeof(logger->time_format_str), "%s", final_tf);
        logger->config.format = logger->format_str;
        logger->config.time_format = logger->time_format_str;
    }

    return 0;
}

/* ── Singleton API ── */

log_config_t *log_config_get(void) {
    return &g_default_logger.config;
}

static int apply_config(const log_config_t *cfg) {
    g_default_logger.config.level = cfg->level;
    g_default_logger.config.async = cfg->async;
    g_default_logger.config.queue_size = cfg->queue_size;
    g_default_logger.config.color = cfg->color;
    g_default_logger.config.console_enable = cfg->console_enable;
    g_default_logger.config.console_stderr = cfg->console_stderr;
    g_default_logger.config.file_enable = cfg->file_enable;
    g_default_logger.config.file_max_size = cfg->file_max_size;
    g_default_logger.config.file_backups = cfg->file_backups;
    g_default_logger.config.socket_enable = cfg->socket_enable;
    snprintf(g_default_logger.config.file_path, sizeof(g_default_logger.config.file_path), "%s",
             cfg->file_path);
    snprintf(g_default_logger.config.socket_host, sizeof(g_default_logger.config.socket_host), "%s",
             cfg->socket_host);
    g_default_logger.config.socket_port = cfg->socket_port;
    g_default_logger.config.socket_tls = cfg->socket_tls;
    snprintf(g_default_logger.config.socket_tls_ca_file,
             sizeof(g_default_logger.config.socket_tls_ca_file), "%s", cfg->socket_tls_ca_file);
    g_default_logger.config.socket_tls_skip_verify = cfg->socket_tls_skip_verify;
    g_default_logger.config.rate_limit_enable = cfg->rate_limit_enable;
    g_default_logger.config.rate_limit_max_per_sec = cfg->rate_limit_max_per_sec;
    g_default_logger.config.rate_limit_burst = cfg->rate_limit_burst;
    g_default_logger.config.catch_signals = cfg->catch_signals;
    g_default_logger.config.prometheus_enable = cfg->prometheus_enable;
    g_default_logger.config.prometheus_port = cfg->prometheus_port;

    g_default_logger.config.plugin_count = cfg->plugin_count;
    for (int i = 0; i < cfg->plugin_count && i < CLOG_MAX_PLUGINS; i++) {
        snprintf(g_default_logger.config.plugin_so_paths[i],
                 sizeof(g_default_logger.config.plugin_so_paths[0]), "%s",
                 cfg->plugin_so_paths[i]);
        snprintf(g_default_logger.config.plugin_params_json[i],
                 sizeof(g_default_logger.config.plugin_params_json[0]), "%s",
                 cfg->plugin_params_json[i]);
    }

    if (cfg->format) {
        snprintf(g_default_logger.format_str, sizeof(g_default_logger.format_str), "%s",
                 cfg->format);
        g_default_logger.config.format = g_default_logger.format_str;
    }
    if (cfg->time_format) {
        snprintf(g_default_logger.time_format_str, sizeof(g_default_logger.time_format_str), "%s",
                 cfg->time_format);
        g_default_logger.config.time_format = g_default_logger.time_format_str;
    }

    return 0;
}

int log_config_set(const log_config_t *cfg) {
    if (!cfg)
        return -1;
    clog_rwlock_wrlock(&g_default_logger.config_rwlock);
    int ret = apply_config(cfg);
    clog_rwlock_wrunlock(&g_default_logger.config_rwlock);
    return ret;
}

int log_config_init(const char *yaml_path) {
    if (!yaml_path)
        yaml_path = "";
    return log_config_load_into(&g_default_logger, yaml_path);
}

int log_config_reload(void) {
    return log_config_load_into(&g_default_logger, g_default_logger.config_path);
}

int log_set_level(log_level_t level) {
    clog_rwlock_wrlock(&g_default_logger.config_rwlock);
    g_default_logger.config.level = level;
    clog_rwlock_wrunlock(&g_default_logger.config_rwlock);
    return 0;
}

log_level_t log_get_level(void) {
    clog_rwlock_rdlock(&g_default_logger.config_rwlock);
    log_level_t lvl = g_default_logger.config.level;
    clog_rwlock_rdunlock(&g_default_logger.config_rwlock);
    return lvl;
}

bool log_config_is_async(void) {
    clog_rwlock_rdlock(&g_default_logger.config_rwlock);
    bool async = g_default_logger.config.async;
    clog_rwlock_rdunlock(&g_default_logger.config_rwlock);
    return async;
}

bool log_config_color_enabled(void) {
    clog_rwlock_rdlock(&g_default_logger.config_rwlock);
    bool color = g_default_logger.config.color;
    clog_rwlock_rdunlock(&g_default_logger.config_rwlock);
    return color;
}
