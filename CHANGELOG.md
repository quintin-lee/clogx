# Changelog

All notable changes to this project will be documented in this file.

## [0.1.0] - 2026-07-29

### Added
- Focused Fuzzing Harness (`fuzz/fuzz_pipeline.c`): AFL/libFuzzer test harness targeting formatting, string truncation, and boundary conditions
- `clang-tidy` Static Analysis Integration: Makefile targets `make tidy` and `make check-tidy` (integrated into `make check`), CMake option `CLOG_ENABLE_CLANG_TIDY=ON`, and CMake custom target `tidy` with a zero-warning policy
- MSVC RAII Mutex Guard: `CLOG_MUTEXGUARDED` macro updated with `__try / __finally` for MSVC (`_MSC_VER`), ensuring safe mutex release across Windows builds
- Branch Coverage Boost: expanded edge-case branch tests in `test_coverage_boost.c` reaching >80.08% overall branch coverage (>91% across core library files)
- Native POSIX Syslog Sink: `syslog_sink_create(ident, facility)` mapping log levels to syslog priorities (`LOG_DEBUG`, `LOG_INFO`, `LOG_WARNING`, `LOG_ERR`, `LOG_CRIT`) with automatic syslog re-connection in child processes after `fork()`
- Thread-Local Mapped Diagnostic Context (MDC): `log_set_thread_context(key, val)`, `log_get_thread_context(key)`, and `log_clear_thread_context()` supporting `%context` format tokens and auto-injecting top-level JSON fields
- Operational Observability Metrics API: `log_get_stats(&stats)` retrieving runtime counters (`total_logged_count`, `dropped_queue_full_count`, `suppressed_rate_count`, `current_queue_depth`)
- High Performance Async Queue Batching: worker thread dequeues up to 64 records per batch (`mpsc_queue_get_batch`) and flushes sinks once per batch to minimize mutex contention and I/O syscalls
- Graceful Shutdown (`SIGTERM`/`SIGINT` handling): POSIX `sigaction` signal handlers
  via `log_install_signal_handlers()`, `log_signal_handler(sig)`, and YAML
  setting `catch_signals: true` to flush pending async/sync logs before process exit
- POSIX `pthread_atfork` process safety: registers fork handlers to lock internal
  mutexes during `fork()`, release them in parent/child, and safely re-initialize
  `mpsc_queue` condition variables and restart the async worker thread in the child
- Built-in Token Bucket Rate Limiter: global throttling via `rate_limit_enable`,
  `rate_limit_max_per_sec`, and `rate_limit_burst` YAML settings with automatic
  suppression notice logging when tokens replenish

### Changed
- Migrated Token Bucket Rate Limiter time source from `gettimeofday` (`CLOCK_REALTIME`)
  to POSIX `CLOCK_MONOTONIC` to ensure immunity against NTP clock adjustments
- Official package manager integration: `vcpkg.json` manifest with `tls` and
  `system-yaml` feature flags; Conan 2.0+ recipe (`conanfile.py`) supporting
  `shared`, `with_tls`, and `with_yaml` options
- Native JSON structured logging: setting `format: "json"` (or `format: "JSON"`)
  emits single-line JSON log objects with RFC 8259 string escaping for quotes,
  backslashes, newlines, and control bytes
- Optional Socket sink OpenSSL TLS encryption: `socket_tls`, `socket_tls_ca_file`,
  `socket_tls_skip_verify` YAML options and `socket_sink_create_tls()` factory
  guarded by `#ifdef CLOG_USE_TLS`; supported via Makefile (`make TLS=1`) and
  CMake (`CLOG_ENABLE_TLS=ON`)
- Centralized buffer limit configuration: `include/log_limits.h` defines
  `CLOG_MAX_MESSAGE_SIZE` (4096), `CLOG_MAX_FORMATTED_SIZE` (8192),
  `CLOG_MAX_COLORED_SIZE` (16384), `CLOG_MAX_FORMAT_SIZE` (1024), and
  `CLOG_MAX_PATH_SIZE` (512) with `#ifndef` guards for compile-time override
- AFL / libFuzzer fuzzing targets: `fuzz/fuzz_config.c` and `fuzz/fuzz_formatter.c`
  harnesses with Makefile targets `fuzz-build`, `fuzz-config`, `fuzz-formatter`
- Programmatic configuration API: `log_config_set()` applies a caller-provided
  `log_config_t` directly; `log_config_get()` returns the current config
- Configurable time format: `time_format` key sets the strftime template for
  `%time` (default `%Y-%m-%d %H:%M:%S`); microseconds always appended after `.`
- Per-sink log level filtering: `log_sink_set_level()` / `log_sink_get_level()`
- Printf format attribute on `log_writevprintf` for compile-time format string
  validation (`CLOGX_PRINTF_FMT` portability macro)
