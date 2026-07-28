# `pthread_atfork` Safety Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement `pthread_atfork` handlers ensuring deadlock-free `fork()` behavior and seamless async worker thread restarting in child processes.

**Architecture:** Add `log_async_atfork_child()` in `core/async.c` and `include/log_async.h`. Implement `log_atfork_prepare`, `log_atfork_parent`, `log_atfork_child` in `core/log.c`. Register handlers with `pthread_atfork()` in `log_init()`.

**Tech Stack:** C99, POSIX pthreads, `pthread_atfork`.

---

### Task 1: Add `log_async_atfork_child()` and `pthread_atfork` Handlers

**Files:**
- Modify: `include/log_async.h`
- Modify: `core/async.c`
- Modify: `core/log.c`

- [ ] **Step 1: Update `include/log_async.h`**

Declare `void log_async_atfork_child(void);`.

- [ ] **Step 2: Implement `log_async_atfork_child()` in `core/async.c`**

Safely recreate `mpsc_queue` and spawn a new `async_worker` thread in child process.

- [ ] **Step 3: Implement `pthread_atfork` handlers in `core/log.c`**

Implement `log_atfork_prepare()`, `log_atfork_parent()`, `log_atfork_child()` and call `pthread_atfork()` in `log_init()`.

- [ ] **Step 4: Verify build**

Run: `make clean && make test`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/log_async.h core/async.c core/log.c
git commit -m "feat(async): implement pthread_atfork handlers and child worker restart"
```

---

### Task 2: Create `test_fork_safety.c` Unit Test & Register in Build System

**Files:**
- Create: `tests/test_fork_safety.c`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create `tests/test_fork_safety.c`**

Test parent/child async logging across `fork()` and `waitpid()`.

- [ ] **Step 2: Register `test_fork_safety` in `Makefile` and `CMakeLists.txt`**

- [ ] **Step 3: Verify test suite**

Run: `make clean && make test`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add tests/test_fork_safety.c Makefile CMakeLists.txt
git commit -m "test: add test_fork_safety unit test"
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
