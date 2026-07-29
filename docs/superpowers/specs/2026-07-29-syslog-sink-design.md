# Design Spec: POSIX Syslog Sink

**Date**: 2026-07-29  
**Topic**: Native POSIX Syslog Sink Implementation for clogx  
**Status**: Approved  

---

## 1. Overview

This feature provides a native POSIX `syslog_sink` allowing applications running on Linux/macOS/UNIX systems to emit log events directly to system logging services (syslogd, journald via syslog compatibility socket).

---

## 2. Component Design

### 2.1 Public Factory API (`include/log_sink.h`)

```c
/**
 * @brief Create a POSIX syslog sink.
 * @param[in] ident    Program identifier tag string (e.g. "my_app").
 * @param[in] facility Syslog facility (e.g. LOG_USER, LOG_DAEMON, LOG_LOCAL0).
 * @return New sink, or NULL on platforms without syslog / allocation failure.
 */
CLOGX_API log_sink_t *syslog_sink_create(const char *ident, int facility);
```

### 2.2 Implementation (`sinks/syslog_sink.c`)

- Under non-Windows environments (`#ifndef _WIN32`):
  - Maps `log_level_t` enum values to syslog priority constants (`LOG_DEBUG`, `LOG_INFO`, `LOG_WARNING`, `LOG_ERR`, `LOG_CRIT`).
  - Calls `openlog(ident, LOG_PID | LOG_NDELAY, facility)` on create.
  - Calls `syslog(priority, "%s", message)` on write.
  - Implements `syslog_atfork_child` callback to re-open `syslog` with child PID.
- Under Windows (`#ifdef _WIN32`):
  - Returns NULL or no-op stub.

---

## 3. Test & Verification Plan

1. **Unit Test**: `tests/test_syslog_sink.c`
2. **Quality Gate**: `make check`, `make test-asan`, `make test-ubsan`
