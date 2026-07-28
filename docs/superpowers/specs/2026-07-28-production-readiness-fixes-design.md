# Design Spec: Production Readiness Fixes

**Date**: 2026-07-28
**Topic**: Fix remaining production readiness issues across documentation, performance, security, and fork safety
**Status**: Approved

---

## 1. Overview

This design addresses four categories of production readiness concerns identified during the project assessment:

1. **Documentation & Accuracy**: Fix incorrect README claims and document missing public APIs
2. **Performance**: Reduce mutex contention in rate limiter hot path; reduce overhead of pending signal checks
3. **Security Hardening**: Replace deprecated OpenSSL API; add TLS hostname verification; add fuzzing integration
4. **Fork Safety**: Add atfork_child callback to console sink; preserve async queue records across fork

---

## 2. Documentation & Accuracy

### 2.1 Fix README Lock-Free Claim

The README states "Lock-free MPSC queue" but `queue.c` uses `pthread_mutex` + `pthread_cond`. The header `queue.h` correctly documents this as mutex-based. The README must be corrected.

**Change**: In `README.md`, replace "Lock-free MPSC queue" with "Mutex-protected bounded MPSC queue" in the async mode section and features list.

### 2.2 Document Missing Public APIs

`log_process_pending_signals()` and `log_get_pending_signal()` are declared in `log.h` but absent from the README.

**Change**: Add a section to README.md documenting:
- `log_get_pending_signal()` — returns non-zero if a signal is pending
- `log_process_pending_signals()` — processes pending signals (flush, restore handlers, re-raise)

### 2.3 Document Rate Limiter Behavior

The README does not document that the rate limiter uses a mutex on every log call. This should be noted in the performance considerations section.

---

## 3. Performance

### 3.1 Rate Limiter Fast Path (Disabled)

When rate limiting is disabled (`g_enabled = false`), `log_rate_limit_allow()` still acquires `g_rate_mutex`. This adds unnecessary overhead to every log call.

**Approach**: Add an atomic fast-path check before the mutex acquisition:

```c
bool log_rate_limit_allow(uint64_t *out_suppressed_count) {
    if (out_suppressed_count)
        *out_suppressed_count = 0;

    if (!g_enabled)
        return true;

    pthread_mutex_lock(&g_rate_mutex);
    ...
```

Move the `g_enabled` check before the mutex lock using an atomic read. This avoids the mutex overhead when rate limiting is disabled.

**Files**: `core/rate_limit.c`

### 3.2 Reduce Pending Signal Check Frequency

`log_process_pending_signals()` is called on every `log_writevprintf()` invocation. While the check is cheap (atomic read), calling `log_flush()` from the hot path when a signal is pending can cause significant latency spikes.

**Approach**: Keep the current design (check on every call) but ensure `log_process_pending_signals()` is lightweight when no signal is pending (it already returns early if `g_signal_pending == 0`). No change needed for this item — the current implementation is acceptable.

---

## 4. Security Hardening

### 4.1 Replace Deprecated OpenSSL API

`TLS_client_method()` is deprecated in OpenSSL 1.1.0+. The replacement is `TLS_method()`.

**Change**: In `sinks/socket_sink.c`, replace:
```c
const SSL_METHOD *method = TLS_client_method();
```
with:
```c
const SSL_METHOD *method = TLS_method();
```

**File**: `sinks/socket_sink.c`

### 4.2 Add TLS Hostname Verification

When `SSL_VERIFY_PEER` is used, the library does not verify that the server certificate's hostname matches the connection target. This leaves the library vulnerable to man-in-the-middle attacks.

**Approach**: After `SSL_connect()` succeeds, call `SSL_set1_host()` to set the expected hostname, then enable hostname verification via `SSL_set_verify()`.

```c
SSL_set1_host(data->ssl, data->host);
SSL_set_verify(data->ssl, SSL_VERIFY_PEER, NULL);
```

If hostname verification fails, the connection should be closed and treated as a TLS error.

**File**: `sinks/socket_sink.c`

### 4.3 Add Fuzzing Integration

Add AFL/libFuzzer fuzzing harnesses for config parsing and line formatting to catch buffer overflows and parsing edge cases.

**Approach**: Create `fuzz/` directory with:
- `fuzz_config.c` — fuzz `log_config_init()` with arbitrary YAML input
- `fuzz_formatter.c` — fuzz `log_formatter_format()` with arbitrary format strings and records
- `fuzz/seeds/` — seed corpus
- Makefile targets: `fuzz-build`, `fuzz-config`, `fuzz-formatter`

**Files**: New: `fuzz/fuzz_config.c`, `fuzz/fuzz_formatter.c`, `fuzz/seeds/valid_config.yaml`, `Makefile`

---

## 5. Fork Safety

### 5.1 Add atfork_child to Console Sink

The console sink (`console_sink.c`) does not implement the `atfork_child` callback. While stdout/stderr file descriptors are inherited correctly across fork, adding a no-op callback ensures the vtable is complete and future-proofs the console sink against changes.

**Change**: Add `sink->atfork_child = console_atfork_child;` to both `console_sink_create()` and `console_sink_create_stderr()`. The callback is a no-op since stdout/stderr do not need re-opening.

**File**: `sinks/console_sink.c`

### 5.2 Preserve Async Queue Records Across Fork

Currently, `log_async_atfork_child()` resets `head`, `tail`, and `count` to 0, discarding any records in the queue at fork time. This loses log messages that were enqueued but not yet processed by the worker thread.

**Approach**: Instead of resetting the queue, drain the queue in the child process before restarting the worker thread. The child's worker thread processes all remaining records, then continues normally.

```c
void log_async_atfork_child(void) {
    if (!g_async_logger.running || !g_async_logger.queue)
        return;

    mpsc_queue_t *q = g_async_logger.queue;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_full, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->drained, NULL);

    /* Drain existing records instead of discarding them */
    g_async_logger.running = 1;
    if (pthread_create(&g_async_logger.worker_thread, NULL, async_worker, &g_async_logger) != 0) {
        g_async_logger.running = 0;
    }
}
```

The queue's `head`, `tail`, and `count` are preserved from the parent process. The new worker thread will process them normally. The mutex and condition variables are reinitialized to avoid undefined state from the parent.

**File**: `core/async.c`

---

## 6. Test & Verification Plan

1. **Documentation**: Verify README accurately reflects implementation (lock-free claim removed, APIs documented)
2. **Performance**: Benchmark `log_writevprintf()` with rate limiter enabled vs disabled before and after fast-path optimization
3. **Security**: Run fuzzing harnesses for 60 seconds each; verify no crashes or sanitizer errors
4. **Fork Safety**: Run `test_fork_safety.c` with async mode enabled; verify queue records are preserved in child process
5. **Quality Gate**: `make check`, `make test-asan`, `make test-ubsan` — all must pass