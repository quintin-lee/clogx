# Graceful Shutdown Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement POSIX `SIGTERM`/`SIGINT` signal handlers and `log_install_signal_handlers()` / `log_signal_handler(sig)` APIs for graceful log flushing prior to termination.

**Architecture:** Create `core/signal_handler.c` and `include/log_signal.h`. Add `catch_signals` configuration option in `include/log_config.h` and `core/config.c`. Expose public API in `include/log.h`.

**Tech Stack:** C99, POSIX `sigaction`, `signal.h`.

---

### Task 1: Add Public API, Configuration Extensions & Signal Engine

**Files:**
- Modify: `include/log.h`
- Modify: `include/log_config.h`
- Modify: `core/config.c`
- Create: `include/log_signal.h`
- Create: `core/signal_handler.c`
- Modify: `core/log.c`

- [ ] **Step 1: Update `include/log.h`**

Declare `log_install_signal_handlers()` and `log_signal_handler()`.

- [ ] **Step 2: Update `include/log_config.h` and `core/config.c`**

Add `bool catch_signals` to `log_config_t` and parse `catch_signals` key in `core/config.c`.

- [ ] **Step 3: Create `include/log_signal.h` and `core/signal_handler.c`**

Implement `log_install_signal_handlers()` and `log_signal_handler(int sig)` in `core/signal_handler.c`.

- [ ] **Step 4: Integrate in `core/log.c`**

If `cfg->catch_signals` is true in `log_init()`, invoke `log_install_signal_handlers()`.

- [ ] **Step 5: Verify build**

Run: `make clean && make test`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add include/log.h include/log_config.h include/log_signal.h core/config.c core/signal_handler.c core/log.c
git commit -m "feat(signal): add graceful shutdown signal handlers and catch_signals config"
```

---

### Task 2: Create `test_signal_handler.c` Unit Test & Register in Build System

**Files:**
- Create: `tests/test_signal_handler.c`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Create `tests/test_signal_handler.c`**

Test signal handling and log flushing via `raise(SIGINT)` in child process.

- [ ] **Step 2: Register `test_signal_handler` in `Makefile` and `CMakeLists.txt`**

- [ ] **Step 3: Verify test suite**

Run: `make clean && make test`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add tests/test_signal_handler.c Makefile CMakeLists.txt
git commit -m "test: add test_signal_handler unit test"
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
