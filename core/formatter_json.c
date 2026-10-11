/**
 * @file formatter_json.c
 * @brief Single-line JSON renderer (format_json_ex) and KV appender.
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
/**
 * @brief Append one typed KV attribute: `,"key":value`.
 *
 * @return 0 on success; 1 when a `"`-delimited string was truncated
 *         (the caller must close the object via json_close_object);
 *         -1 on failure (the caller must drop the line).
 */
int append_json_kv(char **out, size_t *remaining, const clog_kv_t *kv)
{
    if (!kv || !kv->key) {
        return 0;
    }
    if (append_buf_str(out, remaining, ",\"", 2) != 0) {
        return -1;
    }
    if (append_json_escaped_string(out, remaining, kv->key) != 0) {
        return 1; /* key string truncated — caller closes with '"' + closer */
    }
    if (append_buf_str(out, remaining, "\":", 2) != 0) {
        return -1;
    }
    switch (kv->type) {
    case CLOG_KV_TYPE_INT:
        return append_buf_i64(out, remaining, kv->val.i64);
    case CLOG_KV_TYPE_UINT:
        return append_buf_u64(out, remaining, kv->val.u64);
    case CLOG_KV_TYPE_FLOAT: {
        char fbuf[32];
        snprintf(fbuf, sizeof(fbuf), "%.6g", kv->val.f64);
        return append_buf_str(out, remaining, fbuf, strlen(fbuf));
    }
    case CLOG_KV_TYPE_STR:
        if (append_buf_str(out, remaining, "\"", 1) != 0) {
            return -1;
        }
        if (append_json_escaped_string(out, remaining, kv->val.str ? kv->val.str : "") != 0) {
            return 1; /* value string truncated — caller closes with '"' + closer */
        }
        return append_buf_str(out, remaining, "\"", 1);
    case CLOG_KV_TYPE_BOOL:
        return append_buf_str(out, remaining, kv->val.b ? "true" : "false", kv->val.b ? 4 : 5);
    default:
        return 0;
    }
}

/**
 * @brief Format a log record as a single-line RFC 8259 JSON object.
 *
 * Renders fields: timestamp (ISO 8601 + µs), level, module, file, line,
 * func, thread, pid, tag, message. All string values are JSON-escaped.
 * trace_id and span_id are included only when non-zero.
 *
 * @param[in]  record    Log record to format.
 * @param[out] buf       Output buffer.
 * @param[in]  buf_size  Capacity of @p buf.
 * @param[in]  time_format  strftime template (NULL → default).
 * @return Bytes written, or -1 on truncation.
 */
int format_json_ex(log_record_t *restrict record,
                   char *restrict buf,
                   size_t      buf_size,
                   const char *time_format)
{
    time_t   sec  = (time_t)(record->timestamp / 1000000);
    uint32_t usec = (uint32_t)(record->timestamp % 1000000);

    char time_buf[64];
    format_sec_cached(sec, time_format, time_buf, sizeof(time_buf));

    char       *out       = buf;
    size_t      remaining = buf_size;
    const char *lvl_str   = level_to_string(record->level);

    if (append_buf_str(&out, &remaining, "{\"timestamp\":\"", 14) != 0 ||
        append_buf_str(&out, &remaining, time_buf, strlen(time_buf)) != 0 ||
        append_buf_str(&out, &remaining, ".", 1) != 0 ||
        append_buf_u32_pad6(&out, &remaining, usec) != 0 ||
        append_buf_str(&out, &remaining, "\",\"level\":\"", 11) != 0 ||
        append_buf_str(&out, &remaining, lvl_str, strlen(lvl_str)) != 0 ||
        append_buf_str(&out, &remaining, "\",\"module\":\"", 12) != 0) {
        return -1;
    }

    if (append_json_escaped_string(&out, &remaining, record->module ? record->module : "") != 0) {
        return json_close_object(buf, &out, &remaining, "\"}");
    }

    if (append_buf_str(&out, &remaining, "\",\"file\":\"", 10) != 0) {
        return -1;
    }
    if (append_json_escaped_string(&out, &remaining, record->file ? record->file : "") != 0) {
        return json_close_object(buf, &out, &remaining, "\"}");
    }

    if (append_buf_str(&out, &remaining, "\",\"line\":", 9) != 0 ||
        append_buf_i32(&out, &remaining, record->line) != 0 ||
        append_buf_str(&out, &remaining, ",\"func\":\"", 9) != 0) {
        return -1;
    }
    if (append_json_escaped_string(&out, &remaining, record->func ? record->func : "") != 0) {
        return json_close_object(buf, &out, &remaining, "\"}");
    }

    if (append_buf_str(&out, &remaining, "\",\"thread\":", 11) != 0 ||
        append_buf_u32(&out, &remaining, record->tid) != 0 ||
        append_buf_str(&out, &remaining, ",\"pid\":", 7) != 0 ||
        append_buf_u32(&out, &remaining, record->pid) != 0 ||
        append_buf_str(&out, &remaining, ",\"tag\":\"", 8) != 0) {
        return -1;
    }
    if (append_json_escaped_string(&out, &remaining, record->tag ? record->tag : "") != 0) {
        return json_close_object(buf, &out, &remaining, "\"}");
    }

    char tid_hex[33];
    char sid_hex[17];
    if (!is_zero_id(record->trace_id, 16)) {
        trace_id_hex(record->trace_id, tid_hex);
        if (append_buf_str(&out, &remaining, "\",\"trace_id\":\"", 14) != 0 ||
            append_buf_str(&out, &remaining, tid_hex, 32) != 0) {
            return -1;
        }
    }
    if (!is_zero_id(record->span_id, 8)) {
        span_id_hex(record->span_id, sid_hex);
        if (append_buf_str(&out, &remaining, "\",\"span_id\":\"", 13) != 0 ||
            append_buf_str(&out, &remaining, sid_hex, 16) != 0) {
            return -1;
        }
    }

    if (append_buf_str(&out, &remaining, "\",\"message\":\"", 13) != 0) {
        return -1;
    }
    if (append_json_escaped_string(&out, &remaining, record->message ? record->message : "") != 0) {
        return json_close_object(buf, &out, &remaining, "\"}");
    }
    if (append_buf_str(&out, &remaining, "\"", 1) != 0) {
        return -1;
    }

    for (size_t i = 0; i < record->kv_count; i++) {
        int r = append_json_kv(&out, &remaining, &record->kv[i]);
        if (r == 1) {
            return json_close_object(buf, &out, &remaining, "\"}");
        }
        if (r != 0) {
            return -1;
        }
    }

    if (append_buf_str(&out, &remaining, "}", 1) != 0) {
        return -1;
    }

    return (int)(out - buf);
}
