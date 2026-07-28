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

static log_config_t g_config;
static clog_rwlock_t g_config_rwlock = CLOG_RWLOCK_INITIALIZER;
static char g_config_format[512] = "";
static char g_config_time_format[64] = "";
static char g_config_path[512] = "./config.yaml";

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

    while (1) {
        if (!yaml_parser_parse(&parser, &event)) {
            fprintf(stderr, "YAML parse error: %s\n", filepath);
            has_errors = 1;
            break;
        }

        switch (event.type) {
        case YAML_SCALAR_EVENT: {
            const char *val = (const char *)event.data.scalar.value;

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
                    }
                }
                /* unknown keys are silently skipped */
            }
            break;
        }
        case YAML_MAPPING_START_EVENT:
            depth++;
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
            if (depth == 1) {
                in_log_section = 0;
            } else if (depth == 2 && in_log_section) {
                in_log_section = 0;
            }
            depth--;
            expect_key = 1;
            break;
        case YAML_SEQUENCE_START_EVENT:
        case YAML_SEQUENCE_END_EVENT:
            /* sequences not supported — silently skip */
            break;
        default:
            /* STREAM_START, DOCUMENT_START, DOCUMENT_END, STREAM_END — no action */
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

static int load_default_and_apply(const char *yaml_path) {
    g_config.level = LOG_LEVEL_INFO;
    g_config.async = false;
    g_config.queue_size = 8192;
    g_config.color = true;
    strcpy(g_config_format, "[%time] [%level] %msg");
    g_config.console_enable = 1;
    g_config.console_stderr = 0;
    g_config.file_enable = 0;
    g_config.file_path[0] = '\0';
    g_config.file_max_size = 100 * 1024 * 1024;
    g_config.file_backups = 10;
    g_config.socket_enable = 0;
    g_config.socket_host[0] = '\0';
    g_config.socket_port = 0;
    g_config.format = g_config_format;
    snprintf(g_config_time_format, sizeof(g_config_time_format), "%s", "%Y-%m-%d %H:%M:%S");
    g_config.time_format = g_config_time_format;

    /* Reload passes g_config_path itself; avoid overlapping copy (ASan). */
    if (yaml_path && yaml_path != g_config_path && strlen(yaml_path) > 0) {
        snprintf(g_config_path, sizeof(g_config_path), "%s", yaml_path);
    } else if (!yaml_path || yaml_path[0] == '\0') {
        g_config_path[0] = '\0';
    }

    if (clog_access(g_config_path, R_OK) == 0) {
        return parse_config_file(g_config_path, &g_config);
    }

    return 0;
}

log_config_t *log_config_get(void) {
    return &g_config;
}

static int apply_config(const log_config_t *cfg) {
    g_config.level = cfg->level;
    g_config.async = cfg->async;
    g_config.queue_size = cfg->queue_size;
    g_config.color = cfg->color;
    g_config.console_enable = cfg->console_enable;
    g_config.console_stderr = cfg->console_stderr;
    g_config.file_enable = cfg->file_enable;
    g_config.file_max_size = cfg->file_max_size;
    g_config.file_backups = cfg->file_backups;
    g_config.socket_enable = cfg->socket_enable;
    snprintf(g_config.file_path, sizeof(g_config.file_path), "%s", cfg->file_path);
    snprintf(g_config.socket_host, sizeof(g_config.socket_host), "%s", cfg->socket_host);
    g_config.socket_port = cfg->socket_port;
    g_config.socket_tls = cfg->socket_tls;
    snprintf(g_config.socket_tls_ca_file, sizeof(g_config.socket_tls_ca_file), "%s",
             cfg->socket_tls_ca_file);
    g_config.socket_tls_skip_verify = cfg->socket_tls_skip_verify;
    g_config.rate_limit_enable = cfg->rate_limit_enable;
    g_config.rate_limit_max_per_sec = cfg->rate_limit_max_per_sec;
    g_config.rate_limit_burst = cfg->rate_limit_burst;
    g_config.catch_signals = cfg->catch_signals;

    if (cfg->format) {
        snprintf(g_config_format, sizeof(g_config_format), "%s", cfg->format);
        g_config.format = g_config_format;
    }
    if (cfg->time_format) {
        snprintf(g_config_time_format, sizeof(g_config_time_format), "%s", cfg->time_format);
        g_config.time_format = g_config_time_format;
    }

    return 0;
}

int log_config_set(const log_config_t *cfg) {
    if (!cfg)
        return -1;
    clog_rwlock_wrlock(&g_config_rwlock);
    int ret = apply_config(cfg);
    clog_rwlock_wrunlock(&g_config_rwlock);
    return ret;
}

int log_config_init(const char *yaml_path) {
    if (!yaml_path)
        yaml_path = "";
    clog_rwlock_wrlock(&g_config_rwlock);
    int ret = load_default_and_apply(yaml_path);
    clog_rwlock_wrunlock(&g_config_rwlock);
    return ret;
}

int log_config_reload(void) {
    clog_rwlock_wrlock(&g_config_rwlock);
    int ret = load_default_and_apply(g_config_path);
    clog_rwlock_wrunlock(&g_config_rwlock);
    return ret;
}

int log_set_level(log_level_t level) {
    clog_rwlock_wrlock(&g_config_rwlock);
    g_config.level = level;
    clog_rwlock_wrunlock(&g_config_rwlock);
    return 0;
}

log_level_t log_get_level(void) {
    clog_rwlock_rdlock(&g_config_rwlock);
    log_level_t lvl = g_config.level;
    clog_rwlock_rdunlock(&g_config_rwlock);
    return lvl;
}

bool log_config_is_async(void) {
    clog_rwlock_rdlock(&g_config_rwlock);
    bool async = g_config.async;
    clog_rwlock_rdunlock(&g_config_rwlock);
    return async;
}

bool log_config_color_enabled(void) {
    clog_rwlock_rdlock(&g_config_rwlock);
    bool color = g_config.color;
    clog_rwlock_rdunlock(&g_config_rwlock);
    return color;
}
