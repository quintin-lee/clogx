/**
 * @file formatter.c
 * @brief Token formatter and JSON renderer for log lines.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "clog_port.h"
#include "log_formatter.h"
#include "log_limits.h"
#include "log_record.h"

static char g_default_format[CLOG_MAX_FORMAT_SIZE] = "%msg";
static char g_format_buf[CLOG_MAX_FORMAT_SIZE];
static char *g_format_ptr = g_default_format;
static char g_time_format_buf[64] = "%Y-%m-%d %H:%M:%S";
static clog_mutex_t g_format_mutex = CLOG_MUTEX_INITIALIZER;

#define TIME_BUF_SIZE 64

static const char *level_to_string(log_level_t level) {
    switch (level) {
    case LOG_LEVEL_TRACE:
        return "TRACE";
    case LOG_LEVEL_DEBUG:
        return "DEBUG";
    case LOG_LEVEL_INFO:
        return "INFO";
    case LOG_LEVEL_WARN:
        return "WARN";
    case LOG_LEVEL_ERROR:
        return "ERROR";
    case LOG_LEVEL_FATAL:
        return "FATAL";
    default:
        return "UNKNOWN";
    }
}

static int append_token(char **out, size_t *remaining, const char *token, size_t token_len) {
    if (token_len >= *remaining) {
        token_len = *remaining - 1;
    }
    memcpy(*out, token, token_len);
    *out += token_len;
    *remaining -= token_len;
    **out = '\0';
    return (int)token_len;
}

static void append_json_escaped_string(char **out, size_t *remaining, const char *str) {
    if (!str)
        str = "";
    while (*str && *remaining > 1) {
        unsigned char c = (unsigned char)*str++;
        if (c == '"') {
            if (*remaining <= 2)
                break;
            *(*out)++ = '\\';
            *(*out)++ = '"';
            *remaining -= 2;
        } else if (c == '\\') {
            if (*remaining <= 2)
                break;
            *(*out)++ = '\\';
            *(*out)++ = '\\';
            *remaining -= 2;
        } else if (c == '\b') {
            if (*remaining <= 2)
                break;
            *(*out)++ = '\\';
            *(*out)++ = 'b';
            *remaining -= 2;
        } else if (c == '\f') {
            if (*remaining <= 2)
                break;
            *(*out)++ = '\\';
            *(*out)++ = 'f';
            *remaining -= 2;
        } else if (c == '\n') {
            if (*remaining <= 2)
                break;
            *(*out)++ = '\\';
            *(*out)++ = 'n';
            *remaining -= 2;
        } else if (c == '\r') {
            if (*remaining <= 2)
                break;
            *(*out)++ = '\\';
            *(*out)++ = 'r';
            *remaining -= 2;
        } else if (c == '\t') {
            if (*remaining <= 2)
                break;
            *(*out)++ = '\\';
            *(*out)++ = 't';
            *remaining -= 2;
        } else if (c < 0x20) {
            if (*remaining <= 6)
                break;
            int n = snprintf(*out, *remaining, "\\u00%02x", c);
            if (n > 0 && (size_t)n < *remaining) {
                *out += n;
                *remaining -= (size_t)n;
            } else {
                break;
            }
        } else {
            *(*out)++ = (char)c;
            (*remaining)--;
        }
    }
    **out = '\0';
}

static int format_json(log_record_t *restrict record, char *restrict buf, size_t buf_size) {
    struct tm tm_buf;
    time_t sec = (time_t)(record->timestamp / 1000000);
    uint32_t usec = (uint32_t)(record->timestamp % 1000000);
    clog_localtime_r(&sec, &tm_buf);

    char time_buf[64];
    const char *tf;
    clog_mutex_lock(&g_format_mutex);
    tf = g_time_format_buf;
    clog_mutex_unlock(&g_format_mutex);
    strftime(time_buf, sizeof(time_buf), tf, &tm_buf);

    char *out = buf;
    size_t remaining = buf_size;

    int ret = snprintf(out, remaining, "{\"timestamp\":\"%s.%06u\",\"level\":\"%s\",\"module\":\"",
                       time_buf, usec, level_to_string(record->level));
    if (ret <= 0 || (size_t)ret >= remaining)
        return -1;
    out += ret;
    remaining -= (size_t)ret;

    append_json_escaped_string(&out, &remaining, record->module ? record->module : "");

    ret = snprintf(out, remaining, "\",\"file\":\"");
    if (ret <= 0 || (size_t)ret >= remaining)
        return -1;
    out += ret;
    remaining -= (size_t)ret;

    append_json_escaped_string(&out, &remaining, record->file ? record->file : "");

    ret = snprintf(out, remaining, "\",\"line\":%d,\"func\":\"", record->line);
    if (ret <= 0 || (size_t)ret >= remaining)
        return -1;
    out += ret;
    remaining -= (size_t)ret;

    append_json_escaped_string(&out, &remaining, record->func ? record->func : "");

    ret = snprintf(out, remaining, "\",\"thread\":%u,\"pid\":%u,\"tag\":\"", record->tid,
                   record->pid);
    if (ret <= 0 || (size_t)ret >= remaining)
        return -1;
    out += ret;
    remaining -= (size_t)ret;

    append_json_escaped_string(&out, &remaining, record->tag ? record->tag : "");

    ret = snprintf(out, remaining, "\",\"message\":\"");
    if (ret <= 0 || (size_t)ret >= remaining)
        return -1;
    out += ret;
    remaining -= (size_t)ret;

    append_json_escaped_string(&out, &remaining, record->message ? record->message : "");

    ret = snprintf(out, remaining, "\"}");
    if (ret <= 0 || (size_t)ret >= remaining)
        return -1;
    out += ret;
    remaining -= (size_t)ret;

    return (int)(out - buf);
}

int log_formatter_format(log_record_t *restrict record, char *restrict buf, size_t buf_size) {
    const char *fmt;
    clog_mutex_lock(&g_format_mutex);
    fmt = g_format_ptr;
    clog_mutex_unlock(&g_format_mutex);

    if (strcmp(fmt, "json") == 0 || strcmp(fmt, "JSON") == 0) {
        return format_json(record, buf, buf_size);
    }

    char *out = buf;
    size_t remaining = buf_size;
    int total = 0;

    while (*fmt && remaining > 1) {
        if (*fmt != '%') {
            *out++ = *fmt++;
            remaining--;
            total++;
            continue;
        }

        fmt++;

        if (strncmp(fmt, "time", 4) == 0 && (fmt[4] == '\0' || !isalpha(fmt[4]))) {
            fmt += 4;
            struct tm tm_buf;
            time_t sec = (time_t)(record->timestamp / 1000000);
            clog_localtime_r(&sec, &tm_buf);
            char time_buf[TIME_BUF_SIZE];
            const char *tf;
            clog_mutex_lock(&g_format_mutex);
            tf = g_time_format_buf;
            clog_mutex_unlock(&g_format_mutex);
            strftime(time_buf, sizeof(time_buf), tf, &tm_buf);
            size_t tlen = strlen(time_buf);
            if (tlen >= sizeof(time_buf))
                tlen = sizeof(time_buf) - 1;
            append_token(&out, &remaining, time_buf, tlen);
            total += (int)tlen;
        } else if (strncmp(fmt, "level", 5) == 0 && (fmt[5] == '\0' || !isalpha(fmt[5]))) {
            fmt += 5;
            const char *level_str = level_to_string(record->level);
            size_t len = strlen(level_str);
            append_token(&out, &remaining, level_str, len);
            total += (int)len;
        } else if (strncmp(fmt, "msg", 3) == 0 && (fmt[3] == '\0' || !isalpha(fmt[3]))) {
            fmt += 3;
            const char *msg = record->message ? record->message : "(no message)";
            size_t len = strlen(msg);
            append_token(&out, &remaining, msg, len);
            total += (int)len;
        } else if (strncmp(fmt, "thread", 6) == 0 && (fmt[6] == '\0' || !isalpha(fmt[6]))) {
            fmt += 6;
            char thread_buf[32];
            int ret = snprintf(thread_buf, sizeof(thread_buf), "%u", record->tid);
            if (ret > 0) {
                append_token(&out, &remaining, thread_buf, (size_t)ret);
                total += ret;
            }
        } else if (strncmp(fmt, "pid", 3) == 0 && (fmt[3] == '\0' || !isalpha(fmt[3]))) {
            fmt += 3;
            char pid_buf[32];
            int ret = snprintf(pid_buf, sizeof(pid_buf), "%u", record->pid);
            if (ret > 0) {
                append_token(&out, &remaining, pid_buf, (size_t)ret);
                total += ret;
            }
        } else if (strncmp(fmt, "file", 4) == 0 && (fmt[4] == '\0' || !isalpha(fmt[4]))) {
            fmt += 4;
            const char *file = record->file ? record->file : "(unknown)";
            size_t len = strlen(file);
            append_token(&out, &remaining, file, len);
            total += (int)len;
        } else if (strncmp(fmt, "line", 4) == 0 && (fmt[4] == '\0' || !isalpha(fmt[4]))) {
            fmt += 4;
            char line_buf[32];
            int ret = snprintf(line_buf, sizeof(line_buf), "%d", record->line);
            if (ret > 0) {
                append_token(&out, &remaining, line_buf, (size_t)ret);
                total += ret;
            }
        } else if (strncmp(fmt, "func", 4) == 0 && (fmt[4] == '\0' || !isalpha(fmt[4]))) {
            fmt += 4;
            const char *func = record->func ? record->func : "(unknown)";
            size_t len = strlen(func);
            append_token(&out, &remaining, func, len);
            total += (int)len;
        } else if (strncmp(fmt, "module", 6) == 0 && (fmt[6] == '\0' || !isalpha(fmt[6]))) {
            fmt += 6;
            const char *module = record->module ? record->module : "(unknown)";
            size_t len = strlen(module);
            append_token(&out, &remaining, module, len);
            total += (int)len;
        } else if (strncmp(fmt, "tag", 3) == 0 && (fmt[3] == '\0' || !isalpha(fmt[3]))) {
            fmt += 3;
            const char *tag = record->tag ? record->tag : "";
            size_t len = strlen(tag);
            append_token(&out, &remaining, tag, len);
            total += (int)len;
        } else if (strncmp(fmt, "newline", 7) == 0 && (fmt[7] == '\0' || !isalpha(fmt[7]))) {
            fmt += 7;
            if (remaining > 1) {
                *out++ = '\n';
                remaining--;
                total++;
            }
        } else {
            *out++ = '%';
            remaining--;
            total++;
        }
    }

    *out = '\0';
    return total;
}

int log_formatter_init(const char *format, const char *time_format) {
    clog_mutex_lock(&g_format_mutex);
    if (format && strlen(format) > 0) {
        snprintf(g_format_buf, sizeof(g_format_buf), "%s", format);
        g_format_ptr = g_format_buf;
    } else {
        g_format_ptr = g_default_format;
    }
    if (time_format && strlen(time_format) > 0) {
        snprintf(g_time_format_buf, sizeof(g_time_format_buf), "%s", time_format);
    } else {
        snprintf(g_time_format_buf, sizeof(g_time_format_buf), "%s", "%Y-%m-%d %H:%M:%S");
    }
    clog_mutex_unlock(&g_format_mutex);
    return 0;
}

void log_formatter_reset(void) {
    clog_mutex_lock(&g_format_mutex);
    g_format_ptr = g_default_format;
    clog_mutex_unlock(&g_format_mutex);
}

const char *log_formatter_get_format(void) {
    const char *fmt;
    clog_mutex_lock(&g_format_mutex);
    fmt = g_format_ptr;
    clog_mutex_unlock(&g_format_mutex);
    return fmt;
}
