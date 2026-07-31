/**
 * @file clogx_plugin.h
 * @brief Plugin ABI contract for dynamically-loadable sink modules.
 *
 * The clogx plugin system allows extending the logging library with custom
 * sink implementations loaded at runtime as shared objects (.so files). This
 * defines a stable ABI (Application Binary Interface) that plugins must adhere to,
 * enabling third-party or user-defined sinks without recompiling the core library.
 *
 * Plugin Architecture:
 *   1. A plugin author creates a shared library (.so) exporting exactly two symbols:
 *      - @ref CLOGX_PLUGIN_DESC_SYM: returns plugin metadata descriptor
 *      - @ref CLOGX_PLUGIN_CREATE_SYM: factory function to create sink instances
 *   2. The main application loads the plugin via @ref log_plugin_load which calls
 *      dlopen and dlsym internally, then verifies the ABI version.
 *   3. Sink instances are created from the loaded handle via @ref log_plugin_create_sink
 *      and registered with the logger via log_add_sink or logger_add_sink.
 *   4. Plugins can be scanned automatically from a directory using @ref log_plugin_scan.
 *
 * Example Plugin Source (kafka_sink.c):
 * @code
 * #include "clogx_plugin.h"
 * #include <string.h>
 *
 * static const clogx_plugin_t desc = {
 *     .abi_version = CLOGX_PLUGIN_ABI_VERSION,
 *     .plugin_version = 1,
 *     .caps = CLOGX_PLUGIN_CAP_NETWORK,
 *     .name = "kafka",
 *     .description = "Apache Kafka sink for clogx",
 * };
 *
 * static const clogx_plugin_t *clogx_plugin_desc(void) { return &desc; }
 *
 * static log_sink_t* kafka_create(const char *params_json) {
 *     // Parse params_json, allocate sink, set callbacks...
 *     // Must initialize sink->abi_version = CLOGX_PLUGIN_ABI_VERSION
 * }
 *
 * log_sink_t *clogx_plugin_create(const char *params_json) { return kafka_create(params_json); }
 * @endcode
 *
 * Thread Safety: All public plugin APIs are thread-safe. The loader uses internal
 * locks to protect the plugin cache and handle lifecycle.
 */

#ifndef CLOGX_PLUGIN_H
#define CLOGX_PLUGIN_H

#include "log_sink.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  ABI version                                                       */
/* ------------------------------------------------------------------ */

/**
 * @def CLOGX_PLUGIN_ABI_VERSION
 * @brief Current plugin ABI version number.
 *
 * Must match @c abi_version in both @ref clogx_plugin_t and every
 * @ref log_sink_t created by the plugin. Bumped only on incompatible changes
 * to the ABI contract—such as struct layout modifications, callback signature
 * changes, or semantic meaning shifts. Plugin loaders reject versions that
 * do not match exactly.
 *
 * Versioning Policy:
 *   - Increment when struct field order/size changes.
 *   - Increment if callback function signatures change.
 *   - Do NOT increment for new optional features backward compatible with old plugins.
 */
#define CLOGX_PLUGIN_ABI_VERSION 1

/* ------------------------------------------------------------------ */
/*  Capability flags                                                  */
/* ------------------------------------------------------------------ */

#define CLOGX_PLUGIN_CAP_NONE 0ULL /**< No special capabilities; default sink. */
/** Plugin supports batched writes via write_batch (future expansion). */
#define CLOGX_PLUGIN_CAP_BATCH (1ULL << 0)
/** Plugin requires network I/O (influences fork-safety behaviour). */
#define CLOGX_PLUGIN_CAP_NETWORK (1ULL << 1)

/* ------------------------------------------------------------------ */
/*  Plugin descriptor                                                 */
/* ------------------------------------------------------------------ */

/**
 * @struct clogx_plugin_t
 * @brief Read-only metadata exported by every plugin .so.
 *
 * This structure describes the plugin to the loader. It must be placed in
 * read-only memory (`.rodata`) by the plugin author to prevent accidental
 * modification and ensure stability across dlopen/dlclose cycles.
 *
 * Returned by the @c clogx_plugin_desc() symbol. The loader validates this
 * structure before allowing any sink creation; if the ABI version mismatch,
 * the plugin is rejected immediately.
 *
 * Design Guidelines:
 *   - Keep this structure small and immutable after compilation.
 *   - Use string literals for name and description—they must persist.
 *   - Set caps appropriately so the loader knows about fork-safety requirements.
 *
 * Example initialization:
 * @code
 * static const clogx_plugin_t desc = {
 *     .abi_version = CLOGX_PLUGIN_ABI_VERSION,
 *     .plugin_version = 1,
 *     .caps = CLOGX_PLUGIN_CAP_NETWORK,
 *     .name = "kafka",
 *     .description = "Apache Kafka sink for clogx",
 * };
 * @endcode
 */
