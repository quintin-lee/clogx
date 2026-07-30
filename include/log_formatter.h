/**
 * @file log_formatter.h
 * @brief Token-based formatter for log lines, with pattern compiler.
 *
 * Format strings use tokens such as `[%time] [%level] %msg`.
 * The format string is compiled into an opcode sequence once at init
 * time, so the hot path never needs to re-parse the format string.
 */

#ifndef LOG_FORMATTER_H
#define LOG_FORMATTER_H

#include <stddef.h>
#include "log_record.h"

typedef struct logger_t logger_t;

/* ------------------------------------------------------------------ */
/*  Opcode compiler types                                             */
/* ------------------------------------------------------------------ */

/** Maximum number of compiled ops (safety limit, not a tuning parameter). */
#define FMT_MAX_OPS 64

/** @brief Opcode for one format token or literal segment. */
typedef enum {
    FMT_OP_LITERAL = 0, /**< Static text segment (literal / literal_len). */
    FMT_OP_TIME,        /**< %time — timestamp with microsecond suffix.    */
    FMT_OP_LEVEL,       /**< %level — severity name.                      */
    FMT_OP_MSG,         /**< %msg — message body.                         */
    FMT_OP_THREAD,      /**< %thread — thread ID.                         */
    FMT_OP_PID,         /**< %pid — process ID.                           */
    FMT_OP_FILE,        /**< %file — source file name.                    */
    FMT_OP_LINE,        /**< %line — source line number.                  */
    FMT_OP_FUNC,        /**< %func — function name.                       */
    FMT_OP_MODULE,      /**< %module — module name.                       */
    FMT_OP_TAG,         /**< %tag — tag string.                           */
    FMT_OP_NEWLINE,     /**< %newline — literal newline.                  */
    FMT_OP_TRACE_ID,    /**< %trace_id — W3C TraceContext trace ID hex string. */
    FMT_OP_SPAN_ID,     /**< %span_id — W3C TraceContext span ID hex string.   */
} fmt_opcode_t;

/** @brief One instruction in the compiled format program. */
typedef struct {
    fmt_opcode_t op;     /**< Opcode.                         */
    const char *literal; /**< Text segment (FMT_OP_LITERAL).  */
    size_t literal_len;  /**< Byte length of text segment.    */
} fmt_op_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Format @p record into @p buf according to the active format string.
 *
 * @param[in]  record   Log event to format.
 * @param[out] buf      Output buffer (NUL-terminated on success).
 * @param[in]  buf_size Capacity of @p buf in bytes.
 * @return Bytes written excluding the terminating NUL, or <= 0 on failure.
 */
int log_formatter_format(log_record_t *restrict record, char *restrict buf, size_t buf_size);

/**
 * @brief Format @p record into @p buf as an OpenTelemetry OTLP JSON Log Record object.
 *
 * @param[in]  record   Log event to format.
 * @param[out] buf      Output buffer (NUL-terminated on success).
 * @param[in]  buf_size Capacity of @p buf in bytes.
 * @return Bytes written, or <= 0 on failure.
 */
int log_formatter_format_otlp(log_record_t *restrict record, char *restrict buf, size_t buf_size);

/**
 * @brief Set the active format string and time template (both copied into
 *        internal storage).
 *
 * The format string is compiled into @ref fmt_op_t opcodes so the
 * hot path only executes a flat switch — no strncmp parsing at runtime.
 *
 * Supported tokens:
 * - `%time` — local timestamp formatted with @p time_format + microseconds
 * - `%level` — severity name
 * - `%msg` — message body
 * - `%thread` / `%pid`
 * - `%file` / `%line` / `%func`
 * - `%module` / `%tag`
 * - `%newline`
 *
 * @param[in] format      Format string; NULL or empty restores `"%msg"`.
 * @param[in] time_format strftime(3) template for the `%time` token; NULL
 *                        or empty restores `"%Y-%m-%d %H:%M:%S"`.
 * @return 0 on success.
 */
int log_formatter_init(const char *format, const char *time_format);

/**
 * @brief Reset the formatter to the default format (`%msg`).
 */
void log_formatter_reset(void);

/**
 * @brief Get the currently active format string.
 * @return Pointer to internal storage (do not free).
 */
const char *log_formatter_get_format(void);

/* ── Instance variants ── */

int log_formatter_format_for(logger_t *logger, log_record_t *restrict record, char *restrict buf,
                             size_t buf_size);
int log_formatter_init_for(logger_t *logger, const char *format, const char *time_format);

#endif /* LOG_FORMATTER_H */
