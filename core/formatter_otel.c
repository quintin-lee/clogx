/**
 * @file formatter_otel.c
 * @brief W3C TraceContext parser and OTel JSON renderer.
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
/** @brief Convert a single hex character to its 4-bit integer value (0–15), or -1 on invalid input.
 */
static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/**
 * @brief Decode a hex string into a byte array.
 *
 * @param hex   Input hex string (2×@p n characters).
 * @param out   Output byte array (must have at least @p n bytes).
 * @param n     Number of bytes to decode.
 * @return 0 on success, -1 on invalid hex input.
 */
static int hex_decode(const char *hex, uint8_t *out, int n)
{
    for (int i = 0; i < n; i++) {
        int hi = hex_nibble(hex[(size_t)i * 2]);
        int lo = hex_nibble(hex[(size_t)i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return 0;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

/**
 * Extract trace context from the W3C Trace-Context env var.
 * Output arrays are zeroed on parse failure (safety: env input is untrusted).
 */
static void parse_traceparent(uint8_t trace_id[16], uint8_t span_id[8])
{
    const char *tp = getenv("TRACEPARENT");
    if (!tp || tp[0] == '\0') {
        return;
    }

    const char *p = tp;
    while (*p && *p != '-') {
        p++;
    }
    if (*p != '-') {
        return;
    }
    p++;

    size_t remaining = strlen(p);
    if (remaining < 32) {
        return;
    }
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
/**
 * @brief Render a log record as OpenTelemetry-compatible JSON.
 *
 * Produces a single JSON object with OTLP resource attributes,
 * severity number/text, optional W3C TraceContext fields, and
 * thread-local MDC context.
 *
 * @return Bytes written, or -1 on buffer overflow.
 */
int format_otel_json(log_record_t *restrict record, char *restrict buf, size_t buf_size)
{
    time_t   sec  = (time_t)(record->timestamp / 1000000);
    uint32_t usec = (uint32_t)(record->timestamp % 1000000);

    char time_buf[64];
    format_sec_cached(sec, g_time_format_buf, time_buf, sizeof(time_buf));

    char       *out       = buf;
    size_t      remaining = buf_size;
    const char *lvl_str   = level_to_string(record->level);

    if (append_buf_str(&out, &remaining, "{\"timestamp\":\"", 14) != 0 ||
        append_buf_str(&out, &remaining, time_buf, strlen(time_buf)) != 0 ||
        append_buf_str(&out, &remaining, ".", 1) != 0 ||
        append_buf_u32_pad6(&out, &remaining, usec) != 0 ||
        append_buf_str(&out, &remaining, "\",\"severity\":\"", 14) != 0 ||
        append_buf_str(&out, &remaining, lvl_str, strlen(lvl_str)) != 0 ||
        append_buf_str(&out, &remaining, "\",\"severity_number\":", 20) != 0 ||
        append_buf_i32(&out, &remaining, otel_severity_number(record->level)) != 0) {
        return -1;
    }

    /* Trace context (only when present) */
    char tid_hex[33];
    char sid_hex[17];
    parse_traceparent(record->trace_id, record->span_id);
    if (!is_zero_id(record->trace_id, 16)) {
        trace_id_hex(record->trace_id, tid_hex);
        if (append_buf_str(&out, &remaining, ",\"trace_id\":\"", 13) != 0 ||
            append_buf_str(&out, &remaining, tid_hex, 32) != 0 ||
            append_buf_str(&out, &remaining, "\"", 1) != 0) {
            return -1;
        }
    }
    if (!is_zero_id(record->span_id, 8)) {
        span_id_hex(record->span_id, sid_hex);
        if (append_buf_str(&out, &remaining, ",\"span_id\":\"", 12) != 0 ||
            append_buf_str(&out, &remaining, sid_hex, 16) != 0 ||
            append_buf_str(&out, &remaining, "\"", 1) != 0) {
            return -1;
        }
    }

    if (append_buf_str(&out, &remaining, ",\"body\":\"", 9) != 0) {
        return -1;
    }
    if (append_json_escaped_string(&out, &remaining, record->message ? record->message : "") != 0) {
        return json_close_object(buf, &out, &remaining, "\"}");
    }
    if (append_buf_str(&out, &remaining, "\"", 1) != 0) {
        return -1;
    }

    if (append_buf_str(&out, &remaining, ",\"attributes\":{", 15) != 0 ||
        append_buf_str(&out, &remaining, "\"module\":\"", 10) != 0) {
        return -1;
    }
    if (append_json_escaped_string(&out, &remaining, record->module ? record->module : "") != 0) {
        return json_close_object(buf, &out, &remaining, "\"}}");
    }
    if (append_buf_str(&out, &remaining, "\",\"file\":\"", 10) != 0) {
        return -1;
    }
    if (append_json_escaped_string(&out, &remaining, record->file ? record->file : "") != 0) {
        return json_close_object(buf, &out, &remaining, "\"}}");
    }
    if (append_buf_str(&out, &remaining, "\",\"line\":", 9) != 0 ||
        append_buf_i32(&out, &remaining, record->line) != 0 ||
        append_buf_str(&out, &remaining, ",\"func\":\"", 9) != 0) {
        return -1;
    }
    if (append_json_escaped_string(&out, &remaining, record->func ? record->func : "") != 0) {
        return json_close_object(buf, &out, &remaining, "\"}}");
    }
    if (append_buf_str(&out, &remaining, "\",\"thread\":", 11) != 0 ||
        append_buf_u32(&out, &remaining, record->tid) != 0 ||
        append_buf_str(&out, &remaining, ",\"pid\":", 7) != 0 ||
        append_buf_u32(&out, &remaining, record->pid) != 0 ||
        append_buf_str(&out, &remaining, ",\"tag\":\"", 8) != 0) {
        return -1;
    }
    if (append_json_escaped_string(&out, &remaining, record->tag ? record->tag : "") != 0) {
        return json_close_object(buf, &out, &remaining, "\"}}");
    }
    if (append_buf_str(&out, &remaining, "\"", 1) != 0) {
        return -1;
    }

    for (size_t i = 0; i < record->kv_count; i++) {
        int r = append_json_kv(&out, &remaining, &record->kv[i]);
        if (r == 1) {
            return json_close_object(buf, &out, &remaining, "\"}}");
        }
        if (r != 0) {
            return -1;
        }
    }

    if (append_buf_str(&out, &remaining, "}}", 2) != 0) {
        return -1;
    }

    return (int)(out - buf);
}
