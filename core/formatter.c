/**
 * @file formatter.c
 * @brief Token formatter and JSON renderer for log lines.
 *
 * ## Two-Stage Design
 *
 * The format string is compiled into an opcode sequence at init time
 * (`fmt_compile`), so the hot path (`log_formatter_format`) dispatches
 * through a flat switch statement with zero format-string re-parsing.
 *
 * 1. **Compile** (`log_formatter_compile`): Parse format string like
 *    `"[%time] [%level] %msg"` into an array of `fmt_token_t` opcodes.
 *    Literal text becomes `FMT_LITERAL` tokens; `%xxx` placeholders
 *    become `FMT_VAR` tokens with the variable type encoded.
 *    Runs once at logger init — cost is irrelevant.
 *
 * 2. **Render** (`log_formatter_format`): Walk the opcode array and
 *    write output to a `strbuf_t`. Each opcode is a case in a flat
 *    switch — no format-string parsing, no `printf` overhead.
 *    Runs on every log call — must be as fast as possible.
 *
 * ## Supported Tokens
 *
 * | Token     | Source                      | Output example               |
 * |-----------|-----------------------------|------------------------------|
 * | `%time`   | local wall-clock + µs       | `2026-07-31 10:30:45.123456` |
 * | `%level`  | log_level_t enum → string   | `INFO`                       |
 * | `%msg`    | caller-supplied fmt string  | `Server started`             |
 * | `%file`   | `__FILE__`                  | `app.c`                      |
 * | `%line`   | `__LINE__`                  | `42`                         |
 * | `%func`   | `__func__`                  | `main`                       |
 * | `%module` | `log_set_module()`          | `net.http`                   |
 * | `%tag`    | per-record tag              | `auth`                       |
 * | `%thread` | `pthread_self()` / TID      | `12345`                      |
 * | `%pid`    | `getpid()`                  | `678`                        |
 *
 * ## JSON Renderer (`log_formatter_format_otlp_json`)
 *
 * Renders a `log_record_t` as a single-line OTLP-compatible JSON object.
 * All string values are RFC 8259 escaped (`"`, `\`, `\n`, `\r`, `\t`,
 * control bytes). Used exclusively by `otlp_sink.c`.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "clog_port.h"
#include "log_formatter.h"
#include "log_limits.h"
#include "log_record.h"
#include "log_internal.h"

/* ------------------------------------------------------------------ */
/*  Global state                                                      */
/* ------------------------------------------------------------------ */

static char g_default_format[CLOG_MAX_FORMAT_SIZE] = "%msg";
static char g_time_format_buf[64] = "%Y-%m-%d %H:%M:%S";

/* Compiled opcode program (populated at init time). */

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

static void trace_id_hex(const uint8_t trace_id[16], char *out) {
    for (int i = 0; i < 16; i++)
        snprintf(out + (size_t)i * 2, 3, "%02x", trace_id[i]);
}

static void span_id_hex(const uint8_t span_id[8], char *out) {
    for (int i = 0; i < 8; i++)
        snprintf(out + (size_t)i * 2, 3, "%02x", span_id[i]);
}

static int is_zero_id(const uint8_t *id, int len) {
    for (int i = 0; i < len; i++)
        if (id[i])
            return 0;
    return 1;
}

