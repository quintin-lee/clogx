# Design Spec: Token Bucket Rate Limiting (Throttling)

**Date**: 2026-07-28  
**Topic**: Built-in Token Bucket Rate Limiter with Suppressed Log Count Reporting  
**Status**: Approved  

---

## 1. Overview

This design adds global log rate limiting (throttling) to `clogx` using a token bucket algorithm. When log volume exceeds the configured rate (`rate_limit_max_per_sec`) or burst limit (`rate_limit_burst`), excess log records are dropped, and a count of suppressed messages is maintained. When logs resume, `clogx` automatically logs a warning message indicating how many log events were suppressed.

---

## 2. Configuration Extensions

### 2.1 Struct `log_config_t` (`include/log_config.h`)

Add rate limiting fields:

```c
typedef struct {
    ...
    bool rate_limit_enable;     /**< Enable rate limiting when true. */
    int rate_limit_max_per_sec; /**< Max log events per second. */
    int rate_limit_burst;       /**< Maximum burst capacity. */
} log_config_t;
```

### 2.2 YAML Parsing (`core/config.c`)

Parse keys:
- `rate_limit_enable` (bool)
- `rate_limit_max_per_sec` (int)
- `rate_limit_burst` (int)

---

## 3. Rate Limiter Component (`core/rate_limit.c` & `include/log_rate_limit.h`)

### 3.1 Interface (`include/log_rate_limit.h`)

```c
#ifndef LOG_RATE_LIMIT_H
#define LOG_RATE_LIMIT_H

#include <stdbool.h>
#include <stdint.h>

void log_rate_limit_init(bool enable, int max_per_sec, int burst);
bool log_rate_limit_allow(uint64_t *out_suppressed_count);
void log_rate_limit_reset(void);

#endif
```

### 3.2 Token Bucket Algorithm (`core/rate_limit.c`)

- Maintains:
  - `g_rate_limit_enable`: boolean
  - `g_tokens`: current token balance (`double`)
  - `g_max_tokens`: burst threshold (`double`)
  - `g_fill_rate`: tokens replenished per microsecond (`double`)
  - `g_last_update_us`: timestamp of last token refill (`uint64_t`)
  - `g_suppressed_count`: cumulative count of dropped logs (`uint64_t`)
  - `g_rate_limit_mutex`: mutex protecting state
- `log_rate_limit_allow(out_suppressed)`:
  - Calculates `elapsed_us` since `g_last_update_us`.
  - Refills `g_tokens += elapsed_us * g_fill_rate` up to `g_max_tokens`.
  - If `g_tokens >= 1.0`, decrements `g_tokens -= 1.0`, checks if `g_suppressed_count > 0`, copies count to `*out_suppressed`, resets counter to 0, and returns `true`.
  - If `g_tokens < 1.0`, increments `g_suppressed_count++` and returns `false`.

---

## 4. Log Pipeline Integration (`core/log.c`)

In `log_writevprintf()`:
```c
uint64_t suppressed = 0;
if (!log_rate_limit_allow(&suppressed)) {
    return;
}
if (suppressed > 0) {
    /* Emit notification warning before the current log */
}
```

---

## 5. Test & Verification Plan

1. **Unit Test (`tests/test_rate_limit.c`)**:
   - Tests initial burst allowance, subsequent drop under high load, microsecond refill, and suppressed notice output.
2. **Build Suite Verification**:
   - `make check`
   - `make test-asan`
   - `make test-ubsan`
