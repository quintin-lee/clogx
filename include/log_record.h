/**
 * @file log_record.h
 * @brief Core data structures for log events, shared across write path, queue, and dispatcher.
 *
 * This header defines the fundamental types used throughout clogx:
 *   - @ref log_level_t: Severity levels (TRACE .. FATAL)
 *   - @ref log_record_t: In-memory representation of a single log event
 *   - @ref log_color_t: ANSI color mappings for console sinks
 *   - Utility functions for OTel severity mapping and color selection
 *
 * The @ref log_record_t is the central payload passed from the writer (caller thread)
 * through the async queue to the worker thread, then to the dispatcher which writes
 * to configured sinks. On the synchronous (non-async) path, string fields in the record
 * are borrowed pointers pointing to stack memory. In the async path, the writer deep-copies
 * these strings before enqueueing to ensure the record remains valid after the caller returns.
 *
 * Size Considerations:
 *   - log_record_t is designed to fit on the stack with minimal overhead (64 bytes on 64-bit).
 *     String pointers (8 bytes each) avoid copying the actual message until formatting.
 *   - The fixed-size trace_id/span_id arrays support W3C TraceContext without heap allocation.
 *
 * @dot "Log Record Lifecycle"
 * digraph record_lifecycle {
 *     rankdir=LR;
 *     node [shape=box, style=filled, fontname="Helvetica", fontsize=10];
 *     edge [color="#666666", fontname="Helvetica", fontsize=9];
 *
 *     caller [label="Caller Thread\n(LOG_INFO)" fillcolor="#E3F2FD"];
 *     record [label="log_record_t\n(stack or heap)" fillcolor="#FFF9C4"];
 *     sync_path [label="Sync Path\n(direct dispatch)" fillcolor="#E8F5E9"];
 *     async_queue [label="MPSC Queue\n(deep-copy)" fillcolor="#FCE4EC"];
 *     worker [label="Async Worker\n(batch dequeue)" fillcolor="#F3E5F5"];
 *     dispatcher [label="Dispatcher\n(route to sinks)" fillcolor="#FFF3E0"];
 *     sinks [label="Sinks\n(console/file/socket/etc)" fillcolor="#E0F2F1"];
 *
 *     caller -> record [label="populate"];
 *     record -> sync_path [label="sync\n(borrowed ptrs)"];
 *     record -> async_queue [label="async\n(deep-copy)"];
 *     sync_path -> dispatcher;
 *     async_queue -> worker -> dispatcher;
 *     dispatcher -> sinks;
 * }
 * @enddot
 */

#ifndef LOG_RECORD_H
#define LOG_RECORD_H

#include "log_limits.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @enum clog_kv_type_t
 * @brief Data types supported in structured key-value log attributes.
 */
typedef enum {
    CLOG_KV_TYPE_INT,   /**< 64-bit signed integer value (i64). */
    CLOG_KV_TYPE_UINT,  /**< 64-bit unsigned integer value (u64). */
    CLOG_KV_TYPE_FLOAT, /**< Double-precision floating point value (f64). */
    CLOG_KV_TYPE_STR,   /**< Null-terminated C string value (str). */
    CLOG_KV_TYPE_BOOL   /**< Boolean value (b). */
} clog_kv_type_t;

/**
 * @struct clog_kv_t
 * @brief A single typed key-value pair for structured logging.
 */
typedef struct {
    const char    *key;  /**< Key identifier string (non-NULL). */
    clog_kv_type_t type; /**< Value payload type selector. */
    union {
        int64_t     i64; /**< Signed integer value payload. */
        uint64_t    u64; /**< Unsigned integer value payload. */
        double      f64; /**< Floating point value payload. */
        const char *str; /**< String value payload pointer. */
        bool        b;   /**< Boolean value payload. */
    } val;
} clog_kv_t;

/**
 * @enum log_level_t
 * @brief Log severity levels ordered from most verbose to most severe.
 *
 * Used by the logger to filter messages based on configuration. Messages below the
 * minimum configured level are dropped before any formatting or dispatch occurs.
 *
 * Ordering matters for comparisons: lower enum values are more verbose. A log level
 * check like `level >= min_level` determines whether a message should be emitted.
 */
