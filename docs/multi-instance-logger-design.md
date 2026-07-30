# Multi-Instance Logger Design

## Overview

This document describes the design for introducing multi-instance `logger_t` support into clogx while maintaining 100% backward compatibility with the existing global API (`LOG_INFO()`, `log_init()`, etc.).

The library currently uses a singleton global pattern — all subsystem state lives in static globals. This design adds an opaque `logger_t` type so callers can create, configure, and destroy independent logger instances, each with its own sinks, async worker, rate limiter, format string, module name, and statistics.

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Global macros vs instance macros | Both | `LOG_INFO()` wraps `g_default_logger`; `LOGGER_INFO(logger, ...)` targets a specific instance. Existing code unchanged. |
| Subsystem state ownership | `logger_t` owns all state | Cleanest lifecycle model — one pointer, one free. |
| MDC (thread context) | Global TLS | Thread property, not logger property. Consistent with log4j/logback. |
| Signal handler flush | Manual (caller registers instance) | Avoids global weak-list complexity. |
| `logger_t*` ownership | Heap-allocated by `logger_create()`, caller frees via `logger_destroy()` | Simple, explicit. `logger_destroy(NULL)` is a no-op. |
| Internal refactoring strategy | Add `*_for(logger_t *)` variants, wrap old API | Progressive — each commit compilable and test-passing. |

## Implementation Notes

### `g_default_logger` cross-module access

`g_default_logger` is `static` in `log.c`. The old `log_*` wrapper functions that currently live in other modules (e.g. `log_add_sink()` in dispatcher.c) need access to it. Two options:

- **A (recommended)**: Move all `log_*` wrapper functions to `log.c`. Each simply calls the `*_for(logger_t *)` variant with `&g_default_logger`.
- **B**: Expose `logger_t *log_get_default_logger(void)` as an internal non-static accessor.

Approach A is cleaner — it keeps the default instance reference centralized and avoids exposing `g_default_logger` beyond `log.c`.

### `logger_create(NULL)` behavior

When `yaml_path` is NULL, `logger_create(NULL)` creates an instance with default configuration (equivalent to `log_config_default()`).

### Error handling

`logger_create()` returns `NULL` on failure. Error details follow the existing convention: the library may stderr-log the reason before returning. The internal `clogx_errno_t` global is sufficient for the initial implementation — per-instance error state is a future concern.

### Thread safety

