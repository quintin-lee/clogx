# Design Spec: Thread-Local Mapped Diagnostic Context (MDC)

**Date**: 2026-07-29  
**Topic**: Thread-Local Storage (TLS) Context Key-Value Pairs (MDC) for Structured JSON & Formatted Logs  
**Status**: Approved  

---

## 1. Overview

Mapped Diagnostic Context (MDC) allows threads (e.g. web request handlers, RPC workers) to attach contextual metadata such as `trace_id`, `user_id`, or `request_id` to the current thread. All log messages emitted on that thread automatically include the context metadata in JSON output or `%context` format strings.

---

## 2. Component Design

### 2.1 Public API (`include/log.h`)

```c
/**
 * @brief Set a thread-local context key-value pair.
 * @param[in] key   Context key name (e.g. "trace_id"). Pass NULL to clear key.
 * @param[in] value Context value string. Pass NULL to clear key.
 * @return CLOG_OK on success, CLOG_ERR_INVALID_ARG on failure.
 */
CLOGX_API clogx_errno_t log_set_thread_context(const char *key, const char *value);

/**
 * @brief Get a thread-local context value for a key.
 * @param[in] key Context key name.
 * @return Value string if set, or NULL if not found / empty.
 */
CLOGX_API const char *log_get_thread_context(const char *key);

/**
 * @brief Clear all thread-local context key-value pairs for the calling thread.
 */
CLOGX_API void log_clear_thread_context(void);
```

### 2.2 Formatting & Output (`core/formatter.c`)

- Formatter token `%context`: Formats thread context as `[key1=val1 key2=val2]`.
- JSON format (`format: "json"`): Injects active thread-local context key-value pairs as top-level JSON attributes (e.g. `"trace_id":"abc-123"`).

---

## 3. Test & Verification Plan

1. **Unit Test**: `tests/test_thread_context.c`
2. **Quality Gate**: `make check`, `make test-asan`, `make test-ubsan`
