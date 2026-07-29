# Design Spec: Observability Metrics (log_get_stats)

**Date**: 2026-07-29  
**Topic**: Operational Statistics and Metrics API for clogx  
**Status**: Approved  

---

## 1. Overview

This feature adds a runtime metrics retrieval API `log_get_stats()` allowing applications and monitoring agents to query operational health metrics of `clogx`.

---

## 2. Component Design

### 2.1 Public API (`include/log.h`)

```c
typedef struct {
    uint64_t total_logged_count;       /**< Total log records submitted. */
    uint64_t dropped_queue_full_count; /**< Total records that hit async queue full. */
    uint64_t suppressed_rate_count;    /**< Total records suppressed by rate limiter. */
    size_t   current_queue_depth;      /**< Current pending records in async queue. */
} log_stats_t;

/**
 * @brief Retrieve current operational statistics.
 * @param[out] stats Pointer to log_stats_t struct to populate.
 */
CLOGX_API void log_get_stats(log_stats_t *stats);
```

### 2.2 Implementation (`core/log.c` & `core/rate_limit.c`)

- Atomic counters:
  - `g_total_logged_count` (incremented in `log_writevprintf`)
  - `g_dropped_queue_full_count` (incremented when `log_async_write` fails)
  - `g_suppressed_rate_count` (retrieved from `rate_limit.c` or tracked during suppression)
- `current_queue_depth`: safely inspects `g_async_logger.queue->count`.

---

## 3. Test & Verification Plan

1. **Unit Test**: `tests/test_observability_stats.c`
2. **Quality Gate**: `make check`, `make test-asan`, `make test-ubsan`
