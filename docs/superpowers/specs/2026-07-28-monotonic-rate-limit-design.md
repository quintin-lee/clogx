# Design Spec: Rate Limiter Monotonic Clock Migration

**Date**: 2026-07-28  
**Topic**: Migrating Token Bucket Rate Limiter from `gettimeofday` to `CLOCK_MONOTONIC`  
**Status**: Approved  

---

## 1. Overview

This design replaces wall-clock time (`gettimeofday` / `CLOCK_REALTIME`) in `core/rate_limit.c` with POSIX `CLOCK_MONOTONIC` (`clock_gettime(CLOCK_MONOTONIC, &ts)`). Monotonic time guarantees strictly increasing microsecond timestamps that are immune to NTP time jumps or manual wall-clock adjustments.

---

## 2. Implementation Design (`core/rate_limit.c`)

Replace `get_now_us()` in `core/rate_limit.c`:

```c
static uint64_t get_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
```

Remove unnecessary `#include <sys/time.h>` header.

---

## 3. Test & Verification Plan

1. **Unit Test (`tests/test_rate_limit.c`)**:
   - Run rate limiter tests to ensure token refill and rate calculation remain accurate.
2. **Build Suite Verification**:
   - `make check`
   - `make test-asan`
   - `make test-ubsan`
