/**
 * @file log_formatter.h
 * @brief Token-based formatter for log lines, with compile-once pattern engine.
 *
 * ## Design
 *
 * Format strings such as `[%time] [%level] %msg` are compiled into an opcode
 * sequence (@ref fmt_op_t) once at initialisation time. The hot path
 * (@ref log_formatter_format) then executes a flat switch over opcodes —
 * no string parsing, no strcmp overhead per log line.
 *
 * ## Format Tokens
 *
 * | Token        | Content                              |
 * |--------------|--------------------------------------|
 * | `%time`      | Timestamp via strftime + microseconds |
 * | `%level`     | Severity name (TRACE … FATAL)        |
 * | `%msg`       | Log message body                     |
 * | `%thread`    | Thread ID (pthread_t hash)           |
 * | `%pid`       | Process ID                           |
 * | `%file`      | Source file name                     |
 * | `%line`      | Source line number                   |
 * | `%func`      | Function name                        |
 * | `%module`    | Module name                          |
 * | `%tag`       | Tag string                           |
 * | `%newline`   | Literal newline                      |
 * | `%trace_id`  | W3C TraceContext trace ID (hex)      |
 * | `%span_id`   | W3C TraceContext span ID (hex)       |
 *
 * ## Thread Safety
 *
 * - @ref log_formatter_init and @ref log_formatter_set must be called
 *   before any logging thread starts (or under an external lock).
 * - @ref log_formatter_format is thread-safe: it reads compiled opcodes
 *   and buffer-formats into a thread-local output buffer.
 * - @ref log_formatter_reset is safe to call during re-configuration
 *   (guarded by the caller's reload lock).
 */

#ifndef LOG_FORMATTER_H
#define LOG_FORMATTER_H

#include "log_record.h"
#include <stddef.h>

typedef struct logger_t logger_t;

/* ------------------------------------------------------------------ */
/*  Opcode compiler types                                             */
/* ------------------------------------------------------------------ */

/**
 * @def FMT_MAX_OPS
 * @brief Maximum number of compiled opcodes in the format program.
 *
 * Safety limit, not a tuning parameter. Ensures the opcode array is
 * stack-allocatable. If a format string expands to more tokens than
 * this, @ref log_formatter_init returns an error.
 */
#define FMT_MAX_OPS 64

/**
 * @enum fmt_opcode_t
 * @brief Opcode for one format token or literal segment.
 *
 * Each opcode corresponds to either a static literal string extracted
 * from the format pattern, or a dynamic field drawn from the
 * @ref log_record_t at format time.
 */
typedef enum {
    FMT_OP_LITERAL = 0, /**< Static text segment copied verbatim (literal / literal_len). */
    FMT_OP_TIME,        /**< %time — local timestamp via strftime + microsecond suffix.    */
    FMT_OP_LEVEL,       /**< %level — severity name string (TRACE … FATAL).                */
    FMT_OP_MSG,         /**< %msg — formatted message body.                                */
    FMT_OP_THREAD,      /**< %thread — thread ID (hash of pthread_t).                      */
    FMT_OP_PID,         /**< %pid — process ID (getpid()).                                 */
    FMT_OP_FILE,        /**< %file — source file base name (__FILE__).                     */
    FMT_OP_LINE,        /**< %line — source line number (__LINE__).                        */
    FMT_OP_FUNC,        /**< %func — function name (__func__ or __FUNCTION__).             */
    FMT_OP_MODULE,      /**< %module — module name (set via log_set_module / config).      */
    FMT_OP_TAG,         /**< %tag — tag string.                                            */
    FMT_OP_NEWLINE,     /**< %newline — literal newline character.                         */
    FMT_OP_TRACE_ID,    /**< %trace_id — W3C TraceContext trace ID as 32-char hex string. */
    FMT_OP_SPAN_ID,     /**< %span_id — W3C TraceContext span ID as 16-char hex string.   */
} fmt_opcode_t;

/**
 * @struct fmt_op_t
 * @brief One instruction in the compiled format program.
 *
 * The compiler in @ref log_formatter_init translates a format string
 * such as `"[%time] %msg"` into an array of these ops. The hot-path
 * formatter executes them linearly via a switch on @ref op.
 */
