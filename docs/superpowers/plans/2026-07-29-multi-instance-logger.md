# Multi-Instance Logger Implementation Plan

> **For agentic workers:** REQUIRED: Use subagent-driven-development (if subagents available) or executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Introduce `logger_t *` instance API while keeping 100% backward compatibility with existing `LOG_INFO()` / `log_init()` global API.

**Architecture:** `logger_t` is an opaque struct owning all subsystem state (config, dispatcher, async worker, rate limiter, formatter, module, stats). Existing `log_*` functions delegate to a static `g_default_logger`. New `LOGGER_INFO(logger, ...)` macros target specific instances. Internal modules get `*_for(logger_t *)` variants.

**Tech Stack:** C99, libyaml, pthreads (or platform port layer)

**Spec**: `docs/multi-instance-logger-design.md`

---

## Chunk 1: Phase 1 — Internal Refactoring (no public API change)

### Task 1.1: Define `logger_t` in log.c

**Files:**
- Modify: `core/log.c`

- [ ] **Step 1: Add `logger_t` struct definition and `g_default_logger`**

Add to `core/log.c` after the includes:

```c
// CLOG_MAX_FORMAT_SIZE is already defined in log_limits.h (value 1024)

typedef struct logger_t {
    /* ── Configuration ── */
    log_config_t    config;
    char            config_path[512];      /* YAML path for reload */
    clog_rwlock_t   config_rwlock;         /* rwlock for config reads/writes */

    /* ── Sink dispatcher ── */
    log_sink_t    **sinks;
    int             sink_count;
    clog_mutex_t    dispatcher_mutex;

    /* ── Async worker (optional) ── */
    mpsc_queue_t   *queue;
    clog_thread_t   worker_thread;
    volatile int    async_running;
    volatile int    async_processing;

    /* ── Rate limiter (optional) ── */
    bool            rl_enabled;
    double          rl_tokens;
    double          rl_max_tokens;
    double          rl_fill_rate;
    uint64_t        rl_last_update_ms;
    uint64_t        rl_suppressed_count;
    uint64_t        rl_total_suppressed;
    clog_mutex_t    rl_mutex;

    /* ── Formatter ── */
    char            format_str[CLOG_MAX_FORMAT_SIZE];
    char            time_format_str[64];
    clog_mutex_t    fmt_mutex;

    /* ── Module name ── */
    char            module[64];
    clog_mutex_t    module_mutex;

    /* ── Runtime statistics ── */
    uint64_t        total_logged;
    uint64_t        dropped_queue_full;

    /* ── Lifecycle ── */
    bool            initialized;

    /* ── Callbacks ── */
    void          (*async_fallback_cb)(void);
} logger_t;

/* NOT static — needed as extern in log_internal.h for other TUs */
logger_t g_default_logger = {0};
```

- [ ] **Step 2: Verify `CLOG_MAX_FORMAT_SIZE` in log_limits.h**

Run: `grep -n "CLOG_MAX_FORMAT_SIZE" include/log_limits.h`
Expected: `#define CLOG_MAX_FORMAT_SIZE 1024` (value already exists, use it — do NOT redefine).

- [ ] **Step 3: Identify old global variables (keep them for now)**

The old statics (`g_initialized`, `g_async_fallback_cb`, `g_module`, `g_total_logged_count`, `g_dropped_queue_full_count`) are still referenced by functions not yet rewritten. **Do NOT remove them yet.** They will be removed in Task 1.8 after all references are updated.

Add `g_init_mutex` remains — it still protects the global init gate.

- [ ] **Step 4: Verify compilation**

Run: `make -j$(nproc)` from `build/` or project root.
Expected: compiles clean (logger_t and g_default_logger added, all existing code still references old globals).

---

### Task 1.1b: Create `core/log_internal.h` — shared internal header

**Files:**
- Create: `core/log_internal.h`
- Modify: `core/log.c`

All internal modules (dispatcher, formatter, async, config, rate_limit) need access to the `logger_t` struct to implement `*_for(logger_t *)` functions. Create the shared header now, before any module refactoring tasks.

- [ ] **Step 1: Create `core/log_internal.h`**

```c
// core/log_internal.h
#ifndef LOG_INTERNAL_H
#define LOG_INTERNAL_H

#include "clog_port.h"
#include "clogx_errno.h"
#include "log_config.h"
#include "log_limits.h"

// ── Inline helper to guard a code block with a mutex ──
// (Already defined in clog_port.h — CLOG_MUTEXGUARDED exists.)

// ── Full logger_t struct definition ──
// (Opaque to public API — callers only see forward declaration)
typedef struct logger_t {
    /* ── Configuration ── */
    log_config_t    config;
    char            config_path[512];       /* YAML path for reload */
    clog_rwlock_t   config_rwlock;          /* rwlock for config reads/writes */

    /* ── Sink dispatcher ── */
    log_sink_t    **sinks;
    int             sink_count;
    clog_mutex_t    dispatcher_mutex;

    /* ── Async worker (optional) ── */
    mpsc_queue_t   *queue;
    clog_thread_t   worker_thread;
    volatile int    async_running;
    volatile int    async_processing;

    /* ── Rate limiter (optional) ── */
    bool            rl_enabled;
    double          rl_tokens;
    double          rl_max_tokens;
    double          rl_fill_rate;
    uint64_t        rl_last_update_ms;
    uint64_t        rl_suppressed_count;
    uint64_t        rl_total_suppressed;
    clog_mutex_t    rl_mutex;

    /* ── Formatter ── */
    char            format_str[CLOG_MAX_FORMAT_SIZE];
    char            time_format_str[64];
    clog_mutex_t    fmt_mutex;

    /* ── Module name ── */
    char            module[64];
    clog_mutex_t    module_mutex;

    /* ── Runtime statistics ── */
    uint64_t        total_logged;
    uint64_t        dropped_queue_full;

    /* ── Lifecycle ── */
    bool            initialized;

    /* ── Callbacks ── */
    void          (*async_fallback_cb)(void);
} logger_t;

extern logger_t g_default_logger;

#endif /* LOG_INTERNAL_H */
```

- [ ] **Step 2: Update `core/log.c` to include `log_internal.h` instead of defining `logger_t` locally**

Replace the inline `typedef struct logger_t { ... } logger_t;` definition added in Task 1.1 with:
```c
#include "log_internal.h"
```

Keep the definition of `g_default_logger`:
```c
logger_t g_default_logger = {0};
```

- [ ] **Step 3: Verify compilation**

Run: `make -j$(nproc)`
Expected: compiles clean. `logger_t` type now available to all modules via `log_internal.h`.

---

### Task 1.2: Refactor `g_module` → `logger_t.module`

**Files:**
- Modify: `core/log.c`

