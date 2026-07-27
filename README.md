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
- Config validation: rejects invalid `queue_size`, `port`, `backups`, and unknown log levels
- Error handling: structured error codes via `clogx_errno_t` and `log_strerror()`
- Observability: async fallback callback (`log_set_async_fallback_cb()`)
- Build: Makefile and CMake (with CTest, `find_package(clogx)`)

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
    if (log_init("./config.yaml") != 0) {
        return 1;
    }

    LOG_INFO("Server started");
    LOG_WARN("Disk space low: %d%%", 85);
    LOG_ERROR("Failed to connect to database");

    log_flush();
    log_destroy();
    return 0;
}
```

Linking requires pthread:

```bash
gcc -Iinclude app.c -Lbuild -lclogx -lpthread -o app
```

## Build

### Makefile

```bash
make          # generates build/libclogx.a and build/example
make example
make test     # compiles and runs all regression tests
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

The config file is a simple `key: value` text format (not full YAML). Pass the path to `log_init(path)`; when empty, defaults to `./config.yaml`.

Example:

```yaml
level: INFO
async: false
queue_size: 8192
color: true
format: [%time] [%level] %msg
console_enable: true
file_enable: true
file_path: logs/server.log
max_size: 100MB
backups: 10
socket_enable: false
host: 127.0.0.1
port: 5140
```

| Key | Meaning |
|-----|---------|
| `level` | minimum output level: `TRACE` / `DEBUG` / `INFO` / `WARN` / `ERROR` / `FATAL` (unknown values are rejected) |
| `async` | enable background consumer thread when `true` |
| `queue_size` | async queue capacity |
| `color` | console ANSI coloring (does not affect file / socket) |
| `format` | format string |
| `console_enable` | enable console sink |
| `console_stderr` | when `true`, console sink writes to stderr instead of stdout |
| `file_enable` / `file_path` | file sink; key `path` also accepted |
| `max_size` | rotation threshold: raw bytes, or `K`/`KB`, `M`/`MB`, `G`/`GB` |
| `backups` | number of backups to retain (`.1` … `.N`) |
| `socket_enable` / `host` / `port` | TCP socket sink |

Runtime adjustment:

```c
log_set_level(LOG_LEVEL_DEBUG);
log_get_level();
log_reload();   // re-read config path passed to init
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
| `%module` / `%tag` | module and tag (current log entry's module is fixed to `"main"`) |
| `%newline` | newline |

Example: `[%time] [%level] %file:%line %msg`

## Public API

```c
#include "log.h"

clogx_errno_t log_init(const char *yaml_path);
void           log_destroy(void);
void           log_flush(void);
clogx_errno_t log_reload(void);
const char    *log_strerror(int err);
void           log_set_async_fallback_cb(void (*cb)(void));
void         (*log_get_async_fallback_cb(void))(void);

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

Lower-level interfaces: `include/log_config.h`, `log_async.h`, `log_sink.h`, `dispatcher.h`, `log_record.h`, `log_formatter.h`.

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

Covers async lifecycle, reload start/stop worker, dispatcher reuse, file rotation, nested directory creation, config hot reload, invalid config handling, double init protection, empty sink rejection, async fallback notification, non-blocking queue overflow, max_size unit parsing, and stderr console routing. Total: 15 tests.

## CI

GitHub Actions runs:

- Makefile and CMake build/test matrix
- AddressSanitizer (`clang` + `-fsanitize=address`)
- Valgrind leak check on core tests
- `cppcheck --enable=warning,performance,portability` on `include/`, `core/`, `sinks/`

## API Documentation (Doxygen)

Headers and implementations include Doxygen comments. Generate HTML:

```bash
make docs   # requires doxygen
# Output: docs/api/html/index.html
```

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
