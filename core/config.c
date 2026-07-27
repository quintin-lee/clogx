/**
 * @file config.c
 * @brief key:value config parser with defaults, rwlock, and reload path memory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include "log_config.h"
#include "log_formatter.h"

static log_config_t g_config;
static pthread_rwlock_t g_config_rwlock = PTHREAD_RWLOCK_INITIALIZER;
static char g_config_format[512] = "";
static char g_config_path[512] = "./config.yaml";

static void trim(char *s) {
    char *start;
    char *end;

    if (!s || !*s)
        return;

    start = s;
    while (*start == ' ' || *start == '\t')
        start++;

    end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
        end--;

    *end = '\0';
    if (start != s) {
        memmove(s, start, (size_t)(end - start) + 1);
    }
}

static int parse_config_file(const char *filepath, log_config_t *cfg) {
    FILE *f = fopen(filepath, "r");
    if (!f) {
        perror("Failed to open config file");
        return -1;
    }

    char line[1024];
    int has_errors = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
            line[len - 1] = '\0';
            len--;
        }
        if (*line == '\0' || *line == '#')
            continue;

        char *colon = strchr(line, ':');
        if (!colon)
            continue;

        *colon = '\0';
        char *key = line;
        trim(key);

        char *value = colon + 1;
        trim(value);

        if (strcmp(key, "level") == 0) {
            if (strcmp(value, "TRACE") == 0)
                cfg->level = LOG_LEVEL_TRACE;
            else if (strcmp(value, "DEBUG") == 0)
                cfg->level = LOG_LEVEL_DEBUG;
            else if (strcmp(value, "INFO") == 0)
                cfg->level = LOG_LEVEL_INFO;
            else if (strcmp(value, "WARN") == 0)
                cfg->level = LOG_LEVEL_WARN;
            else if (strcmp(value, "ERROR") == 0)
                cfg->level = LOG_LEVEL_ERROR;
            else if (strcmp(value, "FATAL") == 0)
                cfg->level = LOG_LEVEL_FATAL;
            else {
                fprintf(stderr, "Unknown log level: %s\n", value);
                has_errors = 1;
            }
        } else if (strcmp(key, "async") == 0) {
            cfg->async = (strcmp(value, "true") == 0);
        } else if (strcmp(key, "queue_size") == 0) {
            int qs = atoi(value);
            if (qs <= 0) {
                fprintf(stderr, "Invalid queue_size: %s (must be > 0)\n", value);
                has_errors = 1;
            } else {
                cfg->queue_size = qs;
            }
        } else if (strcmp(key, "color") == 0) {
            cfg->color = (strcmp(value, "true") == 0);
        } else if (strcmp(key, "format") == 0) {
            strncpy(g_config_format, value, sizeof(g_config_format) - 1);
            g_config_format[sizeof(g_config_format) - 1] = '\0';
            cfg->format = g_config_format;
        } else if (strcmp(key, "console_enable") == 0) {
            cfg->console_enable = (strcmp(value, "true") == 0);
        } else if (strcmp(key, "console_stderr") == 0) {
            cfg->console_stderr = (strcmp(value, "true") == 0);
        } else if (strcmp(key, "file_enable") == 0) {
            cfg->file_enable = (strcmp(value, "true") == 0);
        } else if (strcmp(key, "socket_enable") == 0) {
            cfg->socket_enable = (strcmp(value, "true") == 0);
        } else if (strcmp(key, "path") == 0 || strcmp(key, "file_path") == 0) {
            strncpy(cfg->file_path, value, sizeof(cfg->file_path) - 1);
            cfg->file_path[sizeof(cfg->file_path) - 1] = '\0';
            cfg->file_enable = 1;
        } else if (strcmp(key, "max_size") == 0) {
            char size_buf[64];
            size_t vlen = strlen(value);
            char *end = NULL;
            unsigned long long n;
            uint64_t mult = 1;

            if (vlen == 0 || vlen >= sizeof(size_buf)) {
                fprintf(stderr, "Invalid max_size: %s\n", value);
                has_errors = 1;
                continue;
            }
            memcpy(size_buf, value, vlen + 1);

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

            n = strtoull(size_buf, &end, 10);
            if (end == size_buf || *end != '\0') {
                fprintf(stderr, "Invalid max_size: %s\n", value);
                has_errors = 1;
            } else {
                cfg->file_max_size = (uint64_t)n * mult;
            }
        } else if (strcmp(key, "backup") == 0 || strcmp(key, "backups") == 0) {
            int bk = atoi(value);
            if (bk < 0) {
                fprintf(stderr, "Invalid backups: %s (must be >= 0)\n", value);
                has_errors = 1;
            } else {
                cfg->file_backups = bk;
            }
        } else if (strcmp(key, "host") == 0) {
            strncpy(cfg->socket_host, value, sizeof(cfg->socket_host) - 1);
            cfg->socket_host[sizeof(cfg->socket_host) - 1] = '\0';
            cfg->socket_enable = 1;
        } else if (strcmp(key, "port") == 0) {
            int p = atoi(value);
            if (p <= 0 || p > 65535) {
                fprintf(stderr, "Invalid port: %s (must be 1..65535)\n", value);
                has_errors = 1;
            } else {
                cfg->socket_port = p;
            }
        }
    }
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

    /* Reload passes g_config_path itself; avoid overlapping copy (ASan). */
    if (yaml_path && yaml_path != g_config_path && strlen(yaml_path) > 0) {
        strncpy(g_config_path, yaml_path, sizeof(g_config_path) - 1);
        g_config_path[sizeof(g_config_path) - 1] = '\0';
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