- `logger_create()` / `logger_create_from_config()` are thread-safe (the instance isn't visible to other threads yet).
- `logger_destroy()` must not be called while any thread is writing to that instance. Caller's responsibility.
- After `logger_destroy()`, all resources (sinks, async queue/thread, mutexes) are freed.

## Type Definition

```c
typedef struct logger_t {
    /* ── Configuration ── */
    log_config_t    config;

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
} logger_t;
```

## API Surface

### Instance creation / destruction

```c
logger_t *logger_create(const char *yaml_path);
logger_t *logger_create_from_config(const log_config_t *cfg);
void      logger_destroy(logger_t *logger);
```

### Instance-level operations

```c
void         logger_writevprintf(logger_t *, log_level_t, const char *file, int line, const char *func, const char *fmt, ...);
void         logger_flush(logger_t *);
int          logger_reload(logger_t *);
int          logger_add_sink(logger_t *, log_sink_t *);
int          logger_remove_sink(logger_t *, log_sink_t *);
int          logger_set_level(logger_t *, log_level_t);
log_level_t  logger_get_level(const logger_t *);
void         logger_set_module(logger_t *, const char *);
void         logger_get_module(const logger_t *, char *buf, size_t n);
void         logger_get_stats(const logger_t *, log_stats_t *);
int          logger_config_set(logger_t *, const log_config_t *);
log_config_t *logger_config_get(const logger_t *);
```

### Instance macros

```c
#define LOGGER_TRACE(logger, ...)  logger_writevprintf((logger), LOG_LEVEL_TRACE, ...
#define LOGGER_DEBUG(logger, ...)  ...
#define LOGGER_INFO(logger, ...)   ...
#define LOGGER_WARN(logger, ...)   ...
#define LOGGER_ERROR(logger, ...)  ...
#define LOGGER_FATAL(logger, ...)  ...
```

### Global default instance

```c
// internal, in log.c:
static logger_t g_default_logger;

// Existing API remains unchanged — all delegate to g_default_logger:
clogx_errno_t log_init(const char *yaml_path);    // initializes g_default_logger
void          log_destroy(void);                   // destroys g_default_logger
void          log_flush(void);                     // flushes g_default_logger
clogx_errno_t log_reload(void);                    // reloads g_default_logger
```

## Internal Refactoring

Each internal module adds a `*_for(logger_t *)` variant. The old global-state function becomes a thin wrapper that passes `&g_default_logger`.

| Module | New Entry Point | Old Wrapper |
|--------|----------------|-------------|
| dispatcher | `log_dispatcher_dispatch_for(logger_t *, record)` | `log_dispatcher_dispatch(record)` |
| async | `log_async_write_for(logger_t *, record)` | `log_async_write(record)` — current signature is `log_async_write(record, sinks, sink_count)` which maps to `logger->queue`, `logger->sinks`, `logger->sink_count` |
| formatter | `log_formatter_format_for(logger_t *, record, buf, size)` | `log_formatter_format(record, buf, size)` |
| rate_limit | `log_rate_limit_allow_for(logger_t *)` | `log_rate_limit_allow()` |

`logger_writevprintf_internal()` replaces the body of `log_writevprintf()`:

```
log_writevprintf(level, file, line, func, fmt, ...)
  └→ logger_writevprintf_internal(&g_default_logger, ...)

logger_writevprintf_internal(logger, level, file, line, func, fmt, args)
  ├─ level gate (logger->config.level threshold)
  ├─ logger->total_logged++
  ├─ vsnprintf → stack buffer
  ├─ logger_get_module(logger) → logger->module
  ├─ log_rate_limit_allow_for(logger)
  ├─ if async: log_async_write_for(logger, &record)
  └─ else:     log_dispatcher_dispatch_for(logger, &record)
```

## Migration Strategy

Two-phase approach:

### Phase 1 — Internal refactoring (no new public API)

1. Define `logger_t` in `log.c` (static, not yet in header)
2. Define `g_default_logger` in `log.c`
3. Refactor each module to accept `logger_t *` parameter
4. Old functions become wrappers
5. No changes to public headers — all existing tests pass

### Phase 2 — Public instance API

1. Move `logger_t` typedef to public header
2. Add `logger_create()` / `logger_destroy()` / `logger_writevprintf()` etc.
3. Add `LOGGER_INFO()` etc. macros
4. Add multi-instance tests

## Testing

| Test Suite | Status | Notes |
|-----------|--------|-------|
| Existing tests | Must all pass unchanged | Proves backward compatibility |
| Create/destroy | New | NULL yaml, NULL config, NULL logger_destroy |
| Instance isolation | New | Different levels, sinks, modules per instance |
| Concurrent instances | New | Thread A writes to instance A, thread B to instance B |
| Sync vs async per instance | New | Instance A sync, B async — no cross-talk |
| MDC global sharing | Existing | Thread context visible to all instances (unchanged) |
| Signal handling | Existing + B | Signal only flushes caller-registered instances |

## Files Changed

| File | Change |
|------|--------|
| `include/clog.h` | Add `logger_t` typedef, `logger_*` declarations, `LOGGER_*` macros |
| `src/log.c` | Define `logger_t`, `g_default_logger`, `logger_writevprintf_internal()`, `logger_*` API impls |
| `src/dispatcher.c` | Refactor to `log_dispatcher_dispatch_for(logger_t *, ...)` |
| `src/async.c` | Refactor to `log_async_write_for(logger_t *, ...)` |
| `src/formatter.c` | Refactor to `log_formatter_format_for(logger_t *, ...)` |
| `src/rate_limit.c` | Refactor to `log_rate_limit_allow_for(logger_t *)` |
| `src/config.c` | Keep YAML parsing pure; config data in `logger_t.config` |
| `src/signal_handler.c` | Signal processing unchanged (global); flush is caller's responsibility |
| `tests/` | New multi-instance tests added; existing tests untouched |