typedef struct {
    uint32_t    abi_version;    /**< MUST equal CLOGX_PLUGIN_ABI_VERSION. Checked by loader. */
    uint32_t    plugin_version; /**< Plugin's own semantic version (independent of ABI). */
    uint64_t    caps;           /**< Bitmask of CLOGX_PLUGIN_CAP_* flags describing capabilities. */
    const char *name;           /**< Short name, e.g., "kafka", "syslog-ext". Not NULL. */
    const char *description;    /**< Human-readable one-liner, e.g., "Kafka log sink". Not NULL. */
} clogx_plugin_t;

/* ------------------------------------------------------------------ */
/*  Entry-point types and symbol names                                */
/* ------------------------------------------------------------------ */

/**
 * @typedef clogx_plugin_create_fn
 * @brief Sink factory function type.
 *
 * Parses @p params_json (nullable) and returns a new @ref log_sink_t instance.
 * The returned sink must have its @c abi_version field set correctly and all
 * function pointers (write, flush, destroy, atfork_child) properly initialized.
 *
 * @param[in] params_json Opaque JSON configuration string passed from YAML plugin config.
 *                        May be NULL if no parameters are provided.
 *
 * @return New sink pointer on success, or NULL on allocation/error (sets errno).
 *
 * Note The caller (dispatcher) owns the returned sink and will call destroy when done.
 */
typedef log_sink_t *(*clogx_plugin_create_fn)(const char *params_json);

/**
 * @typedef clogx_plugin_desc_fn
 * @brief Descriptor query function type.
 *
 * Returns a pointer to the plugin's read-only metadata descriptor. The pointer
 * must remain valid for the lifetime of the loaded module (typically points to
 * static data in .rodata).
 *
 * @return Pointer to constant clogx_plugin_t structure. Never returns NULL.
 */
typedef const clogx_plugin_t *(*clogx_plugin_desc_fn)(void);

/**
 * @def CLOGX_PLUGIN_DESC_SYM
 * @symbol Name that clogx's dlopen wrapper looks for via dlsym.
 *
 * Every plugin must export a function named exactly "clogx_plugin_desc" with
 * signature matching @c clogx_plugin_desc_fn. The loader uses dlsym to find
 * this symbol; failure to export it results in load rejection.
 */
#define CLOGX_PLUGIN_DESC_SYM "clogx_plugin_desc"

/**
 * @def CLOGX_PLUGIN_CREATE_SYM
 * @symbol Name for the factory entry point.
 *
 * Every plugin must export a function named exactly "clogx_plugin_create" with
 * signature matching @c clogx_plugin_create_fn. This is the entry point called
 * to instantiate new sink handles from the plugin.
 */
#define CLOGX_PLUGIN_CREATE_SYM "clogx_plugin_create"

/* ------------------------------------------------------------------ */
/*  Opaque handle returned by log_plugin_load / log_plugin_scan        */
/* ------------------------------------------------------------------ */

/**
 * @struct clogx_plugin_handle_t
 * @brief Opaque handle to a loaded plugin .so.
 *
 * Obtained via @ref log_plugin_load or @ref log_plugin_scan. Multiple sink
 * instances can be created from one handle (each call to
 * @ref log_plugin_create_sink allocates a new sink). The handle encapsulates
 * the DLOpened library handle, cached descriptor, and reference counting for
 * automatic cleanup at log_destroy.
 *
 * Users should treat this as opaque—do not dereference or inspect fields directly.
 * Pass the handle to other plugin API functions as needed.
 */