- Symbol visibility control: `CLOGX_API` macro (`__attribute__((visibility("default")))`)
  on all public functions, `-fvisibility=hidden` for shared lib — ABI exports
  only public API symbols
- `restrict` qualifiers on hot-path internal function parameters (formatter,
  queue, dispatcher, async) for compiler aliasing optimization
- Makefile convenience targets: `make asan`, `make ubsan`, `make check`
  (format → build → test), `make test-valgrind`
- Shared library build: `libclogx.so` with soname versioning via both Makefile
  and CMake (`CLOG_BUILD_SHARED=ON`)
- CMake OBJECT library: shared-build internal-symbol tests link directly with
  objects, avoiding symbol-resolution failures
- Backward compatibility: old-style top-level `key: value` YAML (without `log:`
  wrapper) is accepted alongside the new nested format
- Boundary condition tests: empty YAML, comment-only YAML, old-style top-level
  keys, unknown keys, NULL path, and invalid numeric values
- Example program extended with `log_config_set` demonstration

### Changed
- Config parser migrated from hand-rolled key:value to libyaml event API; all
  settings now live under a top-level `log:` mapping in YAML
- Config key lookup optimized from O(n) `strcmp` chain to O(log n) binary search
  via `bsearch` on a sorted static key table
- Move formatting and color computation outside the global dispatcher lock to
  reduce lock contention in multi-threaded sync mode
- CMakeLists.txt: parallel test targets matching Makefile, `-fvisibility=hidden`
  for shared build
- CI: clang-format check step, expanded Valgrind coverage to all 23 tests

### Fixed
- macOS (Apple Silicon arm64 / Clang) linker failure: removed `__gcov_dump()` reference in `signal_handler.c` to prevent Mach-O `ld64` undefined symbol errors
- ASan test build preload: dynamically query `libasan.so` via `gcc -print-file-name=libasan.so` and isolate preload to test execution phase to prevent compiler binary pollution
- Replace `atoi` with `strtol` for robust numeric parsing of `queue_size`,
  `backups`, and `port` settings with full error validation
- Add `uint64_t` overflow protection for `max_size` parsing
- Replace all remaining `strncpy` with `snprintf` for safe null-terminated
  string copy (7 instances across `config.c`, `log.c`, `formatter.c`)
- `-Wconversion` warnings across all 10 `.c` files (formatter `remaining -= ret`
  size_t underflow, dispatcher sign-conversion casts, socket port/send types)
- `85%` printf format bug in example exposed by format attribute validation
- Doxygen 6 warnings (enum value references, static inline doc)

### Build
- libyaml auto-download: CMake uses FetchContent; Makefile downloads and
  compiles libyaml sources when pkg-config is unavailable — no system package
  required
- New CMake option `CLOG_USE_SYSTEM_YAML=ON` to force using system libyaml
  instead of auto-downloading
- Makefile install/uninstall: install library, public headers, pkg-config `.pc`
- ASan / UBSan build and test targets (`make asan`, `make ubsan`, `make test-*`)
- `make check`: runs clang-format → clean build → all tests in sequence
- `make test-valgrind`: all 23 tests under Valgrind (gracefully skipped if
  valgrind not installed)

## [0.1.0] - 2024

### Added
- Core logging API with six levels: TRACE, DEBUG, INFO, WARN, ERROR, FATAL
- Configurable log format with token-based formatter (`%time`, `%level`,
  `%msg`, `%module`, `%file`, `%line`, `%func`)
- Synchronous and asynchronous logging modes
- Lock-free MPSC queue for async mode with non-blocking try-put and sync
  fallback on full queue
- Multiple sink backends: console (stdout/stderr with ANSI color), file
  (with size-based rotation), and TCP socket
- Config parser with hot-reload support (key:value format)
- Thread-safe design: per-module rwlock-protected config, atomic snapshot
  swap on reload, deep-copy log records for async
- File rotation with configurable max size and backup count
- Auto-creation of parent directories for log file paths
- Runtime dynamic sink management: `log_add_sink`, `log_remove_sink`
- Structured error codes with `log_strerror()`
- Async fallback callback notification when queue is full
- Configurable console output to stderr (`console_stderr: true`)

### Build
- CMake and Makefile build systems
- pkg-config support for installed library
- CI with GitHub Actions (build, ASan, Valgrind, cppcheck)
- Doxygen API documentation generation
- 17 test cases covering lifecycle, reload, rotation, config validation,
  async fallback, and error paths

[Unreleased]: https://github.com/ohmyopenclog/clogx/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/ohmyopenclog/clogx/releases/tag/v0.1.0
