/**
 * @file plugin_loader.c
 * @brief Plugin ABI loader: dlopen wrapper, handle cache, directory scanning.
 *
 * Every plugin .so is loaded once and cached in a small global array.
 * Multiple sinks can be created from the same handle.  Handles are
 * automatically unloaded at @ref log_plugin_shutdown_all (called from
 * @ref log_destroy).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* Plugin ABI is not supported on Windows.
 * Stubs are provided so the library links and the rest of the
 * sink/logger pipeline works without dynamic-load support.          */
#include "clogx_plugin.h"
#include "log_config.h"

clogx_plugin_handle_t *log_plugin_load(const char *so_path) {
    (void)so_path;
    return NULL;
}

void log_plugin_unload(clogx_plugin_handle_t *h) {
    (void)h;
}

log_sink_t *log_plugin_create_sink(clogx_plugin_handle_t *h, const char *params_json) {
    (void)h;
    (void)params_json;
    return NULL;
}

const clogx_plugin_t *log_plugin_info(clogx_plugin_handle_t *h) {
    (void)h;
    return NULL;
}

int log_plugin_scan(const char *dir, clogx_plugin_handle_t **out, int max) {
    (void)dir;
    (void)out;
    (void)max;
    return 0;
}

int log_plugin_create_sinks_from_config(const log_config_t *cfg, log_sink_t **out_sinks,
                                        int max_out) {
    (void)cfg;
    (void)out_sinks;
    (void)max_out;
    return 0;
}

void log_plugin_shutdown_all(void) {
}

#else /* POSIX */

#include <dlfcn.h>
#include <dirent.h>
#include "clog_port.h"
#include "clogx_plugin.h"
#include "log.h"

/* ------------------------------------------------------------------ */
/*  Internal handle cache                                             */
/* ------------------------------------------------------------------ */

/** @brief Maximum number of concurrently loaded plugin .so files. */
#define CLOGX_MAX_LOADED_PLUGINS 16

struct clogx_plugin_handle {
    void *dl_handle;                  /**< Result from dlopen(3).          */
    clogx_plugin_create_fn create_fn; /**< Resolved clogx_plugin_create.   */
    const clogx_plugin_t *desc;       /**< Cached descriptor pointer.      */
    char so_path[CLOG_MAX_PATH_SIZE]; /**< Canonicalised .so path.         */
    int used;                         /**< Non-zero when slot is occupied. */
};

/** Global plugin handle cache — handles are long-lived (reload-safe). */
static struct clogx_plugin_handle g_handles[CLOGX_MAX_LOADED_PLUGINS];
static int g_handle_count = 0;
static clog_mutex_t g_plugin_mutex = CLOG_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                  */
/* ------------------------------------------------------------------ */

/** @brief Find a cached handle by its .so path, or return -1. */
static int find_slot_locked(const char *so_path) {
    for (int i = 0; i < CLOGX_MAX_LOADED_PLUGINS; i++) {
        if (g_handles[i].used && strcmp(g_handles[i].so_path, so_path) == 0) {
            return i;
        }
    }
    return -1;
}

