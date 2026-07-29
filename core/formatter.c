/**
 * @file formatter.c
 * @brief Token formatter and JSON renderer for log lines.
 *
 * The format string is compiled into an opcode sequence at init time
 * (fmt_compile), so the hot path (log_formatter_format) dispatches
 * through a flat switch statement with zero format-string re-parsing.
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

/* ------------------------------------------------------------------ */
/*  Global state                                                      */
/* ------------------------------------------------------------------ */

static char g_default_format[CLOG_MAX_FORMAT_SIZE] = "%msg";
static char g_format_buf[CLOG_MAX_FORMAT_SIZE];
static char *g_format_ptr = g_default_format;
static char g_time_format_buf[64] = "%Y-%m-%d %H:%M:%S";
static clog_mutex_t g_format_mutex = CLOG_MUTEX_INITIALIZER;

/* Compiled opcode program (populated at init time). */
static fmt_op_t g_format_ops[FMT_MAX_OPS];
static int g_fmt_op_count = 1; /* default: one OP_LITERAL("%msg") */
static int g_fmt_is_json = 0;  /* JSON fast-path flag */

#define TIME_BUF_SIZE 64

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

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
    if (token_len >= *remaining)
        token_len = *remaining - 1;
    if (token_len > 0) {
        memcpy(*out, token, token_len);
        *out += token_len;
        *remaining -= token_len;
        **out = '\0';
    }
    return (int)token_len;
}

/* ------------------------------------------------------------------ */
/*  JSON escaping & rendering                                         */
/* ------------------------------------------------------------------ */

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
    strftime(time_buf, sizeof(time_buf), g_time_format_buf, &tm_buf);

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

/* ------------------------------------------------------------------ */
/*  Pattern compiler                                                   */
/* ------------------------------------------------------------------ */

/**
 * @brief Match a token name at the current format-string position.
 *
 * Returns 1 and advances @p fmt past the token if the next characters
 * match @p token and are followed by a non-alpha boundary.
 */
static int token_match(const char **fmt, const char *token) {
    size_t len = strlen(token);
    if (strncmp(*fmt, token, len) == 0) {
        unsigned char c = (unsigned char)(*fmt)[len];
        if (c == '\0' || !isalpha(c)) {
            *fmt += len;
            return 1;
        }
    }
    return 0;
}

/**
 * @brief Compile a format string into an opcode program.
 *
 * Walks @p fmt_str once, emitting one @ref fmt_op_t per literal
 * segment or token.  Literal pointers reference @p fmt_str memory
 * (the caller must ensure it outlives the ops array).
 *
 * @param[in]  fmt_str  Format string to compile.
 * @param[out] ops      Output opcode array (capacity @p max_ops).
 * @param[in]  max_ops  Capacity of @p ops.
 * @return Number of ops written (always < max_ops).
 */
