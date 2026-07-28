# clogx

[![C99](https://img.shields.io/badge/C-C99-blue.svg)](https://en.wikipedia.org/wiki/C99)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![CMake](https://img.shields.io/badge/CMake-3.14%2B-red.svg)](https://cmake.org/)
[![CI](https://github.com/quintin-lee/clogx/actions/workflows/ci.yml/badge.svg)](https://github.com/quintin-lee/clogx/actions/workflows/ci.yml)

Lightweight C99 logging library: config-driven, multi-sink output, optional async queue, size-based log file rotation.

## Features

- Macro API: `LOG_INFO` / `LOG_DEBUG` / `LOG_WARN` / `LOG_ERROR` / `LOG_FATAL` / `LOG_TRACE` (`TRACE` kept as alias)
- Multi-sink: console (optional ANSI color), file (auto-create directories + rotation), TCP socket
- Token formatting: `%time` `%level` `%msg` `%file` `%line` `%func` and more
- Sync / async switchable; async path deep-copies records to avoid dangling stack pointers
- Hot reload: `log_reload()` re-reads config and atomically rebuilds sinks / async worker
- Per-sink level filtering: `log_sink_set_level()` / `log_sink_get_level()` on any sink
- Config validation: rejects invalid `queue_size`, `port`, `backups`, and unknown log levels
- Error handling: structured error codes via `clogx_errno_t` and `log_strerror()`
- Observability: async fallback callback (`log_set_async_fallback_cb()`)
- Printf format safety: compile-time format string validation via `CLOGX_PRINTF_FMT`
- Clean ABI: symbol visibility control exports only 24 public symbols
- Build: Makefile and CMake (with CTest, `find_package(clogx)`); ASan/UBSan/Valgrind check targets

## Directory Layout

```
include/     public headers
core/        config, formatting, dispatch, queue, async, rotation
sinks/       console / file / socket
example/     example programs
tests/       regression tests
cmake/       CMake package config templates
```

## Quick Start

```c
#include "log.h"

int main(void) {
    /* Option 1: load from YAML config file */
    if (log_init("./config.yaml") != 0) {
        return 1;
    }

    /* Option 2: configure programmatically */
    log_config_t cfg = {0};
    cfg.level = LOG_LEVEL_DEBUG;
    cfg.async = false;
    cfg.color = true;
    cfg.format = "[%time] [%level] %msg";
    cfg.file_enable = true;
    snprintf(cfg.file_path, sizeof(cfg.file_path), "logs/app.log");
    log_config_set(&cfg);

    LOG_INFO("Server started");
    LOG_WARN("Disk space low: %d%%", 85);
    LOG_ERROR("Failed to connect to database");

    log_flush();
    log_destroy();
    return 0;
}
```

After `cmake --install`, prefer `#include <clogx/log.h>` (pkg-config / CMake export set the include path).

Linking requires pthread:

```bash
gcc -Iinclude app.c -Lbuild -lclogx -lpthread -o app
```

## Build

### Makefile

```bash
make              # generates build/libclogx.a, build/libclogx.so, build/example
make test         # compiles and runs all 23 regression tests
make asan         # build + test with AddressSanitizer
make ubsan        # build + test with UndefinedBehaviorSanitizer
make check        # full quality gate: format check → build → test
make test-valgrind  # all tests under Valgrind leak check (skipped if not installed)
make format       # apply clang-format to all sources
make docs         # generate Doxygen API docs
make install      # install library + headers + pkg-config
make clean
```

### CMake

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --install build --prefix /usr/local
```

Common options:

| Option | Default | Description |
|--------|---------|-------------|
| `CLOG_BUILD_EXAMPLES` | ON | build examples |
| `CLOG_BUILD_TESTS` | ON | build and register CTest |
| `CLOG_BUILD_SHARED` | OFF | build shared library when ON |
| `CLOG_USE_SYSTEM_YAML` | OFF | use system libyaml instead of auto-downloading |

Downstream projects:

```cmake
find_package(clogx REQUIRED)
target_link_libraries(app PRIVATE clogx::clogx)
```

Or with pkg-config:

```bash
pkg-config --cflags --libs clogx
```

## Configuration

The config file is YAML with all settings under a top-level `log:` mapping.
Pass the path to `log_init(path)`; when empty, defaults to `./config.yaml`.

For backward compatibility, old-style top-level `key: value` pairs (without a
`log:` wrapper) are also accepted.

Example:

```yaml
log:
  level: TRACE
  async: false
  queue_size: 8192
  color: true
  format: "[%time] [%level] [%module] %msg (%file:%line)"
  time_format: "%Y-%m-%d %H:%M:%S"
  console_enable: true
  console_stderr: false
  file_enable: true
  file_path: logs/server.log
  max_size: 100MB
  backups: 10
  socket_enable: false
```

| Key | Meaning |
|-----|---------|
| `level` | minimum output level: `TRACE` / `DEBUG` / `INFO` / `WARN` / `ERROR` / `FATAL` (unknown values are rejected) |
| `async` | enable background consumer thread when `true` |
| `queue_size` | async queue capacity |
| `color` | console ANSI coloring (does not affect file / socket) |
| `format` | format string |
| `time_format` | strftime template for `%time` (default: `%Y-%m-%d %H:%M:%S`); microseconds always appended after `.` |
| `console_enable` | enable console sink |
| `console_stderr` | when `true`, console sink writes to stderr instead of stdout |
| `file_enable` / `file_path` | file sink |
| `max_size` | rotation threshold: raw bytes, or `K`/`KB`, `M`/`MB`, `G`/`GB` |
| `backups` | number of backups to retain (`.1` … `.N`) |
| `socket_enable` / `host` / `port` | TCP socket sink |

Runtime adjustment:

```c
log_set_level(LOG_LEVEL_DEBUG);
log_get_level();
log_reload();   // re-read config path passed to init

/* Or set configuration programmatically */
log_config_t cfg = {0};
cfg.level = LOG_LEVEL_WARN;
cfg.file_enable = true;
snprintf(cfg.file_path, sizeof(cfg.file_path), "logs/app.log");
log_config_set(&cfg);
```

## Format Tokens

| Token | Content |
|-------|---------|
| `%time` | local time `YYYY-MM-DD HH:MM:SS.uuuuuu` |
| `%level` | level name |
| `%msg` | message body |
| `%thread` | thread ID |
| `%pid` | process ID |
| `%file` / `%line` / `%func` | source location |
| `%module` / `%tag` | module (via `log_set_module`) and tag (currently unused / empty) |
| `%newline` | newline |

Example: `[%time] [%level] %file:%line %msg`

## Public API

```c
#include "log.h"           /* in-tree: -Iinclude */
/* #include <clogx/log.h>  after cmake --install */

clogx_errno_t log_init(const char *yaml_path);
void           log_destroy(void);
void           log_flush(void);
clogx_errno_t log_reload(void);
const char    *log_strerror(int err);
void           log_set_async_fallback_cb(void (*cb)(void));
void         (*log_get_async_fallback_cb(void))(void);
void           log_set_module(const char *module);
void           log_get_module(char *buf, size_t n);
int            log_add_sink(log_sink_t *sink);
int            log_remove_sink(log_sink_t *sink);
int            log_set_level(log_level_t level);
log_level_t    log_get_level(void);
void           log_sink_set_level(log_sink_t *sink, log_level_t level);
log_level_t    log_sink_get_level(const log_sink_t *sink);
log_config_t  *log_config_get(void);
int            log_config_set(const log_config_t *cfg);

LOG_INFO("...");
LOG_DEBUG("...");
LOG_WARN("...");
LOG_ERROR("...");
LOG_FATAL("...");
LOG_TRACE("...");
TRACE("..."); /* deprecated alias for LOG_TRACE */
```

Error codes:

| Code | Meaning |
|------|---------|
| `CLOG_OK` | success |
| `CLOG_ERR_INIT_REENTRANT` | `log_init` called without `log_destroy` |
| `CLOG_ERR_CONFIG_OPEN` | failed to open or parse config file |
| `CLOG_ERR_NO_SINKS` | no sinks configured |
| `CLOG_ERR_FILE_OPEN` | failed to open log file |
| `CLOG_ERR_FILE_WRITE` | file write error or short write |
| `CLOG_ERR_QUEUE_FULL` | async queue full or closed |
| `CLOG_ERR_THREAD_CREATE` | failed to create async worker |
| `CLOG_ERR_SOCKET_CONNECT` | socket connect failed |
| `CLOG_ERR_OOM` | out of memory during clone |
| `CLOG_ERR_RELOAD` | reload without init |
| `CLOG_ERR_INVALID_ARG` | invalid argument |

Installed public headers: `log.h`, `log_config.h`, `log_record.h`, `log_sink.h` under `include/clogx/`.
Internal headers (`queue.h`, `dispatcher.h`, `log_async.h`, `log_formatter.h`, …) stay in-tree and are not installed.

## Async Mode

When `async: true`:

1. calling thread formats message and deep-copies string fields
2. record is enqueued to a bounded MPSC queue
3. background worker dequeues and passes to dispatcher
4. `log_flush()` / `log_destroy()` / `log_reload()` correctly drain or stop the worker

When the async path cannot accept a record (queue full, queue closed, OOM, or
worker not running), the library falls back to synchronous dispatch without
blocking the caller. Register `log_set_async_fallback_cb()` to observe these
fallbacks.

## Architecture

```
LOG_* ──► log_writevprintf
               ├─ level filter
               ├─ assemble log_record_t
               └─ async? ──► queue ──► worker ──► dispatcher
                            └─ sync ─────────────► dispatcher
                                                     ├─ formatter
                                                     └─ console / file / socket
```

Reload path uses an atomic snapshot swap so old sinks stay alive until new sinks are ready:

```
config reload → build new snapshot → shutdown async → commit snapshot → restart async if needed
```

## Tests

```bash
make test
# or
ctest --test-dir build --output-on-failure
```

Covers async lifecycle, reload start/stop worker, dispatcher reuse, file rotation, nested directory creation, config hot reload, invalid config handling, double init protection, empty sink rejection, async fallback notification, non-blocking queue overflow, max_size unit parsing, stderr console routing, module/truncation behavior, custom sink registration, multi-threaded sync correctness, socket sink TCP output, per-sink level filtering, runtime log level changes, programmatic config via `log_config_set`, and boundary conditions (empty YAML, comments, old-style top-level keys, unknown keys, NULL path). Total: 23 tests.

## CI

GitHub Actions runs:

- Makefile and CMake build/test matrix
- AddressSanitizer (`clang` + `-fsanitize=address`)
- Valgrind leak check on all tests
- clang-format compliance check
- `cppcheck --enable=warning,performance,portability` on `include/`, `core/`, `sinks/`

## API Documentation (Doxygen)

Headers and implementations include Doxygen comments. Generate HTML:

```bash
make docs   # requires doxygen
# Output: docs/api/html/index.html
```

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
