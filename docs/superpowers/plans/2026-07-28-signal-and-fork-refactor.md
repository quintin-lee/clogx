# Signal & Fork Sink Re-open Refactoring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor signal handling to flag-only Async-Signal-Safe operations, and implement `atfork_child` re-opening for file and socket sinks.

**Architecture:**
- `core/signal_handler.c`: `log_signal_handler(sig)` sets `g_signal_pending`. Add `log_get_pending_signal()` and `log_process_pending_signals()`.
- `include/log_sink.h`: Add `.atfork_child` callback.
- `sinks/file_sink.c` & `sinks/socket_sink.c`: Implement `atfork_child` to re-open/re-connect sinks in child process.
- `core/dispatcher.c`: Invoke `atfork_child` on all active sinks during `log_dispatcher_atfork_child()`.

---

### Task 1: Refactor Signal Handler to Flag-Only Async-Signal-Safety

**Files:**
- Modify: `include/log.h`
- Modify: `core/signal_handler.c`
- Modify: `core/log.c`

- [ ] **Step 1: Update `include/log.h`**

Add `int log_get_pending_signal(void);` and `void log_process_pending_signals(void);`.

- [ ] **Step 2: Update `core/signal_handler.c`**

Make `log_signal_handler(int sig)` flag-only. Implement `log_get_pending_signal()` and `log_process_pending_signals()`.

- [ ] **Step 3: Integrate pending signal checks in `core/log.c`**

Call `log_process_pending_signals()` inside `log_writevprintf()`.

- [ ] **Step 4: Verify build**

Run: `make clean && make test`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/log.h core/signal_handler.c core/log.c
git commit -m "refactor(signal): make signal handler flag-only async-signal-safe"
```

---

### Task 2: Implement Sink `atfork_child` Re-open & Re-connect

**Files:**
- Modify: `include/log_sink.h`
- Modify: `sinks/file_sink.c`
- Modify: `sinks/socket_sink.c`
- Modify: `core/dispatcher.c`

- [ ] **Step 1: Update `include/log_sink.h`**

Add `void (*atfork_child)(struct log_sink *sink);` to `log_sink_t`.

- [ ] **Step 2: Implement `file_sink_atfork_child` in `sinks/file_sink.c`**

Close old file descriptor and `fopen(path, "a")` new independent file handle in child.

- [ ] **Step 3: Implement `socket_sink_atfork_child` in `sinks/socket_sink.c`**

Close old socket, free TLS state, and reset `sockfd = -1` for lazy reconnect in child.

- [ ] **Step 4: Update `log_dispatcher_atfork_child` in `core/dispatcher.c`**

Call `sink->atfork_child(sink)` for all active sinks.

- [ ] **Step 5: Verify build & tests**

Run: `make clean && make test`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add include/log_sink.h sinks/file_sink.c sinks/socket_sink.c core/dispatcher.c
git commit -m "feat(sink): implement atfork_child re-open for file and socket sinks"
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