typedef enum {
    LOG_LEVEL_TRACE, /**< Finest-grained diagnostic output; typically disabled in production. */
    LOG_LEVEL_DEBUG, /**< Detailed debugging information useful during development.      */
    LOG_LEVEL_INFO,  /**< Normal operational messages; entry/exit points, state changes.   */
    LOG_LEVEL_WARN,  /**< Unexpected but recoverable conditions; no immediate action required. */
    LOG_LEVEL_ERROR, /**< Failures that affect a request/operation; should be investigated.   */
    LOG_LEVEL_FATAL  /**< Severe failures that may render the process unusable; requires attention.
                      */
} log_level_t;

/**
 * @struct log_record_t
 * @brief In-memory representation of a single log event.
 *
 * This structure captures all metadata and content needed to emit one log record.
 * It is allocated on the stack in the fast-path macro expansions (LOG_* macros),
 * formatted, then either enqueued to the async MPSC queue or dispatched directly
 * to sinks depending on the configuration.
 *
 * Memory Layout & Borrowing Semantics:
 *   - On the synchronous path (async=false): string fields (`file`, `func`,
 *     `module`, `tag`, `message`) borrow pointers from the caller's stack frame.
 *     The record must be processed immediately before the caller's stack frame
 *     is destroyed.
 *   - On the async path (async=true): before enqueueing, the async layer makes
 *     heap copies of all string fields so the record outlives the caller thread.
 *     This allows the caller to return immediately while the record is safely
 *     processed later by the background worker thread.
 *
 * Thread-Safety:
 *   - The struct itself is NOT thread-safe; concurrent access must be serialized
 *     by the caller (e.g., via the queue or direct sink dispatch).
 *   - Fields like `timestamp`, `tid`, and `pid` are captured at logging time
 *     and reflect the context at the moment the log was issued.
 *
 * Size: 64 bytes on typical 64-bit platforms (12 pointers/arrays + 6 scalars).
 *       Fits comfortably on stack without overflow concerns even in tight loops.
 *
 * @par W3C TraceContext Integration
 * The `trace_id` (16 bytes) and `span_id` (8 bytes) fields carry distributed
 * tracing identifiers per the W3C Trace Context specification. When both are
 * non-zero (i.e., not all-zero bytes), they enable correlation of logs across
 * services and integration with tracing backends (Jaeger, Zipkin, OTel collector).
 * These fields can be set programmatically via @ref clog_set_trace_context or
 * parsed from the TRACEPARENT environment variable at formatter initialization.
 */
typedef struct {
    uint64_t timestamp;  /**< Microseconds since Unix epoch (Jan 1, 1970). Set by clock_gettime or
                            GetSystemTimeAsFileTime. */
    const char *file;    /**< Source file name (static string, usually from __FILE__ or
                            LOG_FILENAME_ONLY()). */
    const char *func;    /**< Source function name (static string, usually from __func__). */
    const char *module;  /**< Logical module name (set via log_set_module or via MDC/thread-local
                            context). */
    const char *tag;     /**< Optional tag/label for additional categorization; may be NULL. */
    const char *message; /**< Formatted message body (result of printf-style expansion). */
    log_level_t level;   /**< Severity of the log entry (LOG_LEVEL_* enum value). */
    uint32_t  tid; /**< Thread identifier derived from pthread_t (truncated hash for uniqueness). */
    uint32_t  pid; /**< Process identifier (getpid()). */
    int       line;         /**< Source line number where the log macro was invoked (__LINE__). */
    uint8_t   trace_id[16]; /**< W3C TraceContext trace-id; zero bytes indicate no active trace. */
    uint8_t   span_id[8];   /**< W3C TraceContext span-id; zero bytes indicate no active span. */
    clog_kv_t kv[CLOG_MAX_KV]; /**< Structured key-value attributes array. */
    size_t    kv_count;        /**< Number of valid entries in kv array (0..CLOG_MAX_KV). */
} log_record_t;

