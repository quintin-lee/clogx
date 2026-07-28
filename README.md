# clogx

[![C99](https://img.shields.io/badge/C-C99-blue.svg)](https://en.wikipedia.org/wiki/C99)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![CMake](https://img.shields.io/badge/CMake-3.14%2B-red.svg)](https://cmake.org/)
[![CI](https://github.com/quintin-lee/clogx/actions/workflows/ci.yml/badge.svg)](https://github.com/quintin-lee/clogx/actions/workflows/ci.yml)

Lightweight C99 logging library: config-driven, multi-sink output, optional async queue, size-based log file rotation, native JSON structured logging, and optional TLS transport security.

## Features

- Macro API: `LOG_INFO` / `LOG_DEBUG` / `LOG_WARN` / `LOG_ERROR` / `LOG_FATAL` / `LOG_TRACE` (`TRACE` kept as alias)
- Multi-sink: console (optional ANSI color), file (auto-create directories + rotation), TCP socket (optional OpenSSL TLS encryption)
- Structured Logging: native single-line JSON format (`format: "json"`) with RFC 8259 string escaping
- Token formatting: `%time` `%level` `%msg` `%file` `%line` `%func` `%module` `%tag` `%thread` `%pid`
- Buffer Limit Safety: macro-configurable buffer sizes (`include/log_limits.h`) with strict boundary checks
- Fuzz Testing: built-in AFL / libFuzzer test harnesses (`make fuzz-build`)
- Sync / async switchable; async path deep-copies records to avoid dangling stack pointers
- Fork Safety: POSIX `pthread_atfork` handlers prevent deadlocks and restart async worker in child processes
- Graceful Shutdown: POSIX `sigaction` signal handlers for `SIGTERM`/`SIGINT` (`catch_signals: true` or `log_install_signal_handlers()`)
- Hot reload: `log_reload()` re-reads config and atomically rebuilds sinks / async worker
- Per-sink level filtering: `log_sink_set_level()` / `log_sink_get_level()` on any sink
- Config validation: rejects invalid `queue_size`, `port`, `backups`, and unknown log levels
- Error handling: structured error codes via `clogx_errno_t` and `log_strerror()`
- Observability: async fallback callback (`log_set_async_fallback_cb()`)
- Printf format safety: compile-time format string validation via `CLOGX_PRINTF_FMT`
- Clean ABI: symbol visibility control exports public symbols cleanly
- Build: Makefile and CMake (with CTest, `find_package(clogx)`); ASan/UBSan/Valgrind check targets

## Directory Layout

```
include/     public headers (log.h, log_config.h, log_limits.h, log_record.h, log_sink.h)
core/        config, formatting, dispatch, queue, async, rotation
sinks/       console / file / socket (with TLS support)
fuzz/        AFL fuzzing harnesses (fuzz_config.c, fuzz_formatter.c)
example/     example programs
tests/       regression tests (24 test suites)
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
    cfg.format = "json";  /* or "[%time] [%level] %msg" */
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

If built with OpenSSL TLS support (`TLS=1` / `CLOG_ENABLE_TLS=ON`):

```bash
gcc -Iinclude app.c -Lbuild -lclogx -lpthread -lssl -lcrypto -o app
```

## Build

### Makefile

```bash
make              # generates build/libclogx.a, build/libclogx.so, build/example
make TLS=1        # builds with OpenSSL TLS socket sink support
make test         # compiles and runs all 24 regression tests
make asan         # build + test with AddressSanitizer
make ubsan        # build + test with UndefinedBehaviorSanitizer
make check        # full quality gate: format check → build → test
make test-valgrind  # all tests under Valgrind leak check (skipped if not installed)
make fuzz-build   # builds AFL fuzzing binaries (build/fuzz_config, build/fuzz_formatter)
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

Common CMake options:

| Option | Default | Description |
|--------|---------|-------------|
| `CLOG_BUILD_EXAMPLES` | ON | build examples |
| `CLOG_BUILD_TESTS` | ON | build and register CTest |
| `CLOG_BUILD_SHARED` | OFF | build shared library when ON |
| `CLOG_USE_SYSTEM_YAML` | OFF | use system libyaml instead of auto-downloading |
| `CLOG_ENABLE_TLS` | OFF | enable OpenSSL TLS support for socket sink |

Downstream projects:

```cmake
find_package(clogx REQUIRED)
target_link_libraries(app PRIVATE clogx::clogx)
```

Or with pkg-config:

```bash
pkg-config --cflags --libs clogx
```

### Package Managers

#### vcpkg

Add `clogx` to your `vcpkg.json`:

```json
{
  "dependencies": [
    {
      "name": "clogx",
      "features": ["tls"]
    }
  ]
}
```

#### Conan 2.0

Install using `conanfile.py`:

```bash
conan install . --build=missing -o clogx/*:with_tls=True
```

## Configuration

The config file is YAML with all settings under a top-level `log:` mapping.
Pass the path to `log_init(path)`; when empty, defaults to `./config.yaml`.

For backward compatibility, old-style top-level `key: value` pairs (without a `log:` wrapper) are also accepted.

Example:

```yaml
log:
  level: TRACE
  async: false
  queue_size: 8192
  color: true
  format: "json"   # Use "json" for structured logging, or "[%time] [%level] %msg"
  time_format: "%Y-%m-%d %H:%M:%S"
  console_enable: true
  console_stderr: false
  file_enable: true
  file_path: logs/server.log
  max_size: 100MB
  backups: 10
  socket_enable: true
  socket_host: "127.0.0.1"
  socket_port: 9000
  socket_tls: true
  socket_tls_ca_file: "certs/ca.crt"
  socket_tls_skip_verify: false
```

| Key | Meaning |
|-----|---------|
| `level` | minimum output level: `TRACE` / `DEBUG` / `INFO` / `WARN` / `ERROR` / `FATAL` |
| `async` | enable background consumer thread when `true` |
| `queue_size` | async queue capacity |
| `color` | console ANSI coloring (does not affect file / socket) |
| `format` | format string, or `"json"` for single-line JSON structured logging |
| `time_format` | strftime template for `%time` (default: `%Y-%m-%d %H:%M:%S`); microseconds always appended after `.` |
| `console_enable` | enable console sink |
| `console_stderr` | when `true`, console sink writes to stderr instead of stdout |
| `file_enable` / `file_path` | file sink |
| `max_size` | rotation threshold: raw bytes, or `K`/`KB`, `M`/`MB`, `G`/`GB` |
| `backups` | number of backups to retain (`.1` … `.N`) |
| `socket_enable` / `socket_host` / `socket_port` | TCP socket sink |
| `socket_tls` | enable OpenSSL TLS encryption for socket sink (`socket_tls: true`) |
| `socket_tls_ca_file` / `tls_ca_file` | path to CA certificate file (optional) |
| `socket_tls_skip_verify` / `tls_skip_verify` | skip server certificate verification (`true`/`false`) |
| `rate_limit_enable` | enable global token bucket rate limiting (`true`/`false`) |
| `rate_limit_max_per_sec` | max allowed log messages per second (e.g. `1000`) |
| `rate_limit_burst` | maximum burst capacity (e.g. `100`) |

## Structured Logging (JSON)

Set `format: "json"` in configuration to emit structured JSON log lines:

```json
{"timestamp":"2026-07-28 14:44:25.123456","level":"INFO","module":"main","file":"app.c","line":42,"func":"main","thread":1234,"pid":5678,"tag":"","message":"Server started successfully"}
```

String fields (`message`, `file`, `func`, `module`, `tag`) are automatically escaped per RFC 8259 (`"`, `\`, `\n`, `\r`, `\t`, control bytes).

## Buffer Limits & Safety (`include/log_limits.h`)

Buffer capacities are centralized and macro-configurable:

| Macro | Default | Purpose |
|-------|---------|---------|
| `CLOG_MAX_MESSAGE_SIZE` | 4096 | formatted message body |
| `CLOG_MAX_FORMATTED_SIZE` | 8192 | formatted log line buffer |
| `CLOG_MAX_COLORED_SIZE` | 16384 | ANSI colorized output buffer |
| `CLOG_MAX_FORMAT_SIZE` | 1024 | format string template storage |
| `CLOG_MAX_PATH_SIZE` | 512 | file and socket path strings |

Override at compile time: `-DCLOG_MAX_MESSAGE_SIZE=8192`.

## Format Tokens (Text Mode)

| Token | Content |
|-------|---------|
| `%time` | local time `YYYY-MM-DD HH:MM:SS.uuuuuu` |
| `%level` | level name |
| `%msg` | message body |
| `%thread` | thread ID |
| `%pid` | process ID |
| `%file` / `%line` / `%func` | source location |
| `%module` / `%tag` | module (via `log_set_module`) and tag |
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
clogx_errno_t log_install_signal_handlers(void);
void          log_signal_handler(int sig);
int           log_get_pending_signal(void);
void          log_process_pending_signals(void);
log_config_t  *log_config_get(void);
int            log_config_set(const log_config_t *cfg);
log_sink_t    *socket_sink_create_tls(const char *host, int port, bool use_tls,
                                      const char *ca_file, bool skip_verify);

LOG_INFO("...");
LOG_DEBUG("...");
LOG_WARN("...");
LOG_ERROR("...");
LOG_FATAL("...");
LOG_TRACE("...");
```

*Note on Rate Limiter Performance*: When rate limiting is disabled (`rate_limit_enable: false`), a lock-free fast-path bypasses mutex overhead. When enabled, a mutex protects the token bucket calculations.

Installed public headers: `log.h`, `log_config.h`, `log_limits.h`, `log_record.h`, `log_sink.h` under `include/clogx/`.

## Fuzzing

AFL / libFuzzer test targets:

```bash
make fuzz-build      # builds build/fuzz_config and build/fuzz_formatter
make fuzz-config     # launches afl-fuzz on config parser
make fuzz-formatter  # launches afl-fuzz on log line formatter
```

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
