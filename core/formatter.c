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

/* ------------------------------------------------------------------ */
/*  Global state                                                      */
/* ------------------------------------------------------------------ */

static char g_default_format[CLOG_MAX_FORMAT_SIZE] = "[%time] [%level] %msg";
char        g_time_format_buf[64]                  = "%Y-%m-%d %H:%M:%S";

typedef struct {
    time_t sec;
    char   time_format[64];
    char   formatted[64];
} clog_time_cache_t;

static clog_thread_local clog_time_cache_t g_time_cache = {0, "", ""};

void format_sec_cached(time_t sec, const char *fmt, char *out_str, size_t out_str_size)
{
    if (!fmt) {
        fmt = "%Y-%m-%d %H:%M:%S";
    }
    if (sec != g_time_cache.sec || strcmp(fmt, g_time_cache.time_format) != 0 ||
        g_time_cache.formatted[0] == '\0') {
        struct tm tm_buf;
        clog_localtime_r(&sec, &tm_buf);
        if (strcmp(fmt, "%Y-%m-%d %H:%M:%S") == 0) {
            clog_format_iso_datetime(&tm_buf, g_time_cache.formatted);
        } else {
            strftime(g_time_cache.formatted, sizeof(g_time_cache.formatted), fmt, &tm_buf);
        }
        size_t fmt_len = strlen(fmt);
        if (fmt_len >= sizeof(g_time_cache.time_format)) {
            fmt_len = sizeof(g_time_cache.time_format) - 1;
        }
        memcpy(g_time_cache.time_format, fmt, fmt_len);
        g_time_cache.time_format[fmt_len] = '\0';
        g_time_cache.sec                  = sec;
    }
    size_t len = strlen(g_time_cache.formatted);
    if (len >= out_str_size) {
        len = out_str_size - 1;
    }
    memcpy(out_str, g_time_cache.formatted, len);
    out_str[len] = '\0';
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

/** @brief Convert a log level enum to its string name. */
const char *level_to_string(log_level_t level)
{
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

/* ------------------------------------------------------------------ */
/*  JSON escaping & rendering                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief RFC 8259 JSON string escaping for log field values.
 *
 * Writes @p str to @p out with proper JSON escapes: `\"`, `\\`,
 * `\b`, `\f`, `\n`, `\r`, `\t`, and `\u00XX` for control bytes
 * below 0x20. Output is always null-terminated within @p remaining.
 *
 * @param[in,out] out        Output pointer (advanced past written bytes).
 * @param[in,out] remaining  Bytes remaining in buffer (decremented).
 * @param[in]     str        Input string (NULL treated as empty).
 * @return 0 when @p str was written completely, -1 when the output
 *         buffer ran out before @p str was exhausted (truncated).
 */
int append_json_escaped_string(char **out, size_t *remaining, const char *str)
{
    if (!str) {
        str = "";
    }
    while (*str && *remaining > 1) {
        unsigned char c = (unsigned char)*str++;
        if (c == '"') {
            if (*remaining <= 2) {
                break;
            }
            *(*out)++ = '\\';
            *(*out)++ = '"';
            *remaining -= 2;
        } else if (c == '\\') {
            if (*remaining <= 2) {
                break;
            }
            *(*out)++ = '\\';
            *(*out)++ = '\\';
            *remaining -= 2;
        } else if (c == '\b') {
            if (*remaining <= 2) {
                break;
            }
            *(*out)++ = '\\';
            *(*out)++ = 'b';
            *remaining -= 2;
        } else if (c == '\f') {
            if (*remaining <= 2) {
                break;
            }
            *(*out)++ = '\\';
            *(*out)++ = 'f';
            *remaining -= 2;
        } else if (c == '\n') {
            if (*remaining <= 2) {
                break;
            }
            *(*out)++ = '\\';
            *(*out)++ = 'n';
            *remaining -= 2;
        } else if (c == '\r') {
            if (*remaining <= 2) {
                break;
            }
            *(*out)++ = '\\';
            *(*out)++ = 'r';
            *remaining -= 2;
        } else if (c == '\t') {
            if (*remaining <= 2) {
                break;
            }
            *(*out)++ = '\\';
            *(*out)++ = 't';
            *remaining -= 2;
        } else if (c < 0x20) {
            if (*remaining <= 6) {
                break;
            }
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
    return (*str == '\0') ? 0 : -1;
}

/**
 * @brief Close a JSON object line after a field string was truncated.
 *
 * Appends @p suffix — e.g. `"}` to close a root object, `"}}` to close
 * the OTel attributes object — and returns the total line length.
 * Returns -1 when the suffix does not fit; the caller must drop the line.
 */
int json_close_object(char *buf, char **out, size_t *remaining, const char *suffix)
{
    size_t n = strlen(suffix);
    if (*remaining < n + 1) {
        return -1;
    }
    memcpy(*out, suffix, n);
    *out += n;
    *remaining -= n;
    **out = '\0';
    return (int)(*out - buf);
}

static const char g_hex_lut[] = "0123456789abcdef";

/** @brief Encode a 16-byte trace ID as a 32-char hex string. */
void trace_id_hex(const uint8_t trace_id[16], char *out)
{
    for (int i = 0; i < 16; i++) {
        uint8_t byte           = trace_id[i];
        out[(size_t)i * 2]     = g_hex_lut[byte >> 4];
        out[(size_t)i * 2 + 1] = g_hex_lut[byte & 0x0F];
    }
    out[32] = '\0';
}

/** @brief Encode an 8-byte span ID as a 16-char hex string. */
void span_id_hex(const uint8_t span_id[8], char *out)
{
    for (int i = 0; i < 8; i++) {
        uint8_t byte           = span_id[i];
        out[(size_t)i * 2]     = g_hex_lut[byte >> 4];
        out[(size_t)i * 2 + 1] = g_hex_lut[byte & 0x0F];
    }
    out[16] = '\0';
}

int append_buf_str(char **out, size_t *remaining, const char *str, size_t len)
{
    if (len >= *remaining) {
        return -1;
    }
    memcpy(*out, str, len);
    *out += len;
    *remaining -= len;
    **out = '\0';
    return 0;
}

int append_buf_u32(char **out, size_t *remaining, uint32_t val)
{
    char   tmp[16];
    size_t len = clog_u32toa(val, tmp);
    return append_buf_str(out, remaining, tmp, len);
}

int append_buf_i32(char **out, size_t *remaining, int32_t val)
{
    char   tmp[16];
    size_t len = clog_i32toa(val, tmp);
    return append_buf_str(out, remaining, tmp, len);
}

int append_buf_i64(char **out, size_t *remaining, int64_t val)
{
    char   tmp[32];
    size_t len = clog_i64toa(val, tmp);
    return append_buf_str(out, remaining, tmp, len);
}

int append_buf_u64(char **out, size_t *remaining, uint64_t val)
{
    char   tmp[32];
    size_t len = clog_u64toa(val, tmp);
    return append_buf_str(out, remaining, tmp, len);
}

int append_buf_u32_pad6(char **out, size_t *remaining, uint32_t val)
{
    char tmp[8];
    clog_u32toa_pad6(val, tmp);
    return append_buf_str(out, remaining, tmp, 6);
}

/** @brief Check if an ID byte array is all zeros (no active trace/span). */
int is_zero_id(const uint8_t *id, int len)
{
    for (int i = 0; i < len; i++) {
        if (id[i]) {
            return 0;
        }
    }
    return 1;
}

/**
 * @brief Format a record as OTLP-compatible JSON (public API for otlp_sink).
 *
 * @param[in]  record    Log record to format.
 * @param[out] buf       Output buffer.
 * @param[in]  buf_size  Capacity of @p buf.
 * @return Bytes written, or -1 on truncation or invalid input.
 */
int log_formatter_format_otlp(log_record_t *restrict record, char *restrict buf, size_t buf_size)
{
    if (!record || !buf || buf_size == 0) {
        return -1;
    }
    return format_otel_json(record, buf, buf_size);
}

/* ── Singleton wrappers ── */

/**
 * @brief Format a log record using the global logger's format settings.
 *
 * Delegates to log_formatter_format_for() with the singleton logger.
 * The format and time_format strings are read from the logger's config
 * (protected by fmt_mutex for concurrent safety).
 *
 * @param[in]  record    Log record to format.
 * @param[out] buf       Output buffer.
 * @param[in]  buf_size  Capacity of @p buf.
 * @return Bytes written, or -1 on truncation or NULL inputs.
 */
int log_formatter_format(log_record_t *restrict record, char *restrict buf, size_t buf_size)
{
    return log_formatter_format_for(&g_default_logger, record, buf, buf_size);
}

int log_formatter_init(const char *format, const char *time_format)
{
    return log_formatter_init_for(&g_default_logger, format, time_format);
}

void log_formatter_reset(void)
{
    log_formatter_init_for(&g_default_logger, NULL, NULL);
}

const char *log_formatter_get_format(void)
{
    return g_default_logger.format_str[0] ? g_default_logger.format_str : g_default_format;
}

/* ── Instance variants ── */

/**
 * @brief Format a log record using a specific logger instance's format settings.
 *
 * @param[in]  logger    Logger instance whose format_str/time_format_str to use.
 * @param[in]  record    Log record to format.
 * @param[out] buf       Output buffer.
 * @param[in]  buf_size  Capacity of @p buf.
 * @return Bytes written, or -1 on truncation or NULL inputs.
 */
int log_formatter_format_for(logger_t *logger,
                             log_record_t *restrict record,
                             char *restrict buf,
                             size_t buf_size)
{
    if (!logger || !record || !buf || buf_size == 0) {
        return -1;
    }
    const char *fmt = logger->format_str[0] ? logger->format_str : g_default_format;
    const char *tf  = logger->time_format_str[0] ? logger->time_format_str : "%Y-%m-%d %H:%M:%S";
    return format_impl(record, buf, buf_size, fmt, tf);
}

/**
 * @brief Initialise the format strings for a logger instance.
 *
 * Copies @p format and @p time_format into the logger's internal buffers.
 * Pass NULL or empty strings to reset to defaults ("%msg" and
 * "%Y-%m-%d %H:%M:%S").
 *
 * @param logger      Logger instance.
 * @param format      Format string (e.g. "[%time] [%level] %msg") or "json".
 * @param time_format strftime template for %time, or NULL for default.
 * @return Always 0 (success).
 */
int log_formatter_init_for(logger_t *logger, const char *format, const char *time_format)
{
    clog_mutex_lock(&logger->fmt_mutex);
    if (format && strlen(format) > 0) {
        snprintf(logger->format_str, sizeof(logger->format_str), "%s", format);
    } else {
        snprintf(logger->format_str, sizeof(logger->format_str), "%s", g_default_format);
    }
    if (time_format && strlen(time_format) > 0) {
        snprintf(logger->time_format_str, sizeof(logger->time_format_str), "%s", time_format);
    } else {
        snprintf(
            logger->time_format_str, sizeof(logger->time_format_str), "%s", "%Y-%m-%d %H:%M:%S");
    }
    clog_mutex_unlock(&logger->fmt_mutex);
    return 0;
}