This is the simplest refactoring — inline `g_module_mutex` + `g_module[]` into `logger_t`.

- [ ] **Step 1: Rewrite `log_set_module` to operate on `logger_t *logger`**

Add new internal function:
```c
static void logger_set_module_internal(logger_t *logger, const char *module) {
    clog_mutex_lock(&logger->module_mutex);
    if (!module || !*module) {
        snprintf(logger->module, sizeof(logger->module), "%s", "main");
    } else {
        snprintf(logger->module, sizeof(logger->module), "%s", module);
    }
    clog_mutex_unlock(&logger->module_mutex);
}
```

Rewrite `log_set_module`:
```c
void log_set_module(const char *module) {
    logger_set_module_internal(&g_default_logger, module);
}
```

- [ ] **Step 2: Rewrite `log_get_module`**

Rewrite to read from `&g_default_logger`:
```c
void log_get_module(char *buf, size_t n) {
    if (!buf || n == 0)
        return;
    clog_mutex_lock(&g_default_logger.module_mutex);
    snprintf(buf, n, "%s", g_default_logger.module);
    clog_mutex_unlock(&g_default_logger.module_mutex);
}
```

- [ ] **Step 3: Verify compilation**

Run: `make -j$(nproc)`
Expected: compiles clean.

---

### Task 1.3: Refactor dispatcher.c to accept `logger_t *`

**Files:**
- Modify: `core/dispatcher.c`
- Modify: `include/dispatcher.h`

This is the largest refactoring task. The dispatcher currently uses `g_dispatcher` (sinks + mutex). We move the state into `logger_t` and add `*_for(logger_t *)` variants.

**Requires:** `log_internal.h` (created in Task 1.1b) — add `#include "log_internal.h"` at the top of `dispatcher.c` for access to `logger_t` struct fields and `g_default_logger` extern.

- [ ] **Step 1: Add `log_dispatcher_dispatch_for(logger_t *, record)` to dispatcher.c**

The function body is identical to `log_dispatcher_dispatch()` but uses `logger->sinks`, `logger->sink_count`, `logger->dispatcher_mutex` instead of `g_dispatcher.*`.

Key changes in the function:
- `log_get_level()` → read from `logger->config.level`
- `log_formatter_format(record, ...)` → `log_formatter_format_for(logger, record, ...)` (will be added in Task 1.4)
- `log_config_color_enabled()` → read from `logger->config.color`
- `g_dispatcher.mutex` → `logger->dispatcher_mutex`
- `g_dispatcher.sinks` → `logger->sinks`
- `g_dispatcher.sink_count` → `logger->sink_count`

Leave `log_dispatcher_dispatch()` as a wrapper:
```c
int log_dispatcher_dispatch(log_record_t *restrict record) {
    return log_dispatcher_dispatch_for(&g_default_logger, record);
}
```

- [ ] **Step 2: Add `log_dispatcher_add_sink_for(logger_t *, sink)`**

```c
int log_dispatcher_add_sink_for(logger_t *logger, log_sink_t *restrict sink) {
    if (!sink)
        return -1;
    int ret = 0;
    CLOG_MUTEXGUARDED(&logger->dispatcher_mutex, {
        log_sink_t **new_sinks = (log_sink_t **)realloc(
            (void *)logger->sinks,
            ((size_t)logger->sink_count + 1) * sizeof(log_sink_t *));
        if (!new_sinks) {
            ret = -1;
        } else {
            logger->sinks = new_sinks;
            logger->sinks[logger->sink_count] = sink;
            logger->sink_count++;
        }
    });
    return ret;
}
```

Make `log_dispatcher_add_sink()` a wrapper.

- [ ] **Step 3: Add `log_dispatcher_remove_sink_for(logger_t *, sink)`**

Same pattern — copy `log_dispatcher_remove_sink()`, replace `g_dispatcher` with `logger->`.

- [ ] **Step 4: Add `log_dispatcher_destroy_for(logger_t *)`**

Same pattern — copy `log_dispatcher_destroy()`, replace `g_dispatcher` with `logger->`.

- [ ] **Step 5: Add `log_dispatcher_flush_for(logger_t *)`**

Same pattern.

- [ ] **Step 6: Add `log_dispatcher_init_for(logger_t *)`**

This is the crucial one — creates sinks from config. Copy `log_dispatcher_init()`, but:
- `log_config_get()` → read from `logger->config`
- Instead of `log_dispatcher_destroy()`, call `log_dispatcher_destroy_for(logger)`

- [ ] **Step 7: Add `log_dispatcher_build_snapshot_for(logger_t *, cfg, snap)`**

Copy `log_dispatcher_build_snapshot()` — it doesn't use globals except through `log_config_get()`, which we replace by reading from `cfg` parameter directly. This one is already almost instance-ready.

- [ ] **Step 8: Add `log_dispatcher_commit_snapshot_for(logger_t *, snap)`**

Copy `log_dispatcher_commit_snapshot()`, replace `g_dispatcher` with `logger->`.

- [ ] **Step 9: Add `log_dispatcher_atfork_prepare/atfork_parent/atfork_child_for(logger_t *)`**

Copy each, replace `g_dispatcher` with `logger->`.

- [ ] **Step 10: Update `dispatcher.h` with new declarations**

Add to `include/dispatcher.h`:
```c
// Forward declaration
typedef struct logger_t logger_t;

// Instance variants
int log_dispatcher_dispatch_for(logger_t *logger, log_record_t *restrict record);
int log_dispatcher_add_sink_for(logger_t *logger, log_sink_t *restrict sink);
int log_dispatcher_remove_sink_for(logger_t *logger, log_sink_t *restrict sink);
void log_dispatcher_destroy_for(logger_t *logger);
void log_dispatcher_flush_for(logger_t *logger);
int log_dispatcher_init_for(logger_t *logger);
int log_dispatcher_build_snapshot_for(logger_t *logger, log_config_t *restrict cfg, log_dispatcher_snapshot_t *restrict snap);
void log_dispatcher_commit_snapshot_for(logger_t *logger, log_dispatcher_snapshot_t *restrict snap);
void log_dispatcher_atfork_prepare_for(logger_t *logger);
void log_dispatcher_atfork_parent_for(logger_t *logger);
void log_dispatcher_atfork_child_for(logger_t *logger);
```

- [ ] **Step 11: Update `console_sink_is_color_enabled` reference**

In `log_dispatcher_dispatch`, the function `console_sink_is_color_enabled(sink)` is called. This function is in the sinks module — check if it's also global-only. If it checks sink internals, it's fine. No change needed.

- [ ] **Step 12: Verify compilation**

Run: `make -j$(nproc)`

Note: `log_dispatcher_dispatch_for` calls `log_formatter_format_for(logger, ...)` which won't exist until Task 1.4. If compilation fails on that symbol, continue to Task 1.4, then re-verify here. The full compilation check is after Task 1.4.

