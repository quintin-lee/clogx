/**
 * @file clogx_plugin.h
 * @brief Plugin ABI contract for dynamically-loadable sink modules.
 *
 * Any shared library that follows this ABI can be loaded at runtime via
 * dlopen(3) and registered with the clogx dispatcher.  The plugin .so must
 * export exactly two symbols:
 *
 *   const clogx_plugin_t *clogx_plugin_desc(void);
 *   log_sink_t *clogx_plugin_create(const char *params_json);
 *
 * See @ref CLOGX_PLUGIN_DESC_SYM and @ref CLOGX_PLUGIN_CREATE_SYM.
 *
 * @par ABI Versioning
 * Every plugin sets @c abi_version to @ref CLOGX_PLUGIN_ABI_VERSION in both
 * the descriptor and every @ref log_sink_t it creates.  The loader verifies
 * the descriptor before creating sinks and will reject mismatched plugins.
 * Bump @ref CLOGX_PLUGIN_ABI_VERSION only on incompatible layout changes.
 */

#ifndef CLOGX_PLUGIN_H
#define CLOGX_PLUGIN_H

#include <stdint.h>
#include <stddef.h>
#include "log_sink.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  ABI version                                                       */
/* ------------------------------------------------------------------ */

/**
 * @def CLOGX_PLUGIN_ABI_VERSION
 * @brief Current plugin ABI version.
 *
 * Must match @c abi_version in both @ref clogx_plugin_t and
 * @ref log_sink_t.  Bumped only on incompatible changes to the ABI
 * contract (struct layout, semantics of existing callbacks).
 */
#define CLOGX_PLUGIN_ABI_VERSION 1

/* ------------------------------------------------------------------ */
/*  Capability flags                                                  */
/* ------------------------------------------------------------------ */

#define CLOGX_PLUGIN_CAP_NONE 0ULL
/** Plugin supports batched writes via write_batch (future). */
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
 * Returned by @c clogx_plugin_desc().  The structure should be placed in
 * `.rodata` by the plugin author:
 *
 * @code
 * static const clogx_plugin_t desc = {
 *     .abi_version = CLOGX_PLUGIN_ABI_VERSION,
 *     .plugin_version = 1,
 *     .caps = CLOGX_PLUGIN_CAP_NONE,
 *     .name = "kafka",
 *     .description = "Apache Kafka sink for clogx",
 * };
 * const clogx_plugin_t *clogx_plugin_desc(void) { return &desc; }
 * @endcode
 */
typedef struct {
    uint32_t abi_version;    /**< MUST equal CLOGX_PLUGIN_ABI_VERSION. */
    uint32_t plugin_version; /**< Plugin's own semantic version.       */
    uint64_t caps;           /**< Bitmask of CLOGX_PLUGIN_CAP_* flags. */
    const char *name;        /**< Short name, e.g. "kafka".            */
    const char *description; /**< Human-readable one-liner.            */
} clogx_plugin_t;

/* ------------------------------------------------------------------ */
/*  Entry-point types and symbol names                                */
/* ------------------------------------------------------------------ */

/** @brief Factory: parse @p params_json (nullable) and return a new sink. */
typedef log_sink_t *(*clogx_plugin_create_fn)(const char *params_json);

/** @brief Descriptor query. */
typedef const clogx_plugin_t *(*clogx_plugin_desc_fn)(void);

/** Symbol name that clogx's dlopen wrapper looks for via dlsym. */
#define CLOGX_PLUGIN_DESC_SYM "clogx_plugin_desc"

/** Symbol name for the factory entry point. */
#define CLOGX_PLUGIN_CREATE_SYM "clogx_plugin_create"

/* ------------------------------------------------------------------ */
/*  Opaque handle returned by log_plugin_load / log_plugin_scan        */
/* ------------------------------------------------------------------ */

/**
 * @struct clogx_plugin_handle_t
 * @brief Opaque handle to a loaded plugin .so.
 *
 * Obtained via @ref log_plugin_load or @ref log_plugin_scan.  Multiple
 * sink instances can be created from one handle.
 */
typedef struct clogx_plugin_handle clogx_plugin_handle_t;

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief Load a plugin .so and verify its ABI version.
 * @param[in] so_path  Absolute or relative path to the shared object.
 * @return Handle, or NULL on error (dlopen/dlsym/ABI mismatch).
 *
 * On success the handle is cached internally; the caller does NOT need to
 * manage its lifetime unless they call @ref log_plugin_unload explicitly.
 * Handles are automatically unloaded at @ref log_destroy.
 *
 * @note Thread-safe.
 */
clogx_plugin_handle_t *log_plugin_load(const char *so_path);

/**
 * @brief Unload a previously loaded plugin handle.
 * @param[in] h  Handle from @ref log_plugin_load or NULL (no-op).
 *
 * All sinks created from this handle become dangling pointers and MUST
 * have been destroyed before calling this function.
 *
 * @note Thread-safe.
 */
void log_plugin_unload(clogx_plugin_handle_t *h);

/**
 * @brief Create a sink instance from a loaded plugin.
 * @param[in] h            Plugin handle (must not be NULL).
 * @param[in] params_json  Opaque JSON configuration string, or NULL.
 * @return A new @ref log_sink_t, or NULL on failure.
 *
 * The returned sink is owned by the caller (ultimately by the dispatcher)
 * and will be destroyed via its @c destroy callback.
 */
log_sink_t *log_plugin_create_sink(clogx_plugin_handle_t *h, const char *params_json);

/**
 * @brief Query a loaded plugin's descriptor.
 * @param[in] h  Plugin handle (must not be NULL).
 * @return Pointer to the plugin's read-only descriptor.
 */
const clogx_plugin_t *log_plugin_info(clogx_plugin_handle_t *h);

/**
 * @brief Scan a directory for plugin .so files and load them.
 * @param[in]  dir  Directory path (e.g. "/usr/lib/clogx/plugins").
 * @param[out] out  Array to receive handles (may be NULL to probe count).
 * @param[in]  max  Capacity of @p out.  Pass 0 when @p out is NULL.
 * @return Number of plugins successfully loaded, or -1 on error.
 *
 * Scans for files matching `*clogx*.so` or ending in `.so`, tries to load
 * each one via @ref log_plugin_load.  Already-loaded .so paths are skipped.
 *
 * @note The handles are also registered in the internal cache and will be
 *       cleaned up at @ref log_destroy.
 */
int log_plugin_scan(const char *dir, clogx_plugin_handle_t **out, int max);

#ifdef __cplusplus
}
#endif

#endif /* CLOGX_PLUGIN_H */
