# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Fixed
- Replace `atoi` with `strtol` for robust numeric parsing of `queue_size`,
  `backups`, and `port` settings with full error validation
- Add `uint64_t` overflow protection for `max_size` parsing
- Replace all remaining `strncpy` with `snprintf` for safe null-terminated
  string copy (7 instances across `config.c`, `log.c`, `formatter.c`)

### Changed
- Move formatting and color computation outside the global dispatcher lock to
  reduce lock contention in multi-threaded sync mode

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
