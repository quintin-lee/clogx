# Rate Limiter Monotonic Clock Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate token bucket rate limiter time source to POSIX `CLOCK_MONOTONIC`.

**Architecture:** Update `get_now_us()` in `core/rate_limit.c` to use `clock_gettime(CLOCK_MONOTONIC, &ts)`.

**Tech Stack:** C99, POSIX `clock_gettime`, `CLOCK_MONOTONIC`.

---

### Task 1: Update `core/rate_limit.c` to `CLOCK_MONOTONIC`

**Files:**
- Modify: `core/rate_limit.c`

- [ ] **Step 1: Update `get_now_us()` in `core/rate_limit.c`**

Replace `gettimeofday` with `clock_gettime(CLOCK_MONOTONIC, &ts)`.

- [ ] **Step 2: Verify build & rate limit test**

Run: `make clean && make test`
Expected: PASS

- [ ] **Step 3: Commit**

```bash
git add core/rate_limit.c
git commit -m "refactor(rate_limit): migrate time source to POSIX CLOCK_MONOTONIC"
```

---

### Task 2: Quality Gate Verification

- [ ] **Step 1: Run standard test suite and format check**

Run: `make check`
Expected: ALL PASS

- [ ] **Step 2: Run ASan test suite**

Run: `make test-asan`
Expected: ALL PASS

- [ ] **Step 3: Run UBSan test suite**

Run: `make test-ubsan`
Expected: ALL PASS