/** @brief Find a free slot, or return -1. */
static int alloc_slot_locked(void) {
    for (int i = 0; i < CLOGX_MAX_LOADED_PLUGINS; i++) {
        if (!g_handles[i].used) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

clogx_plugin_handle_t *log_plugin_load(const char *so_path) {
    if (!so_path || !*so_path) {
        fprintf(stderr, "[clogx] log_plugin_load: NULL or empty path\n");
        return NULL;
    }

    clog_mutex_lock(&g_plugin_mutex);

    /* Already loaded?  Return existing handle. */
    int slot = find_slot_locked(so_path);
    if (slot >= 0) {
        clog_mutex_unlock(&g_plugin_mutex);
        return &g_handles[slot];
    }

    /* Allocate a new slot. */
    slot = alloc_slot_locked();
    if (slot < 0) {
        fprintf(stderr, "[clogx] log_plugin_load: too many loaded plugins (max %d)\n",
                CLOGX_MAX_LOADED_PLUGINS);
        clog_mutex_unlock(&g_plugin_mutex);
        return NULL;
    }

    struct clogx_plugin_handle *h = &g_handles[slot];
    memset(h, 0, sizeof(*h));

    /* dlopen with RTLD_NOW | RTLD_LOCAL — resolve all symbols immediately,
     * don't leak symbols into the global namespace. */
    h->dl_handle = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
    if (!h->dl_handle) {
        fprintf(stderr, "[clogx] dlopen(%s) failed: %s\n", so_path, dlerror());
        clog_mutex_unlock(&g_plugin_mutex);
        return NULL;
    }

    /* Resolve the descriptor symbol. */
    clogx_plugin_desc_fn desc_fn = (clogx_plugin_desc_fn)dlsym(h->dl_handle, CLOGX_PLUGIN_DESC_SYM);
    if (!desc_fn) {
        fprintf(stderr, "[clogx] dlsym(%s, %s) failed: %s\n", so_path, CLOGX_PLUGIN_DESC_SYM,
                dlerror());
        dlclose(h->dl_handle);
        memset(h, 0, sizeof(*h));
        clog_mutex_unlock(&g_plugin_mutex);
        return NULL;
    }

    h->desc = desc_fn();

    /* ABI version check. */
    if (!h->desc || h->desc->abi_version != CLOGX_PLUGIN_ABI_VERSION) {
        fprintf(stderr, "[clogx] plugin %s ABI version mismatch: expected %u, got %u\n", so_path,
                CLOGX_PLUGIN_ABI_VERSION, h->desc ? (unsigned)h->desc->abi_version : 0U);
        dlclose(h->dl_handle);
        memset(h, 0, sizeof(*h));
        clog_mutex_unlock(&g_plugin_mutex);
        return NULL;
    }

    /* Resolve the factory symbol.  (Optional — a plugin might be
     * descriptor-only for capability discovery.) */
    h->create_fn = (clogx_plugin_create_fn)dlsym(h->dl_handle, CLOGX_PLUGIN_CREATE_SYM);
    if (!h->create_fn) {
        /* Not an error — some plugins may only export desc for
         * capability advertisement without sink creation. */
        h->create_fn = NULL;
    }

    /* Stash the path so we can deduplicate. */
    size_t path_len = strlen(so_path);
    if (path_len >= sizeof(h->so_path))
        path_len = sizeof(h->so_path) - 1;
    memcpy(h->so_path, so_path, path_len);
    h->so_path[path_len] = '\0';
    h->used = 1;
    g_handle_count++;

    clog_mutex_unlock(&g_plugin_mutex);

    fprintf(stderr, "[clogx] plugin loaded: %s (v%u) — %s\n", h->desc->name ? h->desc->name : "?",
            (unsigned)h->desc->plugin_version, h->desc->description ? h->desc->description : "");
    return h;
}

void log_plugin_unload(clogx_plugin_handle_t *h) {
    if (!h)
        return;

    clog_mutex_lock(&g_plugin_mutex);

    if (!h->used) {
        clog_mutex_unlock(&g_plugin_mutex);
        return;
    }

    if (h->dl_handle) {
        dlclose(h->dl_handle);
    }
    memset(h, 0, sizeof(*h));
    g_handle_count--;

    clog_mutex_unlock(&g_plugin_mutex);
}

log_sink_t *log_plugin_create_sink(clogx_plugin_handle_t *h, const char *params_json) {
    if (!h || !h->used || !h->create_fn) {
        fprintf(stderr, "[clogx] log_plugin_create_sink: invalid handle or no factory\n");
        return NULL;
    }
    return h->create_fn(params_json);
}

const clogx_plugin_t *log_plugin_info(clogx_plugin_handle_t *h) {
    if (!h || !h->used || !h->desc)
        return NULL;
    return h->desc;
}

int log_plugin_scan(const char *dir, clogx_plugin_handle_t **out, int max) {
    if (!dir) {
        return -1;
    }

    DIR *dp = opendir(dir);
    if (!dp) {
        /* ENOENT is expected — the directory simply doesn't exist. */
        return 0;
    }

    /* Collect candidate names first. */
    char names[CLOGX_MAX_LOADED_PLUGINS][CLOG_MAX_PATH_SIZE];
    int n_found = 0;
    const struct dirent *entry;

    while ((entry = readdir(dp)) != NULL && n_found < CLOGX_MAX_LOADED_PLUGINS) {
        const char *name = entry->d_name;
        size_t len = strlen(name);

        /* Skip ., .., and non-.so files. */
        if (len <= 3 || name[0] == '.')
            continue;
        if (len >= 4 && strcmp(name + len - 3, ".so") == 0) {
            if (len >= sizeof(names[0]))
                continue;
            memcpy(names[n_found], name, len + 1);
            n_found++;
        }
    }
    closedir(dp);

    /* Build full paths and try to load each one. */
    int loaded = 0;
    for (int i = 0; i < n_found; i++) {
        char full_path[CLOG_MAX_PATH_SIZE + 256];
        int ret = snprintf(full_path, sizeof(full_path), "%s/%s", dir, names[i]);
        if (ret < 0 || (size_t)ret >= sizeof(full_path))
            continue;

        clogx_plugin_handle_t *h = log_plugin_load(full_path);
        if (h) {
            if (out && loaded < max) {
                out[loaded] = h;
            }
            loaded++;
        }
    }

    return loaded;
}

/* ------------------------------------------------------------------ */
/*  Internal: lifecycle management (called from log.c)                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Load and create plugin sinks from config entries.
 *
 * Called during @ref log_dispatcher_init and @ref log_dispatcher_build_snapshot.
 * Iterates the plugin config array, loads each .so (if not already cached),
 * and creates a sink via the factory.
 *
 * @param[in]  cfg        Current configuration.
 * @param[out] out_sinks  Array receiving the created sinks.
 * @param[in]  max_sinks  Capacity of @p out_sinks.
 * @return Number of sinks created, or -1 on error.
 */
int log_plugin_create_sinks_from_config(const log_config_t *cfg, log_sink_t **out_sinks,
                                        int max_sinks) {
    if (!cfg || !out_sinks || max_sinks <= 0)
        return 0;

    int created = 0;

    for (int i = 0; i < cfg->plugin_count && created < max_sinks; i++) {
        const char *so_path = cfg->plugin_so_paths[i];
        const char *params = cfg->plugin_params_json[i];

        if (!so_path || !*so_path)
            continue;

        clogx_plugin_handle_t *h = log_plugin_load(so_path);
        if (!h) {
            fprintf(stderr, "[clogx] failed to load plugin: %s\n", so_path);
            continue;
        }

        log_sink_t *sink = log_plugin_create_sink(h, params);
        if (!sink) {
            fprintf(stderr, "[clogx] plugin %s create_sink returned NULL\n",
                    log_plugin_info(h)->name ? log_plugin_info(h)->name : so_path);
            continue;
        }

        out_sinks[created++] = sink;
    }

    return created;
}

/**
 * @brief Unload all currently cached plugin handles.
 *
 * Called from @ref log_destroy.  All sinks MUST have been destroyed first.
 * This function is idempotent.
 */
void log_plugin_shutdown_all(void) {
    clog_mutex_lock(&g_plugin_mutex);

    for (int i = 0; i < CLOGX_MAX_LOADED_PLUGINS; i++) {
        if (g_handles[i].used) {
            if (g_handles[i].dl_handle) {
                dlclose(g_handles[i].dl_handle);
            }
            memset(&g_handles[i], 0, sizeof(g_handles[i]));
        }
    }
    g_handle_count = 0;

    clog_mutex_unlock(&g_plugin_mutex);
}
#endif /* _WIN32 */