static int fmt_compile(const char *fmt_str, fmt_op_t *ops, int max_ops) {
    int n = 0;
    while (*fmt_str && n < max_ops - 1) {
        if (*fmt_str != '%') {
            const char *start = fmt_str;
            while (*fmt_str && *fmt_str != '%')
                fmt_str++;
            size_t len = (size_t)(fmt_str - start);
            if (len > 0) {
                ops[n].op = FMT_OP_LITERAL;
                ops[n].literal = start;
                ops[n].literal_len = len;
                n++;
            }
            continue;
        }

        fmt_str++; /* skip '%' */

        if (token_match(&fmt_str, "time"))
            ops[n++].op = FMT_OP_TIME;
        else if (token_match(&fmt_str, "level"))
            ops[n++].op = FMT_OP_LEVEL;
        else if (token_match(&fmt_str, "msg"))
            ops[n++].op = FMT_OP_MSG;
        else if (token_match(&fmt_str, "thread"))
            ops[n++].op = FMT_OP_THREAD;
        else if (token_match(&fmt_str, "pid"))
            ops[n++].op = FMT_OP_PID;
        else if (token_match(&fmt_str, "file"))
            ops[n++].op = FMT_OP_FILE;
        else if (token_match(&fmt_str, "line"))
            ops[n++].op = FMT_OP_LINE;
        else if (token_match(&fmt_str, "func"))
            ops[n++].op = FMT_OP_FUNC;
        else if (token_match(&fmt_str, "module"))
            ops[n++].op = FMT_OP_MODULE;
        else if (token_match(&fmt_str, "tag"))
            ops[n++].op = FMT_OP_TAG;
        else if (token_match(&fmt_str, "newline"))
            ops[n++].op = FMT_OP_NEWLINE;
        else {
            /* Unknown token — emit '%' as a literal. */
            ops[n].op = FMT_OP_LITERAL;
            ops[n].literal = fmt_str - 1;
            ops[n].literal_len = 1;
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/*  log_formatter_format — hot path                                   */
/* ------------------------------------------------------------------ */

int log_formatter_format(log_record_t *restrict record, char *restrict buf, size_t buf_size) {
    int is_json;
    int op_count;
    const char *tf;

    clog_mutex_lock(&g_format_mutex);
    is_json = g_fmt_is_json;
    op_count = g_fmt_op_count;
    tf = g_time_format_buf;
    clog_mutex_unlock(&g_format_mutex);

    if (is_json)
        return format_json(record, buf, buf_size);

    char *out = buf;
    size_t remaining = buf_size;
    int total = 0;
    struct tm tm_buf;
    int tm_initialized = 0;

    for (int i = 0; i < op_count; i++) {
        switch (g_format_ops[i].op) {

        case FMT_OP_LITERAL:
            total += append_token(&out, &remaining, g_format_ops[i].literal,
                                  g_format_ops[i].literal_len);
            break;

        case FMT_OP_TIME: {
            time_t sec = (time_t)(record->timestamp / 1000000);
            if (!tm_initialized) {
                clog_localtime_r(&sec, &tm_buf);
                tm_initialized = 1;
            }
            char time_buf[TIME_BUF_SIZE];
            strftime(time_buf, sizeof(time_buf), tf, &tm_buf);
            total += append_token(&out, &remaining, time_buf, strlen(time_buf));
            break;
        }

        case FMT_OP_LEVEL: {
            const char *s = level_to_string(record->level);
            total += append_token(&out, &remaining, s, strlen(s));
            break;
        }

        case FMT_OP_MSG: {
            const char *msg = record->message ? record->message : "(no message)";
            total += append_token(&out, &remaining, msg, strlen(msg));
            break;
        }

        case FMT_OP_THREAD: {
            char tmp[32];
            int n = snprintf(tmp, sizeof(tmp), "%u", record->tid);
            if (n > 0)
                total += append_token(&out, &remaining, tmp, (size_t)n);
            break;
        }

        case FMT_OP_PID: {
            char tmp[32];
            int n = snprintf(tmp, sizeof(tmp), "%u", record->pid);
            if (n > 0)
                total += append_token(&out, &remaining, tmp, (size_t)n);
            break;
        }

        case FMT_OP_FILE: {
            const char *s = record->file ? record->file : "(unknown)";
            total += append_token(&out, &remaining, s, strlen(s));
            break;
        }

        case FMT_OP_LINE: {
            char tmp[32];
            int n = snprintf(tmp, sizeof(tmp), "%d", record->line);
            if (n > 0)
                total += append_token(&out, &remaining, tmp, (size_t)n);
            break;
        }

        case FMT_OP_FUNC: {
            const char *s = record->func ? record->func : "(unknown)";
            total += append_token(&out, &remaining, s, strlen(s));
            break;
        }

        case FMT_OP_MODULE: {
            const char *s = record->module ? record->module : "(unknown)";
            total += append_token(&out, &remaining, s, strlen(s));
            break;
        }

        case FMT_OP_TAG: {
            const char *s = record->tag ? record->tag : "";
            total += append_token(&out, &remaining, s, strlen(s));
            break;
        }

        case FMT_OP_NEWLINE: {
            if (remaining > 1) {
                *out++ = '\n';
                remaining--;
                total++;
            }
            break;
        }
        }
    }

    *out = '\0';
    return total;
}

/* ------------------------------------------------------------------ */
/*  Init / reset                                                      */
/* ------------------------------------------------------------------ */

int log_formatter_init(const char *format, const char *time_format) {
    clog_mutex_lock(&g_format_mutex);

    if (format && format[0]) {
        snprintf(g_format_buf, sizeof(g_format_buf), "%s", format);
        g_format_ptr = g_format_buf;
    } else {
        g_format_ptr = g_default_format;
    }

    if (time_format && time_format[0]) {
        snprintf(g_time_format_buf, sizeof(g_time_format_buf), "%s", time_format);
    } else {
        snprintf(g_time_format_buf, sizeof(g_time_format_buf), "%s", "%Y-%m-%d %H:%M:%S");
    }

    g_fmt_is_json = (strcmp(g_format_ptr, "json") == 0 || strcmp(g_format_ptr, "JSON") == 0);

    if (!g_fmt_is_json)
        g_fmt_op_count = fmt_compile(g_format_ptr, g_format_ops, FMT_MAX_OPS);

    clog_mutex_unlock(&g_format_mutex);
    return 0;
}

void log_formatter_reset(void) {
    clog_mutex_lock(&g_format_mutex);
    g_format_ptr = g_default_format;
    g_fmt_is_json = 0;
    g_fmt_op_count = fmt_compile(g_default_format, g_format_ops, FMT_MAX_OPS);
    clog_mutex_unlock(&g_format_mutex);
}

const char *log_formatter_get_format(void) {
    const char *fmt;
    clog_mutex_lock(&g_format_mutex);
    fmt = g_format_ptr;
    clog_mutex_unlock(&g_format_mutex);
    return fmt;
}