/**
 * @brief Map a clogx log level to an OpenTelemetry log severity number.
 *
 * The OpenTelemetry (OTel) Log Data Model defines severity numbers that differ
 * from the standard IETF syslog levels. This mapping enables clogx to produce
 * OTel-compatible logs when using the JSON format with OTel extensions.
 *
 * Reference: OpenTelemetry Specification, "Log Data Model" section:
 *   - TRACE    = 1
 *   - DEBUG    = 5
 *   - INFO     = 9
 *   - WARNING  = 13
 *   - ERROR    = 17
 *   - FATAL    = 21
 *
 * @param[in] level clogx severity level (one of LOG_LEVEL_*).
 * @return Corresponding OTel severity number (1-21), or 0 if level is unknown.
 *
 * Note: This is a pure function with no side effects; safe to call anytime.
 *       Inlined for performance in hot formatting paths.
 */
static inline int otel_severity_number(log_level_t level)
{
    switch (level) {
    case LOG_LEVEL_TRACE:
        return 1;
    case LOG_LEVEL_DEBUG:
        return 5;
    case LOG_LEVEL_INFO:
        return 9;
    case LOG_LEVEL_WARN:
        return 13;
    case LOG_LEVEL_ERROR:
        return 17;
    case LOG_LEVEL_FATAL:
        return 21;
    default:
        return 0;
    }
}

/**
 * @enum log_color_t
 * @brief ANSI color selectors used for console sink output.
 *
 * Each enumerated value corresponds to an ANSI escape sequence (defined in
 * the console sink implementation) that changes text color when writing to a
 * terminal that supports ANSI codes. Color mapping ensures consistency between
 * severity levels and visual cues for operators scanning logs.
 */
typedef enum {
    COLOR_NONE = 0, /**< No color / fallback (plain text). */
    COLOR_BLACK,    /**< Black text. */
    COLOR_RED,      /**< Red text (typically for errors/warnings). */
    COLOR_GREEN,    /**< Green text (typically for informational/debug). */
    COLOR_YELLOW,   /**< Yellow text (typically for warnings). */
    COLOR_BLUE,     /**< Blue text (typically for trace/debug). */
    COLOR_PURPLE,   /**< Purple text (typically for fatal/critical). */
    COLOR_CYAN,     /**< Cyan text (for meta-information). */
    COLOR_WHITE     /**< White text (fallback/highlight). */
} log_color_t;

/**
 * @brief Map a log severity level to an appropriate console color.
 *
 * Returns the ANSI color code that should be used when rendering this severity
 * in a color-enabled console sink. The mapping provides visual differentiation:
 *   - TRACE -> BLACK      (quiet, subtle)
 *   - DEBUG -> BLUE       (diagnostic)
 *   - INFO  -> GREEN      (normal operation)
 *   - WARN  -> YELLOW     (caution)
 *   - ERROR -> RED        (problem)
 *   - FATAL -> PURPLE     (critical)
 *
 * @param[in] level Log severity (log_level_t).
 *
 * @return Corresponding @ref log_color_t value suitable for ANSI coloring.
 *
 * Note: Console sinks should wrap text with ANSI escape sequences when emitting,
 *       e.g., "\033[31mERROR\033[0m" for red. This function only selects the color index;
 *       the escape sequence generation is sink-specific.
 */
static inline log_color_t get_log_color(log_level_t level)
{
    switch ((int)level) {
    case LOG_LEVEL_TRACE:
        return COLOR_BLACK;
    case LOG_LEVEL_DEBUG:
        return COLOR_BLUE;
    case LOG_LEVEL_INFO:
        return COLOR_GREEN;
    case LOG_LEVEL_WARN:
        return COLOR_YELLOW;
    case LOG_LEVEL_ERROR:
        return COLOR_RED;
    case LOG_LEVEL_FATAL:
        return COLOR_PURPLE;
    default:
        return COLOR_NONE;
    }
}

#endif /* LOG_RECORD_H */
