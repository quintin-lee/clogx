# Production Readiness Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement remaining production readiness fixes across documentation, performance, security hardening, and fork safety.

**Architecture:**
- Documentation: Update `README.md`.
- Performance: Add atomic/fast-path check in `core/rate_limit.c`.
- Security: Update `sinks/socket_sink.c` for OpenSSL 1.1.0+ (`TLS_method()`, `SSL_set1_host`), add fuzzing harnesses in `fuzz/`.
- Fork Safety: Add `atfork_child` in `sinks/console_sink.c` and preserve enqueued records in `core/async.c`.

---

### Task 1: Documentation & Accuracy Updates

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Fix Lock-Free MPSC queue claim**
  Replace "Lock-free MPSC queue" with "Mutex-protected bounded MPSC queue".

- [ ] **Step 2: Document missing public signal APIs and rate limit performance notes**
  Add documentation for `log_get_pending_signal()` and `log_process_pending_signals()`. Add rate limiter mutex note.

- [ ] **Step 3: Commit**
```bash
git add README.md
git commit -m "docs: correct queue concurrency claim and document signal APIs"
```

---

### Task 2: Performance — Rate Limiter Fast Path

**Files:**
- Modify: `core/rate_limit.c`

- [ ] **Step 1: Implement fast-path check when rate limiting is disabled**
  Check `!g_enabled` before acquiring `g_rate_mutex` in `log_rate_limit_allow()`.

- [ ] **Step 2: Verify test suite**
  Run: `make clean && make test`
  Expected: PASS

- [ ] **Step 3: Commit**
```bash
git add core/rate_limit.c
git commit -m "perf(rate_limit): add fast-path check when rate limiting is disabled"
```

---

### Task 3: Security Hardening — OpenSSL API Modernization & TLS Hostname Verification

**Files:**
- Modify: `sinks/socket_sink.c`

- [ ] **Step 1: Replace deprecated `TLS_client_method()` with `TLS_method()`**

- [ ] **Step 2: Add TLS Hostname Verification (`SSL_set1_host`)**

- [ ] **Step 3: Verify build**
  Run: `make clean && make test`
  Expected: PASS

- [ ] **Step 4: Commit**
```bash
git add sinks/socket_sink.c
git commit -m "security(tls): modernize OpenSSL API and add hostname verification"
```

---

### Task 4: Security Hardening — Fuzzing Harnesses Integration

**Files:**
- Create: `fuzz/fuzz_config.c`
- Create: `fuzz/fuzz_formatter.c`
- Create: `fuzz/seeds/valid_config.yaml`
- Modify: `Makefile`

- [ ] **Step 1: Create `fuzz/fuzz_config.c` and `fuzz/fuzz_formatter.c`**

- [ ] **Step 2: Create `fuzz/seeds/valid_config.yaml`**

- [ ] **Step 3: Update `Makefile` with `fuzz-build`, `fuzz-config`, `fuzz-formatter` rules**

- [ ] **Step 4: Verify build**
  Run: `make fuzz-build`
  Expected: Success

- [ ] **Step 5: Commit**
```bash
git add fuzz/ Makefile
git commit -m "security(fuzz): add AFL/libFuzzer harnesses for config parsing and formatter"
```

---

### Task 5: Fork Safety — Console Sink Callback & Async Queue Preservation

**Files:**
- Modify: `sinks/console_sink.c`
- Modify: `core/async.c`

- [ ] **Step 1: Add no-op `atfork_child` callback to `sinks/console_sink.c`**

- [ ] **Step 2: Preserve enqueued records across fork in `core/async.c`**
  Remove queue head/tail/count resetting in `log_async_atfork_child()` so pending records are preserved and processed by child's worker thread.

- [ ] **Step 3: Verify test suite**
  Run: `make clean && make test`
  Expected: PASS

- [ ] **Step 4: Commit**
```bash
git add sinks/console_sink.c core/async.c
git commit -m "feat(fork): add console atfork_child callback and preserve async queue records"
```

---

### Task 6: Quality Gate Verification

- [ ] **Step 1: Run standard test suite and format check**
  Run: `make check`
  Expected: ALL PASS

- [ ] **Step 2: Run ASan test suite**
  Run: `make test-asan`
  Expected: ALL PASS

- [ ] **Step 3: Run UBSan test suite**
  Run: `make test-ubsan`
  Expected: ALL PASS
