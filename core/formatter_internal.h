/**
 * @file formatter_internal.h
 * @brief Internal shared declarations for the split formatter modules.
 *
 * formatter.c keeps the shared append/format helpers and the public API;
 * formatter_fmt.c / formatter_json.c / formatter_otel.c implement the
 * pattern compiler, JSON renderer, and OTel renderer respectively.
 * All symbols here have hidden visibility (project builds with
 * -fvisibility=hidden); public API stays in include/log_formatter.h.
 */
#ifndef FORMATTER_INTERNAL_H
#define FORMATTER_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "log_limits.h"
#include "log_record.h"

/* ── Shared append helpers (defined in formatter.c) ── */
int append_buf_str(char **out, size_t *remaining, const char *str, size_t len);
int append_buf_u32(char **out, size_t *remaining, uint32_t val);
int append_buf_i32(char **out, size_t *remaining, int32_t val);
int append_buf_i64(char **out, size_t *remaining, int64_t val);
int append_buf_u64(char **out, size_t *remaining, uint64_t val);
int append_buf_u32_pad6(char **out, size_t *remaining, uint32_t val);
int append_json_escaped_string(char **out, size_t *remaining, const char *str);
int json_close_object(char *buf, char **out, size_t *remaining, const char *suffix);
int append_json_kv(char **out, size_t *remaining, const clog_kv_t *kv);

/* ── Shared scalar helpers (defined in formatter.c) ── */
void        format_sec_cached(time_t sec, const char *fmt, char *out_str, size_t out_str_size);
const char *level_to_string(log_level_t level);
int         is_zero_id(const uint8_t *id, int len);
void        trace_id_hex(const uint8_t trace_id[16], char *out);
void        span_id_hex(const uint8_t span_id[8], char *out);

/* Shared default time format (defined in formatter.c; OTel renderer uses it). */
extern char g_time_format_buf[64];

/* ── Renderer entry points (defined in their own TUs) ── */
int format_json_ex(log_record_t *restrict record,
                   char *restrict buf,
                   size_t      buf_size,
                   const char *time_format);
int format_otel_json(log_record_t *restrict record, char *restrict buf, size_t buf_size);
int format_impl(log_record_t *restrict record,
                char *restrict buf,
                size_t      buf_size,
                const char *fmt,
                const char *time_format);

#endif /* FORMATTER_INTERNAL_H */
