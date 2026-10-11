/**
 * @file formatter_fmt.c
 * @brief Pattern compiler (fmt_compile/token_match) and opcode interpreter (format_impl).
 *
 * Split from formatter.c (2026-10-11, zero behavior change).
 * Shared helpers and cross-unit declarations live in
 * formatter_internal.h.
 */

#include "clog_port.h"
#include "fast_ascii.h"
#include "formatter_internal.h"
#include "log_formatter.h"
#include "log_internal.h"
#include "log_limits.h"
#include "log_record.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* Compiled opcode program (populated at init time). */

#define TIME_BUF_SIZE 64

/*
 * Per-thread compiled-format cache.
 *
 * fmt_compile() output depends only on the format string, so re-parsing it
 * on every log call is pure waste. The cache keeps a private copy of the
 * format string; literal opcodes reference that copy, making the cached
 * program self-contained regardless of the lifetime of the caller's format
 * buffer (e.g. `logger->format_str` across a reload). A string compare on
 * each call invalidates the cache whenever the format actually changes.
 * Thread-local: no locking, no cross-thread sharing.
 */
typedef struct {
    char     fmt[CLOG_MAX_FORMAT_SIZE];
    fmt_op_t ops[FMT_MAX_OPS];
    int      op_count;
} clog_fmt_cache_t;

static clog_thread_local clog_fmt_cache_t g_fmt_cache;

/**
 * @brief Append a token's literal text to the output buffer.
 *
 * Advances @p out and decrements @p remaining by the token length.
 * Returns -1 if the token doesn't fit; 0 on success.
 */
static int append_token(char **out, size_t *remaining, const char *token, size_t token_len)
{
    if (token_len >= *remaining) {
        token_len = *remaining - 1;
    }
    if (token_len > 0) {
        memcpy(*out, token, token_len);
        *out += token_len;
        *remaining -= token_len;
        **out = '\0';
    }
    return (int)token_len;
}

/**
 * @brief Match a token name at the current format-string position.
 *
 * Returns 1 and advances @p fmt past the token if the next characters
 * match @p token and are followed by a non-alpha boundary.
 */
