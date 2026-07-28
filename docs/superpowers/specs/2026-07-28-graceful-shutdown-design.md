# Design Spec: Graceful Shutdown and Signal Handling (`SIGTERM`/`SIGINT`)

**Date**: 2026-07-28  
**Topic**: POSIX `sigaction` Signal Handling for Graceful Shutdown (`SIGTERM`, `SIGINT`)  
**Status**: Approved  

---

## 1. Overview

This design adds graceful shutdown capabilities to `clogx` when process termination signals (`SIGTERM`, `SIGINT`) occur. It provides both an automatic YAML configuration switch (`catch_signals: true`) / API (`log_install_signal_handlers()`) and an explicit signal handling function (`log_signal_handler(int sig)`) for integration into application-specific signal loops.

---

## 2. API & Configuration Extensions

### 2.1 Public API (`include/log.h`)

```c
/**
 * @brief Register automatic signal handlers for SIGTERM and SIGINT.
 * @return CLOG_OK on success.
 */
CLOGX_API clogx_errno_t log_install_signal_handlers(void);

/**
 * @brief Signal-safe handler that flushes logs, restores default signal, and re-raises.
 * @param[in] sig Signal number (SIGTERM, SIGINT, etc.).
 */
CLOGX_API void log_signal_handler(int sig);
```

### 2.2 Struct `log_config_t` (`include/log_config.h`)

```c
typedef struct {
    ...
    bool catch_signals; /**< Automatically install SIGTERM/SIGINT handlers on log_init(). */
} log_config_t;
```

---

## 3. Signal Engine Design (`core/signal_handler.c`)

1. **State**:
   - `static sig_atomic_t g_signal_handled = 0;`
   - `static struct sigaction g_old_sigterm;`
   - `static struct sigaction g_old_sigint;`
   - `static bool g_handlers_installed = false;`

2. **`log_signal_handler(int sig)`**:
   - If `g_signal_handled` is non-zero, returns immediately (re-entrancy protection).
   - Sets `g_signal_handled = 1`.
   - Calls `log_flush()` to flush all enqueued and active logs to disk/network.
   - Restores `g_old_sigterm` or `g_old_sigint`.
   - Re-raises the signal using `raise(sig)` so containers (Docker/Kubernetes) and process supervisors receive the true process termination status code.

3. **`log_install_signal_handlers()`**:
   - Uses `sigaction` to install `log_signal_handler` for `SIGTERM` and `SIGINT` with `SA_RESETHAND`.

---

## 4. Test & Verification Plan

1. **Unit Test (`tests/test_signal_handler.c`)**:
   - Tests `log_install_signal_handlers()` and `log_signal_handler(SIGTERM)` in a child process, verifying log flush completion prior to exit.
2. **Build Suite Verification**:
   - `make check`
   - `make test-asan`
   - `make test-ubsan`
