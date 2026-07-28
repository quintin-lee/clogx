# Design Spec: Signal-Safe Flag Handler and Fork Sink Re-open

**Date**: 2026-07-28  
**Topic**: Refactoring Signal Handling to Flag-Only Async-Signal-Safety & Fork Child Re-opening of File/Socket Sinks  
**Status**: Approved  

---

## 1. Overview

This design addresses two key concerns:
1. Signal handlers must be strictly Async-Signal-Safe under POSIX standards: `log_signal_handler(int sig)` only sets `g_signal_pending = sig`. Pending signals are processed on the main thread during `log_writevprintf` or explicit `log_process_pending_signals()` calls.
2. In child processes created by `fork()`, file sinks must close and re-open file descriptors to prevent offset sharing, and socket sinks must close old sockets/TLS sessions so the child process establishes its own independent network connection.

---

## 2. Component Design

### 2.1 Async-Signal-Safe Signal Handler (`core/signal_handler.c` & `include/log.h`)

- **`log_signal_handler(int sig)`**:
  - `g_signal_pending = sig;` (atomic assignment, no mutexes, no allocations, no file I/O).

- **`log_get_pending_signal(void)` & `log_process_pending_signals(void)`**:
  - Main thread methods that flush logs, restore default signal handlers, and re-raise pending signals safely outside signal context.

- **`log_writevprintf(...)` (`core/log.c`)**:
  - Checks `if (log_get_pending_signal() != 0)` and triggers `log_process_pending_signals()`.

### 2.2 Sink Fork Hooks (`include/log_sink.h`, `sinks/file_sink.c`, `sinks/socket_sink.c`)

- **`log_sink_t` (`include/log_sink.h`)**:
  - Add `void (*atfork_child)(struct log_sink *sink);` pointer.

- **File Sink (`sinks/file_sink.c`)**:
  - Closes inherited `FILE *file` and re-opens `path` via `fopen(path, "a")`.

- **Socket Sink (`sinks/socket_sink.c`)**:
  - Closes inherited `sockfd` and frees TLS state; resets `sockfd = -1` for lazy re-connection in child.

- **Dispatcher (`core/dispatcher.c`)**:
  - Invokes `atfork_child` on all active sinks during `log_dispatcher_atfork_child()`.

---

## 3. Test & Verification Plan

1. **Unit Test Updates**:
   - `test_signal_handler.c`: Verifies signal flag processing and log flushing.
   - `test_fork_safety.c`: Verifies file and socket sink re-opening in child process.
2. **Build Suite Verification**:
   - `make check`
   - `make test-asan`
   - `make test-ubsan`