static int token_match(const char **fmt, const char *token)
{
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
static int fmt_compile(const char *fmt_str, fmt_op_t *ops, int max_ops)
{
    int n = 0;
    while (*fmt_str && n < max_ops - 1) {
        if (*fmt_str != '%') {
            const char *start = fmt_str;
            while (*fmt_str && *fmt_str != '%') {
                fmt_str++;
            }
            size_t len = (size_t)(fmt_str - start);
            if (len > 0) {
                ops[n].op          = FMT_OP_LITERAL;
                ops[n].literal     = start;
                ops[n].literal_len = len;
                n++;
            }
            continue;
        }

        fmt_str++; /* skip '%' */

        if (token_match(&fmt_str, "time")) {
            ops[n++].op = FMT_OP_TIME;
        } else if (token_match(&fmt_str, "level")) {
            ops[n++].op = FMT_OP_LEVEL;
        } else if (token_match(&fmt_str, "msg")) {
            ops[n++].op = FMT_OP_MSG;
        } else if (token_match(&fmt_str, "thread")) {
            ops[n++].op = FMT_OP_THREAD;
        } else if (token_match(&fmt_str, "pid")) {
            ops[n++].op = FMT_OP_PID;
        } else if (token_match(&fmt_str, "file")) {
            ops[n++].op = FMT_OP_FILE;
        } else if (token_match(&fmt_str, "line")) {
            ops[n++].op = FMT_OP_LINE;
        } else if (token_match(&fmt_str, "func")) {
            ops[n++].op = FMT_OP_FUNC;
        } else if (token_match(&fmt_str, "module")) {
            ops[n++].op = FMT_OP_MODULE;
        } else if (token_match(&fmt_str, "tag")) {
            ops[n++].op = FMT_OP_TAG;
        } else if (token_match(&fmt_str, "newline")) {
            ops[n++].op = FMT_OP_NEWLINE;
        } else if (token_match(&fmt_str, "trace_id")) {
            ops[n++].op = FMT_OP_TRACE_ID;
        } else if (token_match(&fmt_str, "span_id")) {
            ops[n++].op = FMT_OP_SPAN_ID;
        } else {
            /* Unknown token — emit '%' as a literal. */
            ops[n].op          = FMT_OP_LITERAL;
            ops[n].literal     = fmt_str - 1;
            ops[n].literal_len = 1;
            n++;
        }
    }
    return n;
}

/**
 * @brief Internal format dispatch: routes to JSON, OTel, or token-based renderer.
 *
 * If @p fmt is "json"/"JSON", delegates to format_json_ex().
 * If @p fmt is "otlp"/"OTLP"/"otel"/"OTEL", delegates to format_otel_json().
 * Otherwise, resolves the per-thread compiled-opcode cache and renders by
 * walking the opcode array.
 *
 * @param[in]  record       Log record to format.
 * @param[out] buf          Output buffer.
 * @param[in]  buf_size     Capacity of @p buf.
 * @param[in]  fmt          Format string or mode keyword.
 * @param[in]  time_format  strftime template for %time token.
 * @return Bytes written, or -1 on error.
 */
int format_impl(log_record_t *restrict record,
                char *restrict buf,
                size_t      buf_size,
                const char *fmt,
                const char *time_format)
{
    if (strcmp(fmt, "json") == 0 || strcmp(fmt, "JSON") == 0) {
        return format_json_ex(record, buf, buf_size, time_format);
    }
    if (strcmp(fmt, "otlp") == 0 || strcmp(fmt, "OTLP") == 0 || strcmp(fmt, "otel") == 0 ||
        strcmp(fmt, "OTEL") == 0) {
        return format_otel_json(record, buf, buf_size);
    }

    if (strcmp(fmt, g_fmt_cache.fmt) != 0) {
        snprintf(g_fmt_cache.fmt, sizeof(g_fmt_cache.fmt), "%s", fmt);
        g_fmt_cache.op_count = fmt_compile(g_fmt_cache.fmt, g_fmt_cache.ops, FMT_MAX_OPS);
    }
    const fmt_op_t *ops      = g_fmt_cache.ops;
    int             op_count = g_fmt_cache.op_count;
    const char     *tf       = (time_format && time_format[0]) ? time_format : "%Y-%m-%d %H:%M:%S";

    char  *out       = buf;
    size_t remaining = buf_size;
    int    total     = 0;

    for (int i = 0; i < op_count; i++) {
        switch (ops[i].op) {

        case FMT_OP_LITERAL:
            total += append_token(&out, &remaining, ops[i].literal, ops[i].literal_len);
            break;

        case FMT_OP_TIME: {
            time_t sec = (time_t)(record->timestamp / 1000000);
            char   time_buf[TIME_BUF_SIZE];
            format_sec_cached(sec, tf, time_buf, sizeof(time_buf));
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
            char   tmp[16];
            size_t n = clog_u32toa(record->tid, tmp);
            total += append_token(&out, &remaining, tmp, n);
            break;
        }

        case FMT_OP_PID: {
            char   tmp[16];
            size_t n = clog_u32toa(record->pid, tmp);
            total += append_token(&out, &remaining, tmp, n);
            break;
        }

        case FMT_OP_FILE: {
            const char *s = record->file ? record->file : "(unknown)";
            total += append_token(&out, &remaining, s, strlen(s));
            break;
        }

        case FMT_OP_LINE: {
            char   tmp[16];
            size_t n = clog_i32toa(record->line, tmp);
            total += append_token(&out, &remaining, tmp, n);
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

    if (record->kv_count > 0) {
        for (size_t i = 0; i < record->kv_count; i++) {
            const clog_kv_t *kv = &record->kv[i];
            if (!kv->key) {
                continue;
            }
            total += append_token(&out, &remaining, " ", 1);
            total += append_token(&out, &remaining, kv->key, strlen(kv->key));
            total += append_token(&out, &remaining, "=", 1);
            switch (kv->type) {
            case CLOG_KV_TYPE_INT: {
                char   tmp[32];
                size_t n = clog_i64toa(kv->val.i64, tmp);
                total += append_token(&out, &remaining, tmp, n);
                break;
            }
            case CLOG_KV_TYPE_UINT: {
                char   tmp[32];
                size_t n = clog_u64toa(kv->val.u64, tmp);
                total += append_token(&out, &remaining, tmp, n);
                break;
            }
            case CLOG_KV_TYPE_FLOAT: {
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "%.6g", kv->val.f64);
                total += append_token(&out, &remaining, tmp, strlen(tmp));
                break;
            }
            case CLOG_KV_TYPE_STR: {
                const char *s = kv->val.str ? kv->val.str : "";
                total += append_token(&out, &remaining, s, strlen(s));
                break;
            }
            case CLOG_KV_TYPE_BOOL: {
                const char *b = kv->val.b ? "true" : "false";
                total += append_token(&out, &remaining, b, strlen(b));
                break;
            }
            }
        }
    }

    *out = '\0';
    return total;
}
