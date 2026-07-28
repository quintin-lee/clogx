/**
 * @file config.c
 * @brief YAML config parser via libyaml event API, with defaults, rwlock, and reload path memory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <yaml.h>
#include "log_config.h"
#include "log_formatter.h"

static log_config_t g_config;
static pthread_rwlock_t g_config_rwlock = PTHREAD_RWLOCK_INITIALIZER;
static char g_config_format[512] = "";
static char g_config_time_format[64] = "";
static char g_config_path[512] = "./config.yaml";

/**
 * @brief Parse a YAML config file using libyaml's event-based API.
 *
 * Looks for a top-level "log" mapping and processes all scalar key:value
 * pairs inside it (depth 2).  Other top-level keys and deeper nestings are
 * silently skipped so that a config can contain YAML structure other
 * sinks/providers may want.
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
    int depth = 0;          /* mapping nesting depth */
    int expect_key = 1;     /* 1 = expect key, 0 = expect value at current depth */
    int in_log_section = 0; /* 1 when we're inside the top-level "log" mapping */
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

            if (!in_log_section || depth != 2)
                break;

            if (expect_key) {
                snprintf(current_key, sizeof(current_key), "%s", val);
                expect_key = 0;
            } else {
                expect_key = 1; /* ready for next key */

                if (strcmp(current_key, "level") == 0) {
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
                } else if (strcmp(current_key, "async") == 0) {
                    cfg->async = (strcmp(val, "true") == 0);
                } else if (strcmp(current_key, "queue_size") == 0) {
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
                } else if (strcmp(current_key, "color") == 0) {
                    cfg->color = (strcmp(val, "true") == 0);
                } else if (strcmp(current_key, "format") == 0) {
                    snprintf(g_config_format, sizeof(g_config_format), "%s", val);
                    cfg->format = g_config_format;
                } else if (strcmp(current_key, "time_format") == 0) {
                    snprintf(g_config_time_format, sizeof(g_config_time_format), "%s", val);
                    cfg->time_format = g_config_time_format;
                } else if (strcmp(current_key, "console_enable") == 0) {
                    cfg->console_enable = (strcmp(val, "true") == 0);
                } else if (strcmp(current_key, "console_stderr") == 0) {
                    cfg->console_stderr = (strcmp(val, "true") == 0);
                } else if (strcmp(current_key, "file_enable") == 0) {
                    cfg->file_enable = (strcmp(val, "true") == 0);
                } else if (strcmp(current_key, "socket_enable") == 0) {
                    cfg->socket_enable = (strcmp(val, "true") == 0);
                } else if (strcmp(current_key, "path") == 0 ||
                           strcmp(current_key, "file_path") == 0) {
                    snprintf(cfg->file_path, sizeof(cfg->file_path), "%s", val);
                    cfg->file_enable = 1;
                } else if (strcmp(current_key, "max_size") == 0) {
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

                    /* Accept optional unit suffix: K/KB, M/MB, G/GB (case-insensitive). */
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
                } else if (strcmp(current_key, "backup") == 0 ||
                           strcmp(current_key, "backups") == 0) {
                    char *end = NULL;
                    errno = 0;
                    long bk = strtol(val, &end, 10);
                    if (end == val || *end != '\0' || errno == ERANGE || bk < 0) {
                        fprintf(stderr, "Invalid backups: %s (must be a non-negative integer)\n",
                                val);
                        has_errors = 1;
                    } else {
                        cfg->file_backups = (int)bk;
                    }
                } else if (strcmp(current_key, "host") == 0) {
                    snprintf(cfg->socket_host, sizeof(cfg->socket_host), "%s", val);
                    cfg->socket_enable = 1;
                } else if (strcmp(current_key, "port") == 0) {
                    char *end = NULL;
                    errno = 0;
                    long p = strtol(val, &end, 10);
                    if (end == val || *end != '\0' || errno == ERANGE || p <= 0 || p > 65535) {
                        fprintf(stderr, "Invalid port: %s (must be 1..65535)\n", val);
                        has_errors = 1;
                    } else {
                        cfg->socket_port = (int)p;
                    }
                }
                /* unknown keys at top-level are silently skipped */
            }
            break;
        }
        case YAML_MAPPING_START_EVENT:
            depth++;
            if (depth == 1) {
                in_log_section = 0;
            } else if (depth == 2) {
                in_log_section = 1;
            }
            expect_key = 1;
            break;
        case YAML_MAPPING_END_EVENT:
            if (depth == 1) {
                in_log_section = 0;
            } else if (depth == 2) {
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
    }

    if (access(g_config_path, R_OK) == 0) {
        return parse_config_file(g_config_path, &g_config);
    }

    return 0;
}

log_config_t *log_config_get(void) {
    return &g_config;
}

int log_config_init(const char *yaml_path) {
    if (!yaml_path)
        yaml_path = "";
    pthread_rwlock_wrlock(&g_config_rwlock);
    int ret = load_default_and_apply(yaml_path);
    pthread_rwlock_unlock(&g_config_rwlock);
    return ret;
}

int log_config_reload(void) {
    pthread_rwlock_wrlock(&g_config_rwlock);
    int ret = load_default_and_apply(g_config_path);
    pthread_rwlock_unlock(&g_config_rwlock);
    return ret;
}

int log_set_level(log_level_t level) {
    pthread_rwlock_wrlock(&g_config_rwlock);
    g_config.level = level;
    pthread_rwlock_unlock(&g_config_rwlock);
    return 0;
}

log_level_t log_get_level(void) {
    pthread_rwlock_rdlock(&g_config_rwlock);
    log_level_t lvl = g_config.level;
    pthread_rwlock_unlock(&g_config_rwlock);
    return lvl;
}

bool log_config_is_async(void) {
    pthread_rwlock_rdlock(&g_config_rwlock);
    bool async = g_config.async;
    pthread_rwlock_unlock(&g_config_rwlock);
    return async;
}

bool log_config_color_enabled(void) {
    pthread_rwlock_rdlock(&g_config_rwlock);
    bool color = g_config.color;
    pthread_rwlock_unlock(&g_config_rwlock);
    return color;
}
