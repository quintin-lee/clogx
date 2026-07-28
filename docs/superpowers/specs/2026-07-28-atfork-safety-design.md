# Design Spec: `pthread_atfork` Safety and Child Process Worker Restart

**Date**: 2026-07-28  
**Topic**: POSIX `pthread_atfork` integration preventing deadlocks and recreating async worker threads in child processes  
**Status**: Approved  

---

## 1. Overview

This design addresses the undefined behavior and deadlocks caused when `fork()` is called in a multi-threaded application using `clogx` in async mode. By registering `pthread_atfork` handlers, `clogx` acquires all internal locks prior to `fork()`, releases them in the parent process after `fork()`, and in the child process releases the locks and safely recreates the `mpsc_queue` and `async_worker` thread.

---

## 2. Component Design

### 2.1 Child Process Re-initialization (`core/async.c`)

Add `log_async_atfork_child()` in `core/async.c`:

```c
void log_async_atfork_child(void) {
    if (!g_async_logger.running || !g_async_logger.queue) {
        return;
    }

    size_t cap = g_async_logger.queue->capacity;
    /* Clean up old queue structure without calling pthread_join on non-existent thread */
    mpsc_queue_destroy(g_async_logger.queue);

    g_async_logger.queue = mpsc_queue_create(cap);
    if (!g_async_logger.queue) {
        g_async_logger.running = 0;
        return;
    }

    g_async_logger.running = 1;
    if (pthread_create(&g_async_logger.worker_thread, NULL, async_worker, &g_async_logger) != 0) {
        mpsc_queue_destroy(g_async_logger.queue);
        g_async_logger.queue = NULL;
        g_async_logger.running = 0;
    }
}
```

### 2.2 Global Fork Handlers (`core/log.c`)

Register `pthread_atfork` in `log_init()`:

- `log_atfork_prepare()`: Acquires `g_init_mutex`, `g_config_rwlock` (write lock), `g_module_mutex`.
- `log_atfork_parent()`: Releases `g_module_mutex`, `g_config_rwlock`, `g_init_mutex` in parent.
- `log_atfork_child()`: Releases `g_module_mutex`, `g_config_rwlock`, `g_init_mutex` in child, then invokes `log_async_atfork_child()`.

---

## 3. Test & Verification Plan

1. **Unit Test (`tests/test_fork_safety.c`)**:
   - Initializes `clogx` with `async: true`.
   - Fires logs in parent.
   - Calls `fork()`.
   - In child: logs messages, flushes, destroys logger, exits with status 0.
   - In parent: waits for child (`waitpid`), logs messages, flushes, verifies log output file contains entries from both parent and child.
2. **Build Suite Verification**:
   - `make check`
   - `make test-asan`
   - `make test-ubsan`
