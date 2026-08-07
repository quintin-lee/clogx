# Platform Support

clogx targets POSIX (Linux, macOS, BSD) as first-class; Windows is **partial**.
This page is the authoritative status matrix. Everything below is grounded in the
`#if`/`#ifdef` guards and portability abstractions actually present in the tree
(`include/clog_port.h`), not aspirational.

## Verdict at a glance

| Component                          | POSIX            | Windows                                   |
|------------------------------------|------------------|-------------------------------------------|
| `console` sink                     | ✅ full          | ✅ full (stdio)                           |
| `file` sink                        | ✅ full          | ✅ full (stdio `fopen`)                   |
| `socket` sink (sync + async)       | ✅ full          | ⚠️ functional, minor gaps (see below)     |
| `syslog` sink                      | ✅ full          | ❌ **stub** — `syslog_sink_create()` → NULL     |
| `otlp` sink                        | ✅ full          | ✅ full (stdio/stdout)                    |
| `custom` sink                      | ✅ full          | ✅ full (pure callbacks)                  |
| signal handling                    | ✅ full (self-pipe) | ⚠️ functional (flag-based, no self-pipe) |
| plugin loading                      | ✅ full (`dlopen`) | ❌ **stub** — all plugin APIs no-op/NULL |
| async socket writer (`socket_async`) | ✅ full         | ⚠️ functional (Winsock-native)           |

Legend: ✅ works · ⚠️ works but degraded/different · ❌ stub / not implemented.

## What "partial" means concretely

All of these are real, present code paths — nothing here is aspirational.

### Sinks

- **console / file / otlp / custom** — implemented entirely with standard C
  `stdio` (`fwrite`, `fprintf`, `fopen`). No POSIX-only calls, no guards. They
  build and run unchanged on Windows.

- **socket** — built on the Winsock abstraction in `include/clog/port.h`
  (`clog_socket_t`, `clog_net_init`, `clog_close_socket`,
  `clog_is_invalid_socket`, `clog_sock_size_t`), which map to `SOCKET`,
  `WSAStartup`, `closesocket`, `INVALID_SOCKET`. Sends work on Windows.
  **Minor gaps** (guarded by `#if !defined(_WIN32)`):
  - `SO_RCVTIMEO` / `SO_SNDTIMEO` socket timeouts are not applied on Windows.
  - `shutdown(SHUT_WR)` graceful half-close is skipped.
  - The non-blocking writer (`core/socket_async.c`) is Winsock-native
    (`ioctlsocket(FIONBIO)`, `WSAGetLastError`, `WSAEWOULDBLOCK`) and works.
  - TLS (`CLOG_USE_TLS`) is OpenSSL-based and cross-platform, but **only
    enabled explicitly** (`-DCLOG_ENABLE_TLS=ON`); it is not wired into the
    Windows build by default.

- **syslog** — **stub on Windows.** The entire POSIX implementation sits under
  `#ifndef _WIN32`; the Windows branch is `syslog_sink_create()` returning
  `NULL`. No Windows Event Log (`RegisterEventSource` / `ReportEvent`) or
  anything equivalent is implemented.

## Core

- **signal handling** — on Windows, `signal()` is used instead of `sigaction`,
  and the self-pipe trick is unavailable: `log_signal_handler()` only sets a
  flag, and `log_process_pending_signals()` (called from the write path) does
  the flush + re-raise. `log_get_signal_fd()` returns `-1` on Windows, so
  callers wanting to integrate the signal fd into an event loop lose that
  capability. **functional but degraded.**

- **plugin loading** — **stub on Windows.** The whole `_WIN32` branch of
  `core/plugin_loader.c` returns `NULL`/`0` from every function
  (`log_plugin_load`, `log_plugin_create_sink`, `log_plugin_scan`,
  `log_plugin_create_sinks_from_config`), and `log_plugin_shutdown_all()`
  is a no-op. Even though `include/clog/port.h` provides `clog_dlopen`
  (`LoadLibrary`) / `clog_dlsym` (`GetProcAddress`) wrappers, they are **not
  used** — the plugin subsystem is disabled on Windows so the rest of the
  pipeline links without dynamic loading.

## Recommended next steps (to close the gaps)

1. **syslog sink (Windows)** → real implementation via the Windows Event Log
   (`RegisterEventSourceW` / `ReportEventW` / `DeregisterEventSource`),
   mapping severity → event type, inside the existing `_WIN32` branch.
2. **plugin loader (Windows)** → drive the existing `clog_dlopen` /
   `clog_dlsym` / `clog_dlclose` wrappers in `clog/port.h` to implement
   `log_plugin_*` instead of returning `NULL`. DSO filename conventions
   (`.dll` vs `.so`) would need `#ifdef` handling in the path plumbing.
3. **socket timeout on Windows** → apply `SO_RCVTIMEO`/`SO_SNDTIMEO` via
   `setsockopt` (Winsock supports these with a `DWORD` millisecond timeout),
   replacing the skipped POSIX branch.

These are tracked as current known stubs; the rest of the log pipeline
(queue, dispatch, formatter, async) is fully portable.