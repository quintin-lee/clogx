/**
 * @file log_sink.h
 * @brief Sink interface and factory functions for log destinations.
 *
 * A sink represents a destination where log records are written: console, file,
 * TCP socket, syslog, custom plugin, or OTLP endpoint. The sink abstraction
 * is polymorphic: each sink type implements the same callback interface (write,
 * flush, destroy) while maintaining its own private state.
 *
 * Architecture Overview:
 *   - The dispatcher (@ref log_dispatcher) receives log records and routes them
 *     to all enabled sinks after level filtering.
 *   - Sinks can be added at runtime via @ref log_add_sink (singleton) or
 *     logger_add_sink (multi-instance).
 *   - Custom sinks allow user-defined write behavior via @ref custom_sink_create.
 *   - Plugin sinks enable dynamic loading of additional sink types from shared objects.
 *
 * Lifecycle:
 *   1. Create sink via one of the factory functions.
 *   2. Add sink to logger/logger instance (must happen after init).
 *   3. Sinks receive formatted log lines via the write callback.
 *   4. Destroy via log_sink_destroy() or logger_destroy().
 *
 * @dot "Sink Polymorphism (Vtable Pattern)"
 * digraph sink_vtable {
 *     rankdir=TB;
 *     node [shape=box, style=filled, fontname="Helvetica", fontsize=10];
 *     edge [color="#666666", fontname="Helvetica", fontsize=9];
 *
 *     subgraph cluster_interface {
 *         label="log_sink_t (interface)";
 *         style=filled;
 *         fillcolor="#E3F2FD";
 *         color="#1976D2";
 *         vtable [label="vtable callbacks\n• write(buf, len)\n• flush()\n• destroy()\n•
 * atfork_child()" shape=record]; state [label="level, private_data" shape=record];
 *     }
 *
 *     subgraph cluster_impl {
 *         label="Concrete Sinks";
 *         style=filled;
 *         fillcolor="#E8F5E9";
 *         color="#388E3C";
 *         console [label="console_sink\ncolor, stream"];
 *         file [label="file_sink\nfp, rotation"];
 *         socket [label="socket_sink\nfd, tls_ctx"];
 *         syslog [label="syslog_sink\nident"];
 *         otlp [label="otlp_sink\nfile_out"];
 *         custom [label="custom_sink\nuser_cb"];
 *     }
 *
 *     dispatcher [label="Dispatcher\n(forEach sink)" shape=ellipse, fillcolor="#FFF3E0"];
 *
 *     dispatcher -> vtable [label="call write/flush"];
 *     console -> vtable [style=dashed, label="implements"];
 *     file -> vtable [style=dashed, label="implements"];
 *     socket -> vtable [style=dashed, label="implements"];
 *     syslog -> vtable [style=dashed, label="implements"];
 *     otlp -> vtable [style=dashed, label="implements"];
 *     custom -> vtable [style=dashed, label="implements"];
 * }
 * @enddot
 */

#ifndef LOG_SINK_H
#define LOG_SINK_H

#include "log_record.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef CLOGX_API
#if defined(__GNUC__) || defined(__clang__)
#define CLOGX_API __attribute__((visibility("default")))
#else
#define CLOGX_API
#endif
#endif

    /** @brief Forward declaration used by @ref clogx_plugin_t. */
    struct clogx_plugin_t;

/**
 * @struct log_sink
 * @brief Polymorphic sink interface with private implementation state.
 *
 * This structure defines the contract that every sink must implement. Built-in
 * sinks (console, file, socket, etc.) and plugin sinks all fill out this
 * struct appropriately. The vtable-style function pointers enable uniform
 * treatment of heterogeneous sink implementations.
 *
 * ABI Stability Note:
 *   - The order and size of fields MUST NOT change without bumping
 *     @ref CLOGX_PLUGIN_ABI_VERSION.  Adding new fields at the end may be safe
 *     but requires careful compatibility testing.
 *   - Every sink factory sets @c abi_version to the current version. The
 *     plugin loader validates this before calling any callbacks.
 */
typedef struct log_sink log_sink_t;

struct log_sink {
    /**
     * @brief ABI version tag; must equal CLOGX_PLUGIN_ABI_VERSION.
     *
     * Set by every sink factory (built-in and plugin) to the current
     * @ref CLOGX_PLUGIN_ABI_VERSION.  The plugin loader checks this
     * when a .so is loaded; mismatched versions are rejected early.
     * For built-in sinks, this is set automatically in the factory function.
     */
    uint32_t abi_version;

    /**
     * @brief Write @p len bytes from @p buf to the sink destination.
     *
     * This is the core writing callback invoked by the dispatcher for each
     * formatted log record. Should write exactly @p len bytes on success,
     * or return -1 on error (setting errno appropriately if applicable).
     *
     * The @p buffer contains pre-formatted text (with or without trailing
     * newline depending on format configuration). The sink should handle
     * partial writes if necessary (though most simple sinks write all at once).
     *
     * @param[in] sink Pointer to the sink instance.
     * @param[in] buf Buffer containing data to write (null-terminated not guaranteed).
     * @param[in] len Number of bytes to write (may be zero).
     *
     * @return Number of bytes written on success, or -1 on error.
     *
     * Note: This function may be called concurrently from multiple threads
     *       (the worker thread serializes calls per sink). Implementations
     *       that require thread-safety should add appropriate locking.
     */
    int (*write)(log_sink_t *sink, const char *buf, size_t len);

    /**
     * @brief Flush buffered output to the underlying destination.
     *
     * Ensures all previously written data is persisted or transmitted.
     * Important for ensuring logs survive program crashes or before exit.
     *
     * @param[in] sink Pointer to the sink instance.
     *
     * Note Called during @ref log_flush, @ref log_destroy, and before reload.
     *       No return value—errors here are typically logged to stderr.
     */
    void (*flush)(log_sink_t *sink);

    /**
     * @brief Release resources and free the sink object.
     *
     * Destroys the sink, freeing any allocated memory, closing file/socket
     * descriptors, and cleaning up private state. After this call, the sink
     * pointer becomes invalid and must not be used.
     *
     * @param[in] sink Pointer to the sink instance (may be NULL in caller's mistake).
     *
     * Note Called by @ref log_destroy and during sink removal. Sink-owned
     *       private data should be freed here (typically via free(private_data)).
     */
    void (*destroy)(log_sink_t *sink);

    /**
     * @brief Handle fork in child process: re-open file/socket descriptor.
     *
     * When the process forks (e.g., after @ref fork() in a pre-fork server),
     * file descriptors and sockets become invalid in the child due to copy-on-write
     * semantics. This callback re-initializes the sink in the child process.
     *
     * @param[in] sink Pointer to the sink instance.
     *
     * Note Only relevant for sinks that hold kernel resources (files, sockets).
     *       Console sinks typically have no-op implementations. Registered via
     *       pthread_atfork_handler in signal_handler.c.
     */
    void (*atfork_child)(log_sink_t *sink);

    /**
     * @brief Implementation-private data pointer.
     *
     * Opaque pointer holding sink-specific state (file handle, connection info,
     * color state, mutexes, buffers, etc.). Cast to the appropriate concrete type
     * within sink implementation functions.
     */
    void *private_data;

    /**
     * @brief Minimum severity level this sink accepts.
     *
     * Messages below this level are silently dropped by the dispatcher before
     * reaching the sink's write callback. Allows per-sink level filtering; e.g.,
     * a file sink could accept ERROR+ while console accepts DEBUG+.
     *
     * Initialised to LOG_LEVEL_TRACE (accepts everything) in the factory
     * helpers; override with log_sink_set_level(). Can be queried with
     * log_sink_get_level().
     */
    log_level_t min_level;
};

/**
 * @brief Create a stdout console sink.
 *
 * Produces a sink that writes formatted log lines to standard output. If
 * color is enabled, ANSI escape sequences wrap messages according to their
 * severity level (see @ref get_log_color for the mapping).
 *
 * @param[in] use_color When true, the dispatcher may wrap output in ANSI colors.
 *                      False outputs plain text without color codes.
 *
 * @return New sink pointer, or NULL on allocation failure.
 *
 * Note Caller owns the returned sink and must pass it to log_add_sink or
 *       logger_add_sink. The sink will be destroyed by log_destroy. Thread-safe:
 *       stdout is internally synchronized but concurrent writes from multiple
 *       sinks may interleave.
 */
CLOGX_API log_sink_t *console_sink_create(bool use_color);

/**
 * @brief Create a stderr console sink.
 *
 * Similar to console_sink_create but writes to standard error instead of
 * standard output. Useful for separating error logs from information/logs,
 * or when redirecting stdout to a file while keeping stderr visible.
 *
 * @param[in] use_color When true, the dispatcher may wrap output in ANSI colors.
 *
 * @return New sink pointer, or NULL on allocation failure.
 *
 * Note Thread-safety considerations same as console_sink_create. Error output
 *       is typically unbuffered, making it suitable for crash diagnostics.
 */
CLOGX_API log_sink_t *console_sink_create_stderr(bool use_color);

/**
 * @brief Test whether @p sink is a color-enabled console sink.
 *
 * Queries the internal flag indicating whether this particular console sink
 * was created with color support enabled. Used by the dispatcher to decide
 * whether to apply ANSI color wrapping around log messages.
 *
 * @param[in] sink Sink to query (may be NULL).
 *
 * @return true if color output is enabled for this sink, false otherwise or if sink is NULL.
 *
 * Note Fast inline check against a boolean stored in private_data. Safe for hot path.
 */