typedef struct {
    fmt_opcode_t op; /**< Opcode selecting the field or literal handler.               */
    const char
          *literal;     /**< Pointer to the literal text segment (valid only for FMT_OP_LITERAL). */
    size_t literal_len; /**< Byte length of the literal segment (excluding NUL).          */
} fmt_op_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Format a log record into a buffer using the active format program.
 *
 * Executes the compiled opcode array (set by @ref log_formatter_init)
 * against @p record, writing the result into @p buf. If the output
 * would exceed @p buf_size, the buffer is NUL-terminated at the limit
 * and the return value indicates truncation.
 *
 * @param[in]  record   Log event to format. Must not be NULL.
 * @param[out] buf      Output buffer (NUL-terminated on success).
 * @param[in]  buf_size Capacity of @p buf in bytes (including NUL terminator).
 * @retval >0  Number of bytes written excluding the terminating NUL.
 * @retval <=0 Failure or truncation.
 *
 * @note Thread-safe: reads compiled format ops and the record; no shared state.
 */
int log_formatter_format(log_record_t *restrict record, char *restrict buf, size_t buf_size);

/**
 * @brief Format @p record as an OpenTelemetry OTLP JSON Log Record.
 *
 * Produces a JSON object conforming to the OTLP LogRecord protobuf-to-JSON
 * mapping. This is separate from the `format: "json"` structured logging
 * path — it targets OTEL collectors rather than human-oriented output.
 *
 * Fields emitted: timestamp, severityNumber (OTEL numeric code), severityText,
 * body (stringValue), attributes (source location, thread, module).
 *
 * @param[in]  record   Log event to format.
 * @param[out] buf      Output buffer (NUL-terminated on success).
 * @param[in]  buf_size Capacity of @p buf in bytes.
 * @retval >0  Bytes written excluding the terminating NUL.
 * @retval <=0 Failure or truncation.
 */
int log_formatter_format_otlp(log_record_t *restrict record, char *restrict buf, size_t buf_size);

/**
 * @brief Compile and activate a format string and time template.
 *
 * Parses @p format into an array of @ref fmt_op_t opcodes. The compiled
 * program is stored in internal static storage and used by all subsequent
 * calls to @ref log_formatter_format.
 *
 * If @p format contains unknown tokens (not matching the supported list),
 * they are treated as literal text. The token list is fixed at compile time
 * and replicated in the file-level documentation.
 *
 * @param[in] format      Format pattern string; NULL or empty restores `"%msg"`.
 * @param[in] time_format strftime(3) template for the `%time` token; NULL
 *                        or empty restores `"%Y-%m-%d %H:%M:%S"`.
 * @retval 0  Success.
 * @retval -1 Compilation failure (too many tokens exceeding @ref FMT_MAX_OPS,
 *            or internal allocation error).
 *
 * @note This function is **not** thread-safe with respect to concurrent
 *       @ref log_formatter_format calls. Call it during initialisation
 *       or while the logger is quiesced.
 */
int log_formatter_init(const char *format, const char *time_format);

/**
 * @brief Reset the formatter to its default state (`"%msg"` format,
 *        `"%Y-%m-%d %H:%M:%S"` time format).
 *
 * Convenience wrapper equivalent to calling `log_formatter_init("%msg", NULL)`.
 */
void log_formatter_reset(void);

/**
 * @brief Get the currently active format string.
 *
 * @return Pointer to the internal buffer holding the format string.
 *         The caller must NOT free or modify this pointer.
 *         Returns `"%msg"` if the formatter has never been initialised.
 */
const char *log_formatter_get_format(void);

/* ── Instance variants (same contract, different logger instance) ── */

/**
 * @brief Instance variant of @ref log_formatter_format for a specific
 *        @ref logger_t.
 */
int log_formatter_format_for(logger_t *logger,
                             log_record_t *restrict record,
                             char *restrict buf,
                             size_t buf_size);

/**
 * @brief Instance variant of @ref log_formatter_init for a specific
 *        @ref logger_t.
 */
int log_formatter_init_for(logger_t *logger, const char *format, const char *time_format);

#endif /* LOG_FORMATTER_H */