typedef struct clogx_plugin_handle clogx_plugin_handle_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Load a plugin .so and verify its ABI version.
 *
 * Opens the shared object with dlopen, locates the required symbols (desc
 * and create), checks the ABI version against the current library version,
 * and caches the handle for reuse. If already loaded, returns the existing handle.
 *
 * @param[in] so_path Absolute or relative path to the shared object file (.so).
 *                    Path is resolved relative to current working directory or
 *                    the plugin scan directory (if used with log_plugin_scan).
 *
 * @return Handle pointer on success, or NULL on error (dlopen/dlsym/ABI mismatch).
 *         Use log_strerror(clogx_errno) to diagnose failure.
 *
 * On success the handle is cached internally; the caller does NOT need to manage
 * its lifetime unless they call @ref log_plugin_unload explicitly. Handles are
 * automatically unloaded at @ref log_destroy when the logging subsystem shuts down.
 *
 * @note Thread-safe: uses internal mutex to protect the plugin cache. Idempotent—
 *       loading the same path twice returns the same handle without reloading.
 */
clogx_plugin_handle_t *log_plugin_load(const char *so_path);

/**
 * @brief Unload a previously loaded plugin handle.
 *
 * Calls dlclose on the shared object, removing it from the internal cache. All
 * sinks created from this handle become dangling pointers and MUST have been
 * destroyed before calling this function—otherwise use-after-free occurs.
 *
 * @param[in] h Handle from @ref log_plugin_load or NULL (no-op safely).
 *
 * Note Thread-safe. Unloading while in use (sinks still active) is dangerous—the
 *       caller is responsible for ensuring no references remain. Prefer relying
 *       on automatic unload at log_destroy rather than manual unloading.
 */
void log_plugin_unload(clogx_plugin_handle_t *h);

/**
 * @brief Create a sink instance from a loaded plugin.
 *
 * Invokes the plugin's create callback (@c clogx_plugin_create) with the given
 * params_json string, returning a new @ref log_sink_t pointer. The sink is fully
 * initialized with correct ABI version and vtable pointers.
 *
 * @param[in] h Plugin handle (must not be NULL and must be successfully loaded).
 * @param[in] params_json Opaque JSON configuration string parsed by the plugin.
 *                        Can be NULL if the plugin accepts no parameters.
 *
 * @return A new @ref log_sink_t, or NULL on failure (plugin allocation/parse error).
 *
 * The returned sink is owned by the caller (ultimately by the dispatcher when
 * added via log_add_sink) and will be destroyed via its @c sink->destroy callback
 * when the sink is removed or the logger is destroyed. Do not free manually.
 */
log_sink_t *log_plugin_create_sink(clogx_plugin_handle_t *h, const char *params_json);

/**
 * @brief Query a loaded plugin's descriptor.
 *
 * Returns read-only metadata about the plugin, including name, description,
 * capabilities, and version information. Safe to call anytime after successful load.
 *
 * @param[in] h Plugin handle (must not be NULL).
 *
 * @return Pointer to the plugin's read-only descriptor structure. Never NULL.
 *
 * Note The returned pointer remains valid until the plugin is unloaded. Useful
 *       for debugging, UI display, or validating plugin identity before use.
 */
const clogx_plugin_t *log_plugin_info(clogx_plugin_handle_t *h);

/**
 * @brief Scan a directory for plugin .so files and load them.
 *
 * Scans the specified directory for files matching `*clogx*.so` or ending in `.so`,
 * tries to load each one via @ref log_plugin_load (which skips already-loaded paths),
 * and returns handles to successfully loaded plugins. Useful for auto-discovering
 * plugins at startup without hardcoding individual paths.
 *
 * @param[in]  dir Directory path to scan (e.g., "/usr/lib/clogx/plugins"). Must exist.
 * @param[out] out Array to receive handles (may be NULL to probe count first).
 * @param[in]  max Capacity of @p out array. Pass 0 when @p out is NULL to get count.
 *
 * @return Number of plugins successfully loaded (positive), or -1 on error (directory scan
 * failure).
 *
 * When @p out is NULL and @p max is 0, the function simply returns the count of available
 * plugins without loading them—useful for sizing an output buffer beforehand.
 *
 * Note The handles are also registered in the internal cache and will be cleaned up
 *       automatically at @ref log_destroy. Do not call log_plugin_unload on these handles
 *       manually unless you want early unload (risks dangling sink pointers).
 */
int log_plugin_scan(const char *dir, clogx_plugin_handle_t **out, int max);

#ifdef __cplusplus
}
#endif

#endif /* CLOGX_PLUGIN_H */
