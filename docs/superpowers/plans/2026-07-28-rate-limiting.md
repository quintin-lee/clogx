# Token Bucket Rate Limiting Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement global token bucket rate limiting (`rate_limit_enable`, `rate_limit_max_per_sec`, `rate_limit_burst`) with automatic suppression count notifications.

**Architecture:** Create `include/log_rate_limit.h` and `core/rate_limit.c`. Extend `log_config_t` and `core/config.c`. Integrate into `log_writevprintf` in `core/log.c`.

**Tech Stack:** C99, POSIX pthreads, microsecond timer.

---

### Task 1: Extend `log_config_t` and Configuration Parser

**Files:**
- Modify: `include/log_config.h`
- Modify: `core/config.c`

- [ ] **Step 1: Update `include/log_config.h`**

Add `rate_limit_enable`, `rate_limit_max_per_sec`, `rate_limit_burst` fields to `log_config_t`.

- [ ] **Step 2: Update `core/config.c`**

Parse `rate_limit_enable`, `rate_limit_max_per_sec`, and `rate_limit_burst` in YAML parser.

- [ ] **Step 3: Verify build**

Run: `make clean && make test`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add include/log_config.h core/config.c
git commit -m "feat(config): add rate limiting configuration fields and parser support"
```

---

### Task 2: Implement Rate Limiter Module & Integrate into Log Pipeline

**Files:**
- Create: `include/log_rate_limit.h`
- Create: `core/rate_limit.c`
- Modify: `core/log.c`
- Create: `tests/test_rate_limit.c`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create `include/log_rate_limit.h`**

Define `log_rate_limit_init()`, `log_rate_limit_allow()`, `log_rate_limit_reset()`.

- [ ] **Step 2: Implement `core/rate_limit.c`**

Implement microsecond-based token bucket algorithm and suppression counter logic.

- [ ] **Step 3: Integrate rate limiter into `core/log.c`**

Call `log_rate_limit_init()` in `log_init()` / `log_config_set()`. Check `log_rate_limit_allow()` in `log_writevprintf()`.

- [ ] **Step 4: Create unit test `tests/test_rate_limit.c`**

Test burst allowance, log dropping, microsecond token refill, and suppression warning message.

- [ ] **Step 5: Register `test_rate_limit` in `Makefile` and `CMakeLists.txt`**

- [ ] **Step 6: Verify build and test suite**

Run: `make clean && make test`
Expected: PASS

- [ ] **Step 7: Commit**

```bash
git add include/log_rate_limit.h core/rate_limit.c core/log.c tests/test_rate_limit.c Makefile CMakeLists.txt
git commit -m "feat: implement token bucket rate limiting and suppression notice"
```

---

### Task 3: Quality Gate Verification

- [ ] **Step 1: Run standard test suite and format check**

Run: `make check`
Expected: ALL PASS

- [ ] **Step 2: Run ASan test suite**

Run: `make test-asan`
Expected: ALL PASS

- [ ] **Step 3: Run UBSan test suite**

Run: `make test-ubsan`
Expected: ALL PASS