CLOGX_API bool console_sink_is_color_enabled(log_sink_t *sink);

/**
 * @brief Create an append-only file sink with size-based rotation.
 *
 * Opens the specified log file for appending, creating parent directories
 * as needed. When the file reaches @p max_size bytes, it is renamed with
 * a numbered backup suffix (.1, .2, ...), and a new file is opened. Old
 * backups beyond @p count are deleted automatically.
 *
 * Rotation is checked on each write; background rotation occurs within the
 * writing thread (not async). Use a sufficiently large max_size to avoid
 * performance impact from frequent renames.
 *
 * @param[in] path     Destination file path (e.g., "/var/log/app.log"). Must be null-terminated.
 * @param[in] max_size Rotate threshold in bytes (0 disables rotation forever).
 *                     Accepts values like 1024, 1024*1024 (1MB), etc.
 * @param[in] backups  Number of numbered backups to retain (`.1` .. `.N`).
 *                     Set to 1 to keep only one backup; higher values keep more history.
 *
 * @return New sink pointer, or NULL on error (directory creation, file open, allocation).
 *
 * Note The sink holds an open FILE* handle internally. The directory path must
 *       exist beforehand (parent directories are created if missing). File mode
 *       is typically 0640. Concurrent writes from the worker thread are serialized.
 */
CLOGX_API log_sink_t *file_sink_create(const char *path, uint64_t max_size, int backups);

/**
 * @brief Create a lazy-connect TCP socket sink.
 *
 * Creates a sink that sends log records to a remote host via TCP. Connection
 * is established lazily on the first write operation (hence "lazy-connect").
 * If the connection drops, it is automatically re-established on subsequent writes.
 *
 * Suitable for sending logs to a centralized logging server. Uses non-blocking
 * I/O with reconnection logic to handle network intermittency gracefully.
 *
 * @param[in] host IPv4 address string (e.g., `"127.0.0.1"` or `"192.168.1.100"`).
 *                 Hostnames are not resolved—use IP addresses for simplicity.
 * @param[in] port Destination port in host byte order (1..65535).
 *
 * @return New sink pointer, or NULL on invalid arguments / allocation failure.
 *
 * Note Requires linking with clogx (no TLS unless compiled with TLS enabled). The sink
 *       maintains its own socket descriptor; automatic reconnection happens on write errors.
 *       Fork safety: the atfork_child handler closes and re-establishes the connection.
 */
CLOGX_API log_sink_t *socket_sink_create(const char *host, int port);

/**
 * @brief Create a lazy-connect TCP socket sink with optional TLS encryption.
 *
 * Same as socket_sink_create but with TLS transport security using OpenSSL.
 * Requires clogx to be compiled with TLS support (@ref CLOG_ENABLE_TLS).
 *
 * @param[in] host        IPv4 address string (e.g., `"127.0.0.1"`).
 * @param[in] port        Destination port in host byte order (1..65535).
 * @param[in] use_tls     Whether to enable TLS encryption on connection.
 *                       If true, performs SSL/TLS handshake on connect.
 * @param[in] ca_file     Optional path to CA certificate file for server verification.
 *                        If NULL, self-signed or unknown certificates may still connect
 *                        unless skip_verify is false.
 * @param[in] skip_verify Whether to skip server certificate verification.
 *                        True = accept any cert (insecure, for testing only).
 *                        False = verify chain against ca_file or system store.
 *
 * @return New sink pointer, or NULL on invalid arguments / allocation failure / SSL init error.
 *
 * Note Thread-safe: each sink has its own SSL_CTX/SSL object. Performance overhead
 *       of TLS handshake is incurred only on first connection or reconnect.
 */
CLOGX_API log_sink_t *socket_sink_create_tls(
    const char *host, int port, bool use_tls, const char *ca_file, bool skip_verify);

/**
 * @brief Create a custom user-defined sink plugin.
 *
 * Allows application code to define a completely custom sink by providing
 * write, flush, and destroy callbacks. Private data is passed through to
 * the callbacks, enabling storage of per-sink state.
 *
 * Ideal for writing to special destinations (memory buffers, external APIs,
 * database inserts, etc.) without compiling new built-in sinks into the library.
 *
 * @param[in] write_fn    Write callback (must not be NULL). See @ref log_sink::write for signature.
 * @param[in] flush_fn    Optional flush callback (or NULL for no-flush behavior).
 * @param[in] destroy_fn  Optional destroy callback (or NULL for no cleanup).
 * @param[in] private_data Implementation-private pointer passed to callbacks.
 *
 * @return New custom sink pointer, or NULL on invalid arguments / allocation failure.
 *
 * Note The caller retains ownership of @p private_data; the destroy callback is
 *       responsible for freeing it if necessary. This sink can be added via
 *       log_add_sink or logger_add_sink just like built-in sinks.
 */
CLOGX_API log_sink_t *
custom_sink_create(int (*write_fn)(log_sink_t *sink, const char *buf, size_t len),
                   void (*flush_fn)(log_sink_t *sink),
                   void (*destroy_fn)(log_sink_t *sink),
                   void *private_data);

/**
 * @brief Retrieve the implementation-private pointer of a custom sink.
 *
 * Useful for accessing custom sink state from outside the sink implementation.
 * Returns NULL if the sink is NULL or was not created via custom_sink_create.
 *
 * @param[in] sink Target custom sink (may be NULL).
 *
 * @return Implementation-private pointer, or NULL if sink is NULL or not a custom sink.
 *
 * Note Should be paired with custom_sink_create for proper type casting. No error checking
 *       beyond NULL validation; responsibility lies with the caller to cast correctly.
 */
CLOGX_API void *custom_sink_get_private_data(log_sink_t *sink);

/**
 * @brief Create a POSIX syslog sink.
 *
 * Wraps the system syslog(3) facility, sending log messages to the system log daemon.
 * Only available on platforms with POSIX syslog support (Linux, BSD, macOS). Not available on
 * Windows.
 *
 * @param[in] ident    Program identifier tag string (e.g., "my_app"). Appears in syslog headers.
 *                     Typically matches the application name; set to NULL to use default.
 * @param[in] facility Syslog facility constant (e.g., LOG_USER, LOG_DAEMON,
 * LOG_LOCAL0..LOG_LOCAL7). Determines which subsystem the log message belongs to.
 *
 * @return New sink pointer, or NULL on platforms without syslog / allocation failure.
 *
 * Note Calls openlog() at initialization with PID tracking. Messages include the ident prefix.
 *       The sink uses the syslog(LOG_*) API directly; no manual socket handling required.
 */
CLOGX_API log_sink_t *syslog_sink_create(const char *ident, int facility);

/**
 * @brief Create an OpenTelemetry OTLP JSON log sink.
 *
 * Formats log records as OpenTelemetry OTLP JSON logs (`resourceLogs` payload)
 * and emits them to an OTLP/HTTP collector or file endpoint. Supports both
 * HTTP POST to an OTLP collector endpoint and local file output.
 *
 * Useful for integrating with modern observability backends that consume OTLP
 * such as Prometheus, Jaeger, Zipkin, Elastic, or commercial APM solutions.
 *
 * @param[in] endpoint     Target socket/file path or HTTP endpoint host:port (e.g.,
 *                         "http://localhost:4318/v1/logs", "127.0.0.1:4318", or
 * "/var/log/otlp.json"). URL format triggers HTTP POST; plain host:port or file path determines
 * behavior.
 * @param[in] service_name Service name string attribute (defaults to "clogx_service" if
 * NULL/empty). Used as the resource name in the OTel payload.
 *
 * @return New sink pointer, or NULL on allocation failure / invalid arguments.
 *
 * Note This sink internally formats records as OTLP JSON, so the formatter's
 *       output format should not interfere with this sink's own formatting.
 *       HTTP mode requires networking; may block on network timeouts.
 */
CLOGX_API log_sink_t *otlp_sink_create(const char *endpoint, const char *service_name);

/**
 * @brief Set the minimum severity level for a sink.
 *
 * Messages below this level are dropped by the dispatcher before reaching
 * the sink's write callback. Allows fine-grained control over what different
 * sinks see—for example, console can show DEBUG+ while file only stores ERROR+.
 *
 * Default is LOG_LEVEL_TRACE (accepts everything) when a sink is created.
 *
 * @param[in] sink  Target sink (must not be NULL).
 * @param[in] level Minimum level to accept (LOG_LEVEL_TRACE .. LOG_LEVEL_FATAL).
 *
 * Note Directly modifies the @c min_level field of the log_sink struct.
 *       Thread-safe: the dispatcher checks min_level under lock when iterating sinks.
 */
static inline void log_sink_set_level(log_sink_t *sink, log_level_t level)
{
    if (sink) {
        sink->min_level = level;
    }
}

/**
 * @brief Get the minimum severity level for a sink.
 *
 * Returns the current minimum level setting for @p sink. If @p sink is NULL,
 * returns LOG_LEVEL_TRACE (the conservative default).
 *
 * @param[in] sink Sink to query (may be NULL).
 *
 * @return Current minimum level, or LOG_LEVEL_TRACE if sink is NULL.
 *
 * Note Inline accessor for fast read. Matches the policy of log_sink_set_level.
 */
static inline log_level_t log_sink_get_level(const log_sink_t *sink)
{
    return sink ? sink->min_level : LOG_LEVEL_TRACE;
}

#endif /* LOG_SINK_H */