---

### Task 1.4: Refactor formatter.c to accept `logger_t *`

**Files:**
- Modify: `core/formatter.c`
- Modify: `include/log_formatter.h`

**Requires:** `log_internal.h` — add `#include "log_internal.h"` at the top of `formatter.c`.

- [ ] **Step 1: Add `log_formatter_init_for(logger_t *, format, time_format)`**

Copy `log_formatter_init()`, replace `g_format_mutex` / `g_format_ptr` / `g_format_buf` / `g_default_format` / `g_time_format_buf` with `logger->fmt_mutex` / `logger->format_str` / `logger->time_format_str`.

```c
static char g_default_format[CLOG_MAX_FORMAT_SIZE] = "%msg";

int log_formatter_init_for(logger_t *logger, const char *format, const char *time_format) {
    clog_mutex_lock(&logger->fmt_mutex);
    if (format && strlen(format) > 0) {
        snprintf(logger->format_str, sizeof(logger->format_str), "%s", format);
    } else {
        snprintf(logger->format_str, sizeof(logger->format_str), "%s", g_default_format);
    }
    if (time_format && strlen(time_format) > 0) {
        snprintf(logger->time_format_str, sizeof(logger->time_format_str), "%s", time_format);
    } else {
        snprintf(logger->time_format_str, sizeof(logger->time_format_str), "%s", "%Y-%m-%d %H:%M:%S");
    }
    clog_mutex_unlock(&logger->fmt_mutex);
    return 0;
}
```

- [ ] **Step 2: Add `log_formatter_format_for(logger_t *, record, buf, size)`**

This is the key one. Copy `log_formatter_format()`, replace:
- `g_format_mutex` → `logger->fmt_mutex`
- `g_format_ptr` / `logger->format_str` — read format string from `logger->format_str`
- `g_time_format_buf` → `logger->time_format_str`

The `format_json` function also reads `g_time_format_buf` — it needs to receive `logger` as well. Either pass `logger` through, or pass `time_format_str` directly.

Simplest approach: have `format_json` accept a `const char *time_format` parameter.

- [ ] **Step 3: Add `log_formatter_reset_for(logger_t *)`**

Reset `logger->format_str` to `"%msg"`.

- [ ] **Step 4: Add `log_formatter_get_format_for(logger_t *)`**

Return `logger->format_str` (under mutex).

- [ ] **Step 5: Update `log_formatter.h` with new declarations**

```c
int log_formatter_init_for(logger_t *logger, const char *format, const char *time_format);
int log_formatter_format_for(logger_t *logger, log_record_t *restrict record, char *restrict buf, size_t buf_size);
void log_formatter_reset_for(logger_t *logger);
const char *log_formatter_get_format_for(logger_t *logger);
```

- [ ] **Step 6: Verify compilation**

Run: `make -j$(nproc)`
Expected: compiles clean.

---

### Task 1.5: Refactor rate_limit.c to accept `logger_t *`

**Files:**
- Modify: `core/rate_limit.c`
- Modify: `include/log_rate_limit.h`

**Requires:** `log_internal.h` — add `#include "log_internal.h"` at the top of `rate_limit.c`.

- [ ] **Step 1: Add `log_rate_limit_init_for(logger_t *, ...)`**

Copy `log_rate_limit_init()`, replace globals with `logger->rl_*` fields.

- [ ] **Step 2: Add `log_rate_limit_allow_for(logger_t *, out_suppressed_count)`**

Copy `log_rate_limit_allow()`, replace globals with `logger->rl_*` fields.

- [ ] **Step 3: Add `log_rate_limit_reset_for(logger_t *)`**

- [ ] **Step 4: Add `log_rate_limit_get_total_suppressed_for(logger_t *)`**

- [ ] **Step 5: Update `log_rate_limit.h`**

```c
void log_rate_limit_init_for(logger_t *logger, bool enable, int max_per_sec, int burst);
bool log_rate_limit_allow_for(logger_t *logger, uint64_t *out_suppressed_count);
void log_rate_limit_reset_for(logger_t *logger);
uint64_t log_rate_limit_get_total_suppressed_for(logger_t *logger);
```

- [ ] **Step 6: Verify compilation**

Run: `make -j$(nproc)`

---

### Task 1.6: Refactor async.c to accept `logger_t *`

**Files:**
- Modify: `core/async.c`
- Modify: `include/log_async.h`

**Requires:** `log_internal.h` — add `#include "log_internal.h"` at the top of `async.c`.

- [ ] **Step 1: Add `log_async_init_for(logger_t *, queue_size)`**

Copy `log_async_init()`, replace `g_async_logger` fields with `logger->queue`, `logger->worker_thread`, `logger->async_running`, `logger->async_processing`.

In `async_worker`, it receives a `void *arg` that is `async_logger_t *` — we change it to receive `logger_t *` and read `logger->queue` directly.

Actually, we have two options:
- Option A: Keep `async_logger_t` as an inner struct and embed it in `logger_t`
- Option B: The worker thread arg points to `logger_t`, and the worker reads `logger->queue` / `logger->sinks` etc.

Option B is simpler and avoids extra indirection. Let's do Option B.

```c
static void *async_worker(void *arg) {
    logger_t *logger = (logger_t *)arg;
    log_record_t batch[ASYNC_BATCH_SIZE];
    while (1) {
        int count = mpsc_queue_get_batch(logger->queue, batch, ASYNC_BATCH_SIZE);
        if (count <= 0)
            break;
        logger->async_processing = 1;
        for (int i = 0; i < count; i++) {
            log_dispatcher_dispatch_for(logger, &batch[i]);
            log_record_free_owned(&batch[i]);
        }
        log_dispatcher_flush_for(logger);
        logger->async_processing = 0;
    }
    return NULL;
}
```

- [ ] **Step 2: Add `log_async_shutdown_for(logger_t *)`**

- [ ] **Step 3: Add `log_async_flush_for(logger_t *)`**

Use `logger->queue` instead of `g_async_logger.queue`.

- [ ] **Step 4: Add `log_async_is_running_for(logger_t *)`**

- [ ] **Step 5: Add `log_async_write_for(logger_t *, record)`**

This enqueues to `logger->queue`, dispatches via `log_dispatcher_dispatch_for(logger, record)`.

- [ ] **Step 6: Add `log_async_get_queue_depth_for(logger_t *)`**

- [ ] **Step 7: Add `log_async_atfork_child_for(logger_t *)`**

- [ ] **Step 8: Update `log_async.h`**

