# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Added
- Programmatic configuration API: `log_config_set()` applies a caller-provided
  `log_config_t` directly; `log_config_get()` returns the current config
- Configurable time format: `time_format` key sets the strftime template for
  `%time` (default `%Y-%m-%d %H:%M:%S`); microseconds always appended after `.`
- Per-sink log level filtering: `log_sink_set_level()` / `log_sink_get_level()`
- Printf format attribute on `log_writevprintf` for compile-time format string
  validation (`CLOGX_PRINTF_FMT` portability macro)
- Symbol visibility control: `CLOGX_API` macro (`__attribute__((visibility("default")))`)
  on all 24 public functions, `-fvisibility=hidden` for shared lib — ABI exports
  only public API symbols
- `restrict` qualifiers on hot-path internal function parameters (formatter,
  queue, dispatcher, async) for compiler aliasing optimization
- Makefile convenience targets: `make asan`, `make ubsan`, `make check`
  (format → build → test), `make test-valgrind`
- Shared library build: `libclogx.so` with soname versioning via both Makefile
  and CMake (`CLOG_BUILD_SHARED=ON`)
- CMake OBJECT library: shared-build internal-symbol tests link directly with
  objects, avoiding symbol-resolution failures

### Changed
- Config parser migrated from hand-rolled key:value to libyaml event API; all
  settings now live under a top-level `log:` mapping in YAML
- Move formatting and color computation outside the global dispatcher lock to
  reduce lock contention in multi-threaded sync mode
- CMakeLists.txt: parallel test targets matching Makefile, `-fvisibility=hidden`
  for shared build
- CI: clang-format check step, expanded Valgrind coverage to all 21 tests

### Fixed
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
- Makefile install/uninstall: install library, public headers, pkg-config `.pc`
- ASan / UBSan build and test targets (`make asan`, `make ubsan`, `make test-*`)
- `make check`: runs clang-format → clean build → all tests in sequence
- `make test-valgrind`: all 22 tests under Valgrind (gracefully skipped if
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
