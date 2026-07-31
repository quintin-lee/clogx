# Thread-Safety and Performance Optimizations Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate multi-threaded data races on global and logger instance statistics, implement second-level timestamp formatting caching, and provide cross-platform Win32 dynamic library abstractions in clogx.

**Architecture:** Add cross-platform atomic primitives (`clog_atomic_inc64`, `clog_atomic_get64`) and dynamic module wrappers (`clog_dlopen`, `clog_dlsym`) in `include/clog_port.h`. Refactor `core/log.c` write path to use atomic increments for `total_logged`, `dropped_queue_full`, and Prometheus level counters. Optimize `core/formatter.c` to cache formatted `YYYY-MM-DD HH:MM:SS` strings per second per thread to skip redundant `clog_localtime_r` calls.

**Tech Stack:** C99, POSIX pthreads / Win32 SRWLock, GCC/Clang `__atomic` builtins / MSVC `InterlockedIncrement64`, libyaml, Make, CMake.

---

### Task 1: Fix Data Race on Statistics Counters via Atomic Abstractions

**Files:**
- Modify: [include/clog_port.h](file:///home/quintin/Data/source/c_cpp/clogx/include/clog_port.h#L440-L450)
- Modify: [core/log.c](file:///home/quintin/Data/source/c_cpp/clogx/core/log.c#L260-L270)
- Modify: [tests/test_observability_stats.c](file:///home/quintin/Data/source/c_cpp/clogx/tests/test_observability_stats.c#L1-L80)

- [ ] **Step 1: Write concurrent multithreaded stats test in `tests/test_observability_stats.c`**

Add a multi-threaded stress test function `test_concurrent_stats_increment` to verify total logged counts match exact expected totals when 4 threads log concurrently.

```c
#include "log.h"
#include "clog_port.h"
#include <assert.h>
#include <stdio.h>

#define THREAD_COUNT 4
#define LOGS_PER_THREAD 5000

static void *stress_log_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < LOGS_PER_THREAD; i++) {
        LOG_INFO("concurrent log test %d", i);
    }
    return NULL;
}

static void test_concurrent_stats_increment(void)
{
    log_config_t cfg = {0};
    cfg.level = LOG_LEVEL_INFO;
    cfg.async = false;
    cfg.console_enable = false;
    cfg.file_enable = false;
    log_config_set(&cfg);

    log_init(NULL);

    clog_thread_t threads[THREAD_COUNT];
    for (int i = 0; i < THREAD_COUNT; i++) {
        clog_thread_create(&threads[i], stress_log_worker, NULL);
    }
    for (int i = 0; i < THREAD_COUNT; i++) {
        clog_thread_join(threads[i]);
    }

    log_stats_t stats = {0};
    log_get_stats(&stats);

    log_destroy();

    assert(stats.total_logged_count == (uint64_t)(THREAD_COUNT * LOGS_PER_THREAD));
    printf("test_concurrent_stats_increment PASSED (%llu logs)\n",
           (unsigned long long)stats.total_logged_count);
}

int main(void)
{
    test_concurrent_stats_increment();
    return 0;
}
```

- [ ] **Step 2: Run test to verify behavior**

Run: `gcc -std=c99 -Iinclude -Icore tests/test_observability_stats.c build/libclogx.a -lpthread -lyaml -o build/test_observability_stats && build/test_observability_stats`
Expected: `test_concurrent_stats_increment PASSED (20000 logs)`

- [ ] **Step 3: Add portable 64-bit atomic abstractions to `include/clog_port.h`**

Add `clog_atomic_inc64`, `clog_atomic_add64`, and `clog_atomic_get64` functions near line 445 in [include/clog_port.h](file:///home/quintin/Data/source/c_cpp/clogx/include/clog_port.h#L445).

```c
/* ── Atomic 64-bit operations ── */

static inline uint64_t clog_atomic_inc64(volatile uint64_t *ptr)
{
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_fetch_add(ptr, 1ULL, __ATOMIC_RELAXED) + 1ULL;
#elif defined(_WIN32) || defined(_WIN64)
    return (uint64_t)InterlockedIncrement64((volatile LONG64 *)ptr);
#else
    uint64_t val = ++(*ptr);
    return val;
#endif
}

static inline uint64_t clog_atomic_get64(volatile uint64_t *ptr)
{
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(ptr, __ATOMIC_RELAXED);
#elif defined(_WIN32) || defined(_WIN64)
    return (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)ptr, 0, 0);
#else
    return *ptr;
#endif
}
```

- [ ] **Step 4: Update `core/log.c` write path to use atomic operations**

In [core/log.c](file:///home/quintin/Data/source/c_cpp/clogx/core/log.c#L262-L266):

```c
    clog_atomic_inc64(&logger->total_logged);
    if ((int)level >= 0 && (int)level < 6) {
        clog_atomic_inc64(&g_prometheus_level_counts[(int)level]);
    }
```

And in [core/log.c:L337](file:///home/quintin/Data/source/c_cpp/clogx/core/log.c#L337):

```c
        if (ar != 0) {
            clog_atomic_inc64(&logger->dropped_queue_full);
            void (*cb)(void) = logger->async_fallback_cb;
            if (cb) {
                cb();
            }
        }
```

And in `log_get_stats` in [core/log.c:L371](file:///home/quintin/Data/source/c_cpp/clogx/core/log.c#L371):

```c
void log_get_stats(log_stats_t *stats)
{
    if (!stats) {
        return;
    }
    stats->total_logged_count       = clog_atomic_get64(&g_default_logger.total_logged);
    stats->dropped_queue_full_count = clog_atomic_get64(&g_default_logger.dropped_queue_full);
    stats->suppressed_rate_count    = log_rate_limit_get_total_suppressed_for(&g_default_logger);
    stats->current_queue_depth      = log_async_get_queue_depth_for(&g_default_logger);
}
```

- [ ] **Step 5: Run unit tests and ASan sanitizer to verify zero data races**

Run: `make test && make asan`
Expected: All tests pass with exit code 0.

- [ ] **Step 6: Commit changes**

```bash
git add include/clog_port.h core/log.c tests/test_observability_stats.c
git commit -m "fix(core): use atomic operations for logger statistics counters"
```

---

### Task 2: Implement Second-Granularity Cached Timestamp Formatting

**Files:**
- Modify: [core/formatter.c](file:///home/quintin/Data/source/c_cpp/clogx/core/formatter.c#L60-L70,L280-L320)
- Test: [tests/test_json_formatter.c](file:///home/quintin/Data/source/c_cpp/clogx/tests/test_json_formatter.c)

- [ ] **Step 1: Write timestamp cache benchmark / unit test in `tests/test_json_formatter.c`**

Add a test validating that rapid timestamps within the same second format identically and correctly to `YYYY-MM-DD HH:MM:SS.uuuuuu`.

```c
static void test_timestamp_cache_formatting(void)
{
    log_record_t rec = {0};
    rec.level = LOG_LEVEL_INFO;
    rec.timestamp = 1700000000000000ULL; /* fixed epoch time in microseconds */
    rec.message = "cache time test";
    rec.module = "main";
    rec.file = "test.c";
    rec.line = 10;
    rec.func = "test_fn";

    char buf1[256];
    char buf2[256];

    logger_t logger = {0};
    logger.config.level = LOG_LEVEL_DEBUG;
    snprintf(logger.config.format, sizeof(logger.config.format), "%s", "[%time] %msg");

    int len1 = log_formatter_format_for(&logger, &rec, buf1, sizeof(buf1));
    rec.timestamp += 500ULL; /* same second, +500 us */
    int len2 = log_formatter_format_for(&logger, &rec, buf2, sizeof(buf2));

    assert(len1 > 0 && len2 > 0);
    assert(strncmp(buf1, buf2, 20) == 0); /* date and second portion must match */
    printf("test_timestamp_cache_formatting PASSED\n");
}
```

- [ ] **Step 2: Run test to verify initial state**

Run: `gcc -std=c99 -Iinclude -Icore tests/test_json_formatter.c build/libclogx.a -lpthread -lyaml -o build/test_json_formatter && build/test_json_formatter`
Expected: PASS

- [ ] **Step 3: Add thread-local second-cache in `core/formatter.c`**

In [core/formatter.c](file:///home/quintin/Data/source/c_cpp/clogx/core/formatter.c):

```c
typedef struct {
    time_t sec;
    char   formatted[32]; /* "YYYY-MM-DD HH:MM:SS" */
} clog_time_cache_t;

static clog_thread_local clog_time_cache_t g_time_cache = {0, ""};

static void format_timestamp_cached(uint64_t timestamp_us, char *out, size_t out_size)
{
    time_t sec = (time_t)(timestamp_us / 1000000ULL);
    uint32_t us = (uint32_t)(timestamp_us % 1000000ULL);

    if (sec != g_time_cache.sec || g_time_cache.formatted[0] == '\0') {
        struct tm tm_buf;
        clog_localtime_r(&sec, &tm_buf);
        snprintf(g_time_cache.formatted,
                 sizeof(g_time_cache.formatted),
                 "%04d-%02d-%02d %02d:%02d:%02d",
                 tm_buf.tm_year + 1900,
                 tm_buf.tm_mon + 1,
                 tm_buf.tm_mday,
                 tm_buf.tm_hour,
                 tm_buf.tm_min,
                 tm_buf.tm_sec);
        g_time_cache.sec = sec;
    }

    snprintf(out, out_size, "%s.%06u", g_time_cache.formatted, us);
}
```

- [ ] **Step 4: Connect `format_timestamp_cached` to `%time` opcode rendering in `log_formatter_format_for`**

Replace direct `clog_localtime_r` formatting in `core/formatter.c` with `format_timestamp_cached(record->timestamp, time_str, sizeof(time_str));`.

- [ ] **Step 5: Run full test suite and verify formatting**

Run: `make test`
Expected: All 40+ tests pass with zero regressions.

- [ ] **Step 6: Commit changes**

```bash
git add core/formatter.c tests/test_json_formatter.c
git commit -m "perf(formatter): add thread-local second-granularity time formatting cache"
```

---

### Task 3: Cross-Platform Dynamic Plugin API Abstraction

**Files:**
- Modify: [include/clog_port.h](file:///home/quintin/Data/source/c_cpp/clogx/include/clog_port.h#L500-L520)
- Modify: [core/plugin_loader.c](file:///home/quintin/Data/source/c_cpp/clogx/core/plugin_loader.c#L1-L150)
- Test: [tests/test_plugin_abi.c](file:///home/quintin/Data/source/c_cpp/clogx/tests/test_plugin_abi.c)

- [ ] **Step 1: Run plugin ABI tests before changes**

Run: `make build/test_plugin_abi && build/test_plugin_abi`
Expected: `=== ALL PASS ===`

- [ ] **Step 2: Add dynamic library portability wrappers to `include/clog_port.h`**

In [include/clog_port.h](file:///home/quintin/Data/source/c_cpp/clogx/include/clog_port.h):

```c
/* ── Dynamic Library Abstractions (dlopen / LoadLibrary) ── */

#if defined(_WIN32) || defined(_WIN64)
typedef HMODULE clog_dl_handle_t;
static inline clog_dl_handle_t clog_dlopen(const char *filename) {
    return LoadLibraryA(filename);
}
static inline void *clog_dlsym(clog_dl_handle_t handle, const char *symbol) {
    return (void *)GetProcAddress(handle, symbol);
}
static inline int clog_dlclose(clog_dl_handle_t handle) {
    return FreeLibrary(handle) ? 0 : -1;
}
static inline const char *clog_dlerror(void) {
    return "Win32 LoadLibrary error";
}
#else
#include <dlfcn.h>
typedef void *clog_dl_handle_t;
static inline clog_dl_handle_t clog_dlopen(const char *filename) {
    return dlopen(filename, RTLD_NOW | RTLD_LOCAL);
}
static inline void *clog_dlsym(clog_dl_handle_t handle, const char *symbol) {
    return dlsym(handle, symbol);
}
static inline int clog_dlclose(clog_dl_handle_t handle) {
    return dlclose(handle);
}
static inline const char *clog_dlerror(void) {
    return dlerror();
}
#endif
```

- [ ] **Step 3: Update `core/plugin_loader.c` to use `clog_dl*` abstractions**

In [core/plugin_loader.c](file:///home/quintin/Data/source/c_cpp/clogx/core/plugin_loader.c), replace `dlopen`, `dlsym`, `dlclose`, `dlerror` with `clog_dlopen`, `clog_dlsym`, `clog_dlclose`, `clog_dlerror`.

- [ ] **Step 4: Run plugin ABI tests and check-tidy**

Run: `make test_plugin_abi && make check-tidy`
Expected: `PASS` and zero static analysis warnings.

- [ ] **Step 5: Commit changes**

```bash
git add include/clog_port.h core/plugin_loader.c
git commit -m "refactor(plugin): wrap dynamic library loading behind clog_dl abstraction"
```