```c
int log_async_init_for(logger_t *logger, int queue_size);
void log_async_shutdown_for(logger_t *logger);
void log_async_flush_for(logger_t *logger);
int log_async_is_running_for(logger_t *logger);
int log_async_write_for(logger_t *logger, log_record_t *restrict record);
size_t log_async_get_queue_depth_for(logger_t *logger);
void log_async_atfork_child_for(logger_t *logger);
```

- [ ] **Step 9: Wrap old functions**

```c
int log_async_init(int queue_size) { return log_async_init_for(&g_default_logger, queue_size); }
// ... etc for all old functions
```

- [ ] **Step 10: Verify compilation**

Run: `make -j$(nproc)`

---

### Task 1.7: Refactor config.c — YAML parsing stays pure, config storage moves to logger_t

**Files:**
- Modify: `core/config.c`
- Modify: `include/log_config.h`

**Requires:** `log_internal.h` — add `#include "log_internal.h"` at the top of `config.c`.

The key insight: YAML `parse_config_file()` is already a stateless pure function that writes to a `log_config_t *cfg` parameter. The global state is in `g_config`, `g_config_path`, `g_config_format`, `g_config_time_format`.

- [ ] **Step 1: Add `log_config_load_into(logger_t *, const char *yaml_path)`**

This is a new function that parses YAML and writes the result directly into `logger->config`:

```c
int log_config_load_into(logger_t *logger, const char *yaml_path) {
    // Use local buffers as initial format/time_format storage.
    // After parse_config_file (which writes cfg->format = g_config_format),
    // we copy the final strings into logger->format_str / time_format_str.
    char local_format[512] = "";
    char local_time_format[64] = "";
    
    // Set defaults first
    logger->config.level = LOG_LEVEL_INFO;
    logger->config.async = false;
    logger->config.queue_size = 8192;
    logger->config.color = true;
    logger->config.console_enable = 1;
    logger->config.console_stderr = 0;
    logger->config.file_enable = 0;
    logger->config.file_path[0] = '\0';
    logger->config.file_max_size = (uint64_t)100 * 1024 * 1024;
    logger->config.file_backups = 10;
    logger->config.socket_enable = 0;
    logger->config.socket_host[0] = '\0';
    logger->config.socket_port = 0;
    logger->config.socket_tls = false;
    logger->config.socket_tls_ca_file[0] = '\0';
    logger->config.socket_tls_skip_verify = false;
    logger->config.rate_limit_enable = false;
    logger->config.rate_limit_max_per_sec = 0;
    logger->config.rate_limit_burst = 0;
    logger->config.catch_signals = true;
    snprintf(local_format, sizeof(local_format), "%s", "[%time] [%level] %msg");
    snprintf(local_time_format, sizeof(local_time_format), "%s", "%Y-%m-%d %H:%M:%S");
    logger->config.format = local_format;        /* point at local until parse */
    logger->config.time_format = local_time_format;
    
    // Store the config path
    if (yaml_path && strlen(yaml_path) > 0) {
        snprintf(logger->config_path, sizeof(logger->config_path), "%s", yaml_path);
    } else {
        logger->config_path[0] = '\0';
    }
    
    const char *try_path = yaml_path;
    if (!try_path || try_path[0] == '\0')
        try_path = "./config.yaml";
    
    if (clog_access(try_path, R_OK) == 0) {
        int ret = parse_config_file(try_path, &logger->config);
        if (ret != 0)
            return ret;
    }
    
    // After parse, cfg->format points at g_config_format (static global).
    // Copy the final format strings into logger-owned storage.
    const char *final_fmt = logger->config.format ? logger->config.format : local_format;
    const char *final_tf  = logger->config.time_format ? logger->config.time_format : local_time_format;
    snprintf(logger->format_str, sizeof(logger->format_str), "%s", final_fmt);
    snprintf(logger->time_format_str, sizeof(logger->time_format_str), "%s", final_tf);
    logger->config.format = logger->format_str;
    logger->config.time_format = logger->time_format_str;
    
    return 0;
}
```

**Important**: The old `parse_config_file()` writes to `cfg->format = g_config_format` (pointer to static buffer). When we pass `&logger->config`, we need `cfg->format` to point somewhere stable. The solution: after parsing, copy the format string into `logger->format_str` and point `logger->config.format` there.

- [ ] **Step 2: Keep old `log_config_init()` as a wrapper**

```c
int log_config_init(const char *yaml_path) {
    return log_config_load_into(&g_default_logger, yaml_path);
}
```

- [ ] **Step 3: Update `log_config_reload()`**

`logger_t` now has `config_path[512]` (set during `log_config_load_into`). Use it:

```c
int log_config_reload(void) {
    return log_config_load_into(&g_default_logger, g_default_logger.config_path);
}
```

- [ ] **Step 4: Update `log_set_level` / `log_get_level` to use `g_default_logger` (with rwlock)**

The `logger_t` struct includes `config_rwlock` for thread-safe config access:

```c
int log_set_level(log_level_t level) {
    clog_rwlock_wrlock(&g_default_logger.config_rwlock);
    g_default_logger.config.level = level;
    clog_rwlock_wrunlock(&g_default_logger.config_rwlock);
    return 0;
}

log_level_t log_get_level(void) {
    clog_rwlock_rdlock(&g_default_logger.config_rwlock);
    log_level_t lvl = g_default_logger.config.level;
    clog_rwlock_rdunlock(&g_default_logger.config_rwlock);
    return lvl;
}
```

- [ ] **Step 5: Update `log_config_is_async` / `log_config_color_enabled`**

These read from `g_default_logger.config`.

- [ ] **Step 6: Update `config.h`**

Add:
```c
int log_config_load_into(logger_t *logger, const char *yaml_path);
```

- [ ] **Step 7: Verify compilation**

Run: `make -j$(nproc)`

---

### Task 1.8: Rewrite `log_init()`, `log_destroy()`, `log_writevprintf()`, etc. to operate on `g_default_logger`

**Files:**
- Modify: `core/log.c`

Now that all subsystems have `*_for(logger_t *)` variants, rewrite the lifecycle functions.

- [ ] **Step 1: Add `logger_init_internal(logger_t *, yaml_path)`**

This is the core init function, extracted from `log_init()`.

> **⚠ snprintf overlap avoidance:** `log_config_load_into` already stores format/time_format into `logger->format_str`/`logger->time_format_str` (with defaults) and sets `logger->config.format = logger->format_str`. Calling `log_formatter_init_for` afterwards would do `snprintf(logger->format_str, ..., logger->format_str)` — overlapping src/dst is UB per C99 §7.19.6.5. So we skip the redundant call.