static int format_json_ex(log_record_t *restrict record, char *restrict buf, size_t buf_size,
                          const char *time_format) {
    struct tm tm_buf;
    time_t sec = (time_t)(record->timestamp / 1000000);
    uint32_t usec = (uint32_t)(record->timestamp % 1000000);
    clog_localtime_r(&sec, &tm_buf);

    char time_buf[64];
    if (!time_format)
        time_format = "%Y-%m-%d %H:%M:%S";
    strftime(time_buf, sizeof(time_buf), time_format, &tm_buf);

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

    char tid_hex[33];
    char sid_hex[17];
    if (!is_zero_id(record->trace_id, 16)) {
        trace_id_hex(record->trace_id, tid_hex);
        ret = snprintf(out, remaining, "\",\"trace_id\":\"%s", tid_hex);
        if (ret <= 0 || (size_t)ret >= remaining)
            return -1;
        out += ret;
        remaining -= (size_t)ret;
    }
    if (!is_zero_id(record->span_id, 8)) {
        span_id_hex(record->span_id, sid_hex);
        ret = snprintf(out, remaining, "\",\"span_id\":\"%s", sid_hex);
        if (ret <= 0 || (size_t)ret >= remaining)
            return -1;
        out += ret;
        remaining -= (size_t)ret;
    }

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
/*  W3C TraceContext parser                                            */
/* ------------------------------------------------------------------ */

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *hex, uint8_t *out, int n) {
    for (int i = 0; i < n; i++) {
        int hi = hex_nibble(hex[(size_t)i * 2]);
        int lo = hex_nibble(hex[(size_t)i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return 0;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

/**
 * Extract trace context from the W3C Trace-Context env var.
 * Output arrays are zeroed on parse failure (safety: env input is untrusted).
 */
static void parse_traceparent(uint8_t trace_id[16], uint8_t span_id[8]) {
    const char *tp = getenv("TRACEPARENT");
    if (!tp || tp[0] == '\0')
        return;

    const char *p = tp;
    while (*p && *p != '-')
        p++;
    if (*p != '-')
        return;
    p++;

    size_t remaining = strlen(p);
    if (remaining < 32)
        return;
    if (!hex_decode(p, trace_id, 16)) {
        memset(trace_id, 0, 16);
        return;
    }
    p += 32;
    if (*p != '-') {
        memset(trace_id, 0, 16);
        return;
    }
    p++;

    remaining = strlen(p);
    if (remaining < 16) {
        memset(trace_id, 0, 16);
        return;
    }
    if (!hex_decode(p, span_id, 8)) {
        memset(trace_id, 0, 16);
        memset(span_id, 0, 8);
        return;
    }
}

/* ------------------------------------------------------------------ */
/*  OTel JSON renderer                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Format @p record as OpenTelemetry-compatible JSON.
 *
 * Output follows the OTel Log Data Model:
 * @code
 * {"timestamp":"...","severity":"INFO","severity_number":9,
 *  "trace_id":"...","span_id":"...",
 *  "body":"message text",
 *  "attributes":{"file":"app.c","line":42,"func":"main",
 *                "module":"...","tag":"...","thread":1234,"pid":5678}}
 * @endcode
 *
 * The `trace_id` and `span_id` fields are omitted when unset.
 *
 * @return Bytes written, or -1 on truncation.
 */
static int format_otel_json(log_record_t *restrict record, char *restrict buf, size_t buf_size) {
    struct tm tm_buf;
    time_t sec = (time_t)(record->timestamp / 1000000);
    uint32_t usec = (uint32_t)(record->timestamp % 1000000);
    clog_localtime_r(&sec, &tm_buf);

    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), g_time_format_buf, &tm_buf);

    char *out = buf;
    size_t remaining = buf_size;
    int ret;

    /* Open the top-level object */
    ret = snprintf(
        out, remaining, "{\"timestamp\":\"%s.%06u\",\"severity\":\"%s\",\"severity_number\":%d",
        time_buf, usec, level_to_string(record->level), otel_severity_number(record->level));
    if (ret <= 0 || (size_t)ret >= remaining)
        return -1;
    out += ret;
    remaining -= (size_t)ret;

    /* Trace context (only when present) */
    char tid_hex[33];
    char sid_hex[17];
    parse_traceparent(record->trace_id, record->span_id);
    if (!is_zero_id(record->trace_id, 16)) {
        trace_id_hex(record->trace_id, tid_hex);
        ret = snprintf(out, remaining, ",\"trace_id\":\"%s\"", tid_hex);
        if (ret <= 0 || (size_t)ret >= remaining)
            return -1;
        out += ret;
        remaining -= (size_t)ret;
    }
    if (!is_zero_id(record->span_id, 8)) {
        span_id_hex(record->span_id, sid_hex);
        ret = snprintf(out, remaining, ",\"span_id\":\"%s\"", sid_hex);
        if (ret <= 0 || (size_t)ret >= remaining)
            return -1;
        out += ret;
        remaining -= (size_t)ret;
    }

    /* Body = message */
    ret = snprintf(out, remaining, ",\"body\":\"");
    if (ret <= 0 || (size_t)ret >= remaining)
        return -1;
    out += ret;
    remaining -= (size_t)ret;
    append_json_escaped_string(&out, &remaining, record->message ? record->message : "");
    ret = snprintf(out, remaining, "\"");
    if (ret <= 0 || (size_t)ret >= remaining)
        return -1;
    out += ret;
    remaining -= (size_t)ret;

    /* Attributes object */
    ret = snprintf(out, remaining, ",\"attributes\":{");
    if (ret <= 0 || (size_t)ret >= remaining)
        return -1;
    out += ret;
    remaining -= (size_t)ret;

    ret = snprintf(out, remaining, "\"module\":\"");
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
    ret = snprintf(out, remaining, "\"}");
    if (ret <= 0 || (size_t)ret >= remaining)
        return -1;
    out += ret;
    remaining -= (size_t)ret;

    /* Close top-level object */
    ret = snprintf(out, remaining, "}");
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
        else if (token_match(&fmt_str, "trace_id"))
            ops[n++].op = FMT_OP_TRACE_ID;
        else if (token_match(&fmt_str, "span_id"))
            ops[n++].op = FMT_OP_SPAN_ID;
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

/** Internal: format a record with given format and time_format strings (no copy). */
static int format_impl(log_record_t *restrict record, char *restrict buf, size_t buf_size,
                       const char *fmt, const char *time_format) {
    if (strcmp(fmt, "json") == 0 || strcmp(fmt, "JSON") == 0) {
        return format_json_ex(record, buf, buf_size, time_format);
    }
    if (strcmp(fmt, "otlp") == 0 || strcmp(fmt, "OTLP") == 0 || strcmp(fmt, "otel") == 0 ||
        strcmp(fmt, "OTEL") == 0) {
        return format_otel_json(record, buf, buf_size);
    }

    fmt_op_t ops[FMT_MAX_OPS];
    int op_count = fmt_compile(fmt, ops, FMT_MAX_OPS);
    const char *tf = (time_format && time_format[0]) ? time_format : "%Y-%m-%d %H:%M:%S";

    char *out = buf;
    size_t remaining = buf_size;
    int total = 0;
    struct tm tm_buf;
    int tm_initialized = 0;

    for (int i = 0; i < op_count; i++) {
        switch (ops[i].op) {

        case FMT_OP_LITERAL:
            total += append_token(&out, &remaining, ops[i].literal, ops[i].literal_len);
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

        case FMT_OP_TRACE_ID: {
            char tid_hex[33] = "";
            if (!is_zero_id(record->trace_id, 16)) {
                trace_id_hex(record->trace_id, tid_hex);
            }
            total += append_token(&out, &remaining, tid_hex, strlen(tid_hex));
            break;
        }

        case FMT_OP_SPAN_ID: {
            char sid_hex[17] = "";
            if (!is_zero_id(record->span_id, 8)) {
                span_id_hex(record->span_id, sid_hex);
            }
            total += append_token(&out, &remaining, sid_hex, strlen(sid_hex));
            break;
        }
        }
    }

    *out = '\0';
    return total;
}

int log_formatter_format_otlp(log_record_t *restrict record, char *restrict buf, size_t buf_size) {
    if (!record || !buf || buf_size == 0)
        return -1;
    return format_otel_json(record, buf, buf_size);
}

/* ── Singleton wrappers ── */

int log_formatter_format(log_record_t *restrict record, char *restrict buf, size_t buf_size) {
    return log_formatter_format_for(&g_default_logger, record, buf, buf_size);
}

int log_formatter_init(const char *format, const char *time_format) {
    return log_formatter_init_for(&g_default_logger, format, time_format);
}

void log_formatter_reset(void) {
    log_formatter_init_for(&g_default_logger, NULL, NULL);
}

const char *log_formatter_get_format(void) {
    return g_default_logger.format_str[0] ? g_default_logger.format_str : g_default_format;
}

/* ── Instance variants ── */

int log_formatter_format_for(logger_t *logger, log_record_t *restrict record, char *restrict buf,
                             size_t buf_size) {
    if (!logger || !record || !buf || buf_size == 0)
        return -1;
    const char *fmt = logger->format_str[0] ? logger->format_str : g_default_format;
    const char *tf = logger->time_format_str[0] ? logger->time_format_str : "%Y-%m-%d %H:%M:%S";
    return format_impl(record, buf, buf_size, fmt, tf);
}

int log_formatter_init_for(logger_t *logger, const char *format, const char *time_format) {
    clog_mutex_lock(&logger->fmt_mutex);
    if (format && strlen(format) > 0) {
        snprintf(logger->format_str, sizeof(logger->format_str), "%s", format);
    } else {
        snprintf(logger->format_str, sizeof(logger->format_str), "%s", g_default_format);
    }
    if (time_format && strlen(time_format) > 0) {
        snprintf(logger->time_format_str, sizeof(logger->time_format_str), "%s", time_format);
    } else {
        snprintf(logger->time_format_str, sizeof(logger->time_format_str), "%s",
                 "%Y-%m-%d %H:%M:%S");
    }
    clog_mutex_unlock(&logger->fmt_mutex);
    return 0;
}