```c
static int logger_init_internal(logger_t *logger, const char *yaml_path) {
    // Initialize per-instance primitives
    clog_rwlock_init(&logger->config_rwlock);
    
    // Config (includes format/time_format init into logger->format_str)
    if (log_config_load_into(logger, yaml_path) != 0)
        return CLOG_ERR_CONFIG_OPEN;
    
    // Rate limiter
    log_rate_limit_init_for(logger, logger->config.rate_limit_enable,
                            logger->config.rate_limit_max_per_sec,
                            logger->config.rate_limit_burst);
    
    // Dispatcher (creates sinks)
    if (log_dispatcher_init_for(logger) != 0)
        return CLOG_ERR_NO_SINKS;
    
    // Async worker
    if (logger->config.async) {
        if (log_async_init_for(logger, logger->config.queue_size) != 0) {
            log_dispatcher_destroy_for(logger);
            return CLOG_ERR_THREAD_CREATE;
        }
    }
    
    // Module default
    logger_set_module_internal(logger, "main");
    
    logger->initialized = true;
    return CLOG_OK;
}
```

- [ ] **Step 2: Rewrite `log_writevprintf()` to use `g_default_logger`**

The function body stays the same, but replace:
- `g_total_logged_count++` → `g_default_logger.total_logged++`
- `log_get_level()` → `g_default_logger.config.level`
- `log_get_module()` → read directly from `g_default_logger.module`
- `log_rate_limit_allow()` → `log_rate_limit_allow_for(&g_default_logger, ...)`
- `log_config_is_async()` → `g_default_logger.config.async`
- `log_async_write()` → `log_async_write_for(&g_default_logger, ...)`
- `log_dispatcher_dispatch()` → `log_dispatcher_dispatch_for(&g_default_logger, ...)`
- `g_dropped_queue_full_count++` → `g_default_logger.dropped_queue_full++`
- `log_get_async_fallback_cb()` → `g_default_logger.async_fallback_cb`

- [ ] **Step 3: Rewrite `log_init()`**

```c
int log_init(const char *yaml_path) {
#ifndef _WIN32
    pthread_once(&g_atfork_once, register_atfork);
#endif

    clog_mutex_lock(&g_init_mutex);
    if (g_default_logger.initialized) {
        clog_mutex_unlock(&g_init_mutex);
        return CLOG_ERR_INIT_REENTRANT;
    }

    int ret = logger_init_internal(&g_default_logger, yaml_path);
    if (ret != CLOG_OK) {
        clog_mutex_unlock(&g_init_mutex);
        return ret;
    }

    if (g_default_logger.config.catch_signals) {
        log_install_signal_handlers();
    }

    clog_mutex_unlock(&g_init_mutex);
    return CLOG_OK;
}
```

- [ ] **Step 4: Rewrite `log_destroy()`**

```c
void log_destroy(void) {
    bool was_init = false;
    clog_mutex_lock(&g_init_mutex);
    if (g_default_logger.initialized) {
        g_default_logger.initialized = false;
        was_init = true;
    }
    clog_mutex_unlock(&g_init_mutex);

    if (was_init) {
        log_restore_signal_handlers();
        log_async_shutdown_for(&g_default_logger);
        log_dispatcher_destroy_for(&g_default_logger);
        log_rate_limit_reset_for(&g_default_logger);
    }
}
```

- [ ] **Step 5: Update atfork handlers for `g_default_logger`**

Replace `g_module_mutex` (being removed) with `g_default_logger.module_mutex`. Replace `log_async_atfork_child()` with `log_async_atfork_child_for(&g_default_logger)`:

```c
#ifndef _WIN32
static pthread_once_t g_atfork_once = PTHREAD_ONCE_INIT;

static void log_atfork_prepare(void) {
    clog_mutex_lock(&g_init_mutex);
    clog_mutex_lock(&g_default_logger.module_mutex);
    log_dispatcher_atfork_prepare_for(&g_default_logger);
}

static void log_atfork_parent(void) {
    log_dispatcher_atfork_parent_for(&g_default_logger);
    clog_mutex_unlock(&g_default_logger.module_mutex);
    clog_mutex_unlock(&g_init_mutex);
}

static void log_atfork_child(void) {
    log_dispatcher_atfork_child_for(&g_default_logger);
    clog_mutex_unlock(&g_default_logger.module_mutex);
    clog_mutex_unlock(&g_init_mutex);
    log_async_atfork_child_for(&g_default_logger);
}

static void register_atfork(void) {
    pthread_atfork(log_atfork_prepare, log_atfork_parent, log_atfork_child);
}
#endif
```

- [ ] **Step 6: Rewrite `log_flush()`**

```c
void log_flush(void) {
    if (g_default_logger.config.async) {
        log_async_flush_for(&g_default_logger);
    } else {
        log_dispatcher_flush_for(&g_default_logger);
    }
}
```

- [ ] **Step 7: Rewrite `log_reload()`**

```c
int log_reload(void) {
    clog_mutex_lock(&g_init_mutex);
    if (!g_default_logger.initialized) {
        clog_mutex_unlock(&g_init_mutex);
        return CLOG_ERR_RELOAD;
    }
    clog_mutex_unlock(&g_init_mutex);

    int ret = log_config_load_into(&g_default_logger, g_default_logger.config_path);
    if (ret != 0) return CLOG_ERR_CONFIG_OPEN;

    // NOTE: log_config_load_into already updated format/time_format strings.
    // Do NOT call log_formatter_init_for here — would be snprintf overlap UB.

    log_rate_limit_init_for(&g_default_logger,
                            g_default_logger.config.rate_limit_enable,
                            g_default_logger.config.rate_limit_max_per_sec,
                            g_default_logger.config.rate_limit_burst);

    log_dispatcher_snapshot_t snap = {0};
    ret = log_dispatcher_build_snapshot_for(&g_default_logger, &g_default_logger.config, &snap);
    if (ret != 0) return CLOG_ERR_NO_SINKS;

    log_async_shutdown_for(&g_default_logger);
    log_dispatcher_commit_snapshot_for(&g_default_logger, &snap);
    log_dispatcher_destroy_snapshot(&snap);

    if (g_default_logger.config.async) {
        if (log_async_init_for(&g_default_logger, g_default_logger.config.queue_size) != 0)
            return CLOG_ERR_THREAD_CREATE;
    }

    return CLOG_OK;
}
```

- [ ] **Step 8: Rewrite `log_add_sink` / `log_remove_sink`**

```c
int log_add_sink(log_sink_t *sink) {
    if (!sink) return CLOG_ERR_INVALID_ARG;
    clog_mutex_lock(&g_init_mutex);
    if (!g_default_logger.initialized) {
        clog_mutex_unlock(&g_init_mutex);
        return CLOG_ERR_RELOAD;
    }
    int ret = log_dispatcher_add_sink_for(&g_default_logger, sink);
    clog_mutex_unlock(&g_init_mutex);
    return ret == 0 ? CLOG_OK : CLOG_ERR_OOM;
}
```

- [ ] **Step 9: Rewrite `log_get_stats()`**

```c
void log_get_stats(log_stats_t *stats) {
    if (!stats) return;
    stats->total_logged_count = g_default_logger.total_logged;
    stats->dropped_queue_full_count = g_default_logger.dropped_queue_full;
    stats->suppressed_rate_count = log_rate_limit_get_total_suppressed_for(&g_default_logger);
    stats->current_queue_depth = log_async_get_queue_depth_for(&g_default_logger);
}
```

- [ ] **Step 10: Rewrite `log_set_async_fallback_cb` / `log_get_async_fallback_cb`**

```c
void log_set_async_fallback_cb(void (*cb)(void)) {
    g_default_logger.async_fallback_cb = cb;
}

void (*log_get_async_fallback_cb(void))(void) {
    return g_default_logger.async_fallback_cb;
}
```

- [ ] **Step 11: Verify all modules include `log_internal.h`**

`log_internal.h` was already created in Task 1.1b. Verify that `dispatcher.c`, `async.c`, `formatter.c`, `rate_limit.c`, and `config.c` all include it. (`log.c` includes it too after Task 1.1b.)

- [ ] **Step 12: Verify compilation**

Run: `make -j$(nproc)`
Expected: compiles clean.

- [ ] **Step 13: Remove old global variables (now unused)**

The old `static` globals from the singleton era are now unused. Remove them from `log.c`:

```c
// Remove these declarations entirely (all references updated to g_default_logger):
static int            g_initialized = 0;           // → logger->initialized
static void         (*g_async_fallback_cb)(void);  // → logger->async_fallback_cb
static char           g_module[64] = "main";       // → logger->module
static volatile uint64_t g_total_logged_count = 0;       // → logger->total_logged
static volatile uint64_t g_dropped_queue_full_count = 0; // → logger->dropped_queue_full
```

Keep `g_init_mutex` — it still protects `log_init()` / `log_destroy()` reentrancy.

- [ ] **Step 14: Re-verify compilation**

Run: `make -j$(nproc)`
Expected: compiles clean (no references to removed globals remain).

---

### Task 1.9: Run existing tests to confirm backward compatibility

- [ ] **Step 1: Build and run existing tests**

```bash
cd build && make -j$(nproc) && ctest --output-on-failure
```

Expected: all existing tests pass.

- [ ] **Step 2: If any test fails, fix and iterate**

(The old global variables were kept in place throughout Phase 1 — they're only removed after all references are updated. Tests should not reference internal globals. If a failure occurs, fix the test to use the public API.)

- [ ] **Step 3: Commit Phase 1**

```bash
git add -A
git commit -m "refactor(logger): internal refactoring to logger_t struct

All subsystem state moved from static globals into a logger_t struct.
g_default_logger replaces g_module, g_dispatcher, g_async_logger,
g_config, g_format_ptr, g_rate_mutex, etc.

Each internal module adds *_for(logger_t *) variants. Old global
API functions become wrappers passing &g_default_logger.

No public API changes. All existing tests pass."
```

---

## Chunk 2: Phase 2 — Public Instance API + Tests

### Task 2.1: Add `logger_t` typedef and new API declarations to public header

**Files:**
- Modify: `include/log.h`

- [ ] **Step 1: Move `logger_t` struct definition to `include/log.h`**

Move the full struct definition from `core/log.c` (or `core/log_internal.h`) to `include/log.h`, making it an opaque handle:

```c
/** Opaque logger instance handle. */
typedef struct logger_t logger_t;
```

Keep the full struct definition in `core/log_internal.h` for internal access.

Actually, for C99, you can't have opaque structs without the full definition available to the compiler for the callers. But we can forward-declare it and have callers only use `logger_t *`. The full struct needs to be in an internal header.

Let's keep `logger_t` forward-declared in `include/log.h` and fully defined in `core/log_internal.h`.

- [ ] **Step 2: Add `logger_*` API declarations to `include/log.h`**

```c
// Instance creation / destruction
CLOGX_API logger_t *logger_create(const char *yaml_path);
CLOGX_API logger_t *logger_create_from_config(const log_config_t *cfg);
CLOGX_API void      logger_destroy(logger_t *logger);

// Instance-level operations
CLOGX_API void       logger_writevprintf(logger_t *logger, log_level_t level, const char *file, int line, const char *func, const char *fmt, ...) CLOGX_PRINTF_FMT(6, 7);
CLOGX_API void       logger_flush(logger_t *logger);
CLOGX_API int        logger_reload(logger_t *logger);
CLOGX_API int        logger_add_sink(logger_t *logger, log_sink_t *sink);
CLOGX_API int        logger_remove_sink(logger_t *logger, log_sink_t *sink);
CLOGX_API int        logger_set_level(logger_t *logger, log_level_t level);
CLOGX_API log_level_t logger_get_level(const logger_t *logger);
CLOGX_API void       logger_set_module(logger_t *logger, const char *module);
CLOGX_API void       logger_get_module(const logger_t *logger, char *buf, size_t n);
CLOGX_API void       logger_get_stats(const logger_t *logger, log_stats_t *stats);
CLOGX_API int        logger_config_set(logger_t *logger, const log_config_t *cfg);
CLOGX_API log_config_t *logger_config_get(const logger_t *logger);
```

- [ ] **Step 3: Add `LOGGER_*` macros to `include/log.h`**

```c
#define LOGGER_TRACE(logger, ...)  logger_writevprintf((logger), LOG_LEVEL_TRACE,  LOG_FILENAME_ONLY(), __LINE__, __func__, __VA_ARGS__)
#define LOGGER_DEBUG(logger, ...)  logger_writevprintf((logger), LOG_LEVEL_DEBUG,  LOG_FILENAME_ONLY(), __LINE__, __func__, __VA_ARGS__)
#define LOGGER_INFO(logger, ...)   logger_writevprintf((logger), LOG_LEVEL_INFO,   LOG_FILENAME_ONLY(), __LINE__, __func__, __VA_ARGS__)
#define LOGGER_WARN(logger, ...)   logger_writevprintf((logger), LOG_LEVEL_WARN,   LOG_FILENAME_ONLY(), __LINE__, __func__, __VA_ARGS__)
#define LOGGER_ERROR(logger, ...)  logger_writevprintf((logger), LOG_LEVEL_ERROR,  LOG_FILENAME_ONLY(), __LINE__, __func__, __VA_ARGS__)
#define LOGGER_FATAL(logger, ...)  logger_writevprintf((logger), LOG_LEVEL_FATAL,  LOG_FILENAME_ONLY(), __LINE__, __func__, __VA_ARGS__)
```

- [ ] **Step 4: Verify compilation**

Run: `make -j$(nproc)`

---

### Task 2.2: Implement `logger_create()` / `logger_destroy()` in log.c

**Files:**
- Modify: `core/log.c`

- [ ] **Step 1: Implement `logger_create()`**

```c
logger_t *logger_create(const char *yaml_path) {
    logger_t *logger = (logger_t *)calloc(1, sizeof(logger_t));
    if (!logger)
        return NULL;

    // Initialize mutexes
    clog_mutex_init(&logger->dispatcher_mutex);
    clog_mutex_init(&logger->rl_mutex);
    clog_mutex_init(&logger->fmt_mutex);
    clog_mutex_init(&logger->module_mutex);

    if (logger_init_internal(logger, yaml_path) != CLOG_OK) {
        logger_destroy(logger);
        return NULL;
    }

    return logger;
}
```

- [ ] **Step 2: Implement `logger_create_from_config()`**

```c
logger_t *logger_create_from_config(const log_config_t *cfg) {
    if (!cfg)
        return NULL;

    logger_t *logger = (logger_t *)calloc(1, sizeof(logger_t));
    if (!logger)
        return NULL;

    // Initialize mutexes and rwlocks
    clog_mutex_init(&logger->dispatcher_mutex);
    clog_mutex_init(&logger->rl_mutex);
    clog_mutex_init(&logger->fmt_mutex);
    clog_mutex_init(&logger->module_mutex);
    clog_rwlock_init(&logger->config_rwlock);

    // Copy config
    memcpy(&logger->config, cfg, sizeof(log_config_t));

    // Copy format strings into logger-owned buffers
    if (cfg->format) {
        snprintf(logger->format_str, sizeof(logger->format_str), "%s", cfg->format);
        logger->config.format = logger->format_str;
    }
    if (cfg->time_format) {
        snprintf(logger->time_format_str, sizeof(logger->time_format_str), "%s", cfg->time_format);
        logger->config.time_format = logger->time_format_str;
    }

    // Formatter
    log_formatter_init_for(logger, cfg->format, cfg->time_format);

    // Rate limiter
    log_rate_limit_init_for(logger, cfg->rate_limit_enable,
                            cfg->rate_limit_max_per_sec, cfg->rate_limit_burst);

    // Dispatcher
    if (log_dispatcher_init_for(logger) != 0) {
        free(logger);
        return NULL;
    }

    // Async
    if (cfg->async) {
        if (log_async_init_for(logger, cfg->queue_size) != 0) {
            log_dispatcher_destroy_for(logger);
            free(logger);
            return NULL;
        }
    }

    logger_set_module_internal(logger, "main");
    logger->initialized = true;
    return logger;
}
```

- [ ] **Step 3: Implement `logger_destroy()`**

```c
void logger_destroy(logger_t *logger) {
    if (!logger)
        return;

    log_async_shutdown_for(logger);
    log_dispatcher_destroy_for(logger);
    log_rate_limit_reset_for(logger);

    // Destroy mutexes
    clog_mutex_destroy(&logger->dispatcher_mutex);
    clog_mutex_destroy(&logger->rl_mutex);
    clog_mutex_destroy(&logger->fmt_mutex);
    clog_mutex_destroy(&logger->module_mutex);

    free(logger);
}
```

- [ ] **Step 4: Implement `logger_writevprintf()`**

```c
void logger_writevprintf(logger_t *logger, log_level_t level,
                         const char *file, int line, const char *func,
                         const char *fmt, ...) {
    if (!logger || !logger->initialized)
        return;

    // Signal check (for the default logger only — instance users manage signals themselves)
    if (logger == &g_default_logger && log_get_pending_signal() != 0) {
        log_process_pending_signals();
    }

    if (level < logger->config.level)
        return;

    logger->total_logged++;

    char message[CLOG_MAX_MESSAGE_SIZE];
    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    if (ret < 0) {
        message[0] = '\0';
    } else if (ret >= (int)sizeof(message)) {
        if (sizeof(message) >= 4)
            memcpy(message + sizeof(message) - 4, "...", 4);
        else
            message[sizeof(message) - 1] = '\0';
    }

    char module_buf[64];
    // Read module under mutex
    clog_mutex_lock(&logger->module_mutex);
    snprintf(module_buf, sizeof(module_buf), "%s", logger->module);
    clog_mutex_unlock(&logger->module_mutex);

    log_record_t record;
    record.level = level;
    record.timestamp = get_timestamp();
    record.tid = get_thread_id();
    record.pid = clog_getpid();
    record.file = file;
    record.func = func;
    record.line = line;
    record.module = module_buf;
    record.tag = NULL;
    record.message = message;

    uint64_t suppressed = 0;
    if (!log_rate_limit_allow_for(logger, &suppressed))
        return;

    if (suppressed > 0) {
        char supp_msg[128];
        snprintf(supp_msg, sizeof(supp_msg),
                 "[clogx] Suppressed %llu log messages due to rate limit",
                 (unsigned long long)suppressed);
        log_record_t supp_rec = record;
        supp_rec.level = LOG_LEVEL_WARN;
        supp_rec.message = supp_msg;
        if (logger->config.async) {
            if (log_async_write_for(logger, &supp_rec) != 0) {
                if (logger->async_fallback_cb)
                    logger->async_fallback_cb();
            }
        } else {
            log_dispatcher_dispatch_for(logger, &supp_rec);
        }
    }

    if (logger->config.async) {
        int ar = log_async_write_for(logger, &record);
        if (ar != 0) {
            logger->dropped_queue_full++;
            if (logger->async_fallback_cb)
                logger->async_fallback_cb();
        }
    } else {
        log_dispatcher_dispatch_for(logger, &record);
    }
}
```

Note: We can extract the common body and have both `log_writevprintf()` and `logger_writevprintf()` call the same internal. But the simplest approach: `log_writevprintf()` delegates to `logger_writevprintf(&g_default_logger, ...)`.

- [ ] **Step 5: Implement remaining `logger_*` wrappers**

```c
void logger_flush(logger_t *logger) {
    if (!logger) return;
    if (logger->config.async)
        log_async_flush_for(logger);
    else
        log_dispatcher_flush_for(logger);
}

int logger_reload(logger_t *logger) {
    if (!logger || !logger->initialized) return CLOG_ERR_RELOAD;
    // ... similar to log_reload() but operating on logger
}

int logger_add_sink(logger_t *logger, log_sink_t *sink) {
    if (!logger || !sink) return CLOG_ERR_INVALID_ARG;
    if (!logger->initialized) return CLOG_ERR_RELOAD;
    return log_dispatcher_add_sink_for(logger, sink) == 0 ? CLOG_OK : CLOG_ERR_OOM;
}

int logger_remove_sink(logger_t *logger, log_sink_t *sink) {
    if (!logger || !sink) return CLOG_ERR_INVALID_ARG;
    return log_dispatcher_remove_sink_for(logger, sink) == 0 ? CLOG_OK : CLOG_ERR_INVALID_ARG;
}

int logger_set_level(logger_t *logger, log_level_t level) {
    if (!logger) return -1;
    logger->config.level = level;
    return 0;
}

log_level_t logger_get_level(const logger_t *logger) {
    return logger ? logger->config.level : LOG_LEVEL_INFO;
}

void logger_set_module(logger_t *logger, const char *module) {
    if (logger) logger_set_module_internal(logger, module);
}

void logger_get_module(const logger_t *logger, char *buf, size_t n) {
    if (!logger || !buf || n == 0) return;
    clog_mutex_lock(&((logger_t *)logger)->module_mutex);  // const cast — internal detail
    snprintf(buf, n, "%s", logger->module);
    clog_mutex_unlock(&((logger_t *)logger)->module_mutex);
}

void logger_get_stats(const logger_t *logger, log_stats_t *stats) {
    if (!logger || !stats) return;
    stats->total_logged_count = logger->total_logged;
    stats->dropped_queue_full_count = logger->dropped_queue_full;
    stats->suppressed_rate_count = log_rate_limit_get_total_suppressed_for((logger_t *)logger);
    stats->current_queue_depth = log_async_get_queue_depth_for((logger_t *)logger);
}

int logger_config_set(logger_t *logger, const log_config_t *cfg) {
    if (!logger || !cfg) return -1;
    memcpy(&logger->config, cfg, sizeof(log_config_t));
    if (cfg->format) {
        snprintf(logger->format_str, sizeof(logger->format_str), "%s", cfg->format);
        logger->config.format = logger->format_str;
    }
    if (cfg->time_format) {
        snprintf(logger->time_format_str, sizeof(logger->time_format_str), "%s", cfg->time_format);
        logger->config.time_format = logger->time_format_str;
    }
    return 0;
}

log_config_t *logger_config_get(const logger_t *logger) {
    return logger ? (log_config_t *)&logger->config : NULL;
}
```

- [ ] **Step 6: Simplify `log_writevprintf()` to delegate**

```c
void log_writevprintf(log_level_t level, const char *file, int line, const char *func,
                      const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    // Use a vprintf variant to avoid code duplication
    // For now, simple delegation:
    va_end(args);
    // Actually we need a va_list version. Let's extract the core logic:
    logger_writevprintf(&g_default_logger, level, file, line, func, fmt, 
                        /* ... va_args would need __VA_ARGS__ ... */);
}
```

Actually there's a subtlety — `log_writevprintf` is variadic and `logger_writevprintf` is also variadic. You can't forward variadic args directly in C. The simplest solution is to have both call the same internal function that takes a `va_list`.

Add an internal `logger_writevprintf_internal(logger_t *, level, file, line, func, fmt, args)` that takes `va_list`. Both `log_writevprintf` and `logger_writevprintf` call it.

- [ ] **Step 7: Verify compilation**

Run: `make -j$(nproc)`

---

### Task 2.3: Add multi-instance tests

**Files:**
- Create: `tests/test_multi_instance.c`

- [ ] **Step 1: Write test for create / destroy with NULL**

```c
#include "log.h"
#include <assert.h>
#include <stdlib.h>

static void test_create_null(void) {
    logger_t *logger = logger_create("nonexistent_config.yaml");
    assert(logger == NULL);  // Expected: file doesn't exist
}

static void test_create_from_config_null(void) {
    logger_t *logger = logger_create_from_config(NULL);
    assert(logger == NULL);
}

static void test_destroy_null(void) {
    logger_destroy(NULL);  // Must be a no-op (not crash)
}
```

- [ ] **Step 2: Write test for instance level isolation**

```c
static void test_level_isolation(void) {
    // Create a minimal config (zero-initialized — use log_config_default() if available)
    log_config_t cfg = {0};
    cfg.level = LOG_LEVEL_INFO;
    cfg.async = false;
    cfg.console_enable = true;
    cfg.console_stderr = true;
    cfg.color = false;

    logger_t *info_logger = logger_create_from_config(&cfg);
    assert(info_logger != NULL);
    assert(logger_get_level(info_logger) == LOG_LEVEL_INFO);

    cfg.level = LOG_LEVEL_ERROR;
    logger_t *error_logger = logger_create_from_config(&cfg);
    assert(error_logger != NULL);
    assert(logger_get_level(error_logger) == LOG_LEVEL_ERROR);

    // Verify isolation
    assert(logger_get_level(info_logger) == LOG_LEVEL_INFO);
    assert(logger_get_level(error_logger) == LOG_LEVEL_ERROR);

    logger_destroy(info_logger);
    logger_destroy(error_logger);
}
```

- [ ] **Step 3: Write test for module name isolation**

```c
static void test_module_isolation(void) {
    log_config_t cfg = {0};
    cfg.async = false;
    cfg.console_enable = false;  // No output needed
    cfg.file_enable = false;
    cfg.socket_enable = false;

    // Need at least one sink otherwise init fails...
    // Actually, we might need to create a null sink for testing.
    // Alternative: use a custom test sink.
    
    // For now, use console to stderr
    cfg.console_enable = true;
    cfg.console_stderr = true;
    cfg.color = false;

    logger_t *a = logger_create_from_config(&cfg);
    logger_t *b = logger_create_from_config(&cfg);
    assert(a && b);

    logger_set_module(a, "module_a");
    logger_set_module(b, "module_b");

    char buf_a[64], buf_b[64];
    logger_get_module(a, buf_a, sizeof(buf_a));
    logger_get_module(b, buf_b, sizeof(buf_b));
    assert(strcmp(buf_a, "module_a") == 0);
    assert(strcmp(buf_b, "module_b") == 0);

    logger_destroy(a);
    logger_destroy(b);
}
```

- [ ] **Step 4: Integrate tests into build**

Add `test_multi_instance.c` to the test build in `CMakeLists.txt` or `Makefile`.

- [ ] **Step 5: Build and run tests**

```bash
cd build && make -j$(nproc) && ctest --output-on-failure
```

Expected: all existing tests pass + new multi-instance tests pass.

- [ ] **Step 6: Commit Phase 2**

```bash
git add -A
git commit -m "feat(logger): add multi-instance logger_t API

Add logger_create() / logger_destroy() for creating independent
logger instances with their own config, sinks, async workers,
rate limiters, formatters, and module names.

Add LOGGER_INFO(logger, ...) etc. instance macros alongside the
existing LOG_INFO() global macros. The old API remains unchanged
and delegates to g_default_logger.

Add multi-instance isolation tests."
```

---

## Review Plan

After Chunk 1: dispatch plan-document-reviewer with Chunk 1 content.
After Chunk 2: dispatch plan-document-reviewer with Chunk 2 content.
