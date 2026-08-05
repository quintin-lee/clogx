# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Added
- `scripts/check_version_consistency.sh` version-reference consistency checker, wired into CI to verify `VERSION`, `include/clogx_version.h`, `vcpkg.json`, the Makefile fallback, and the `CHANGELOG.md` heading stay in sync
- CI shell-syntax check job (`bash -n`) for `scripts/*.sh`
- Locked-ABI export surface: `clogx.map` (GNU/ELF `CLOGX_0_2` version script) and `clogx.exports` (macOS export whitelist) pin the 67-symbol shared-library export set, wired into both Makefile and CMake shared builds
- `scripts/check_abi_exports.sh` dual-direction ABI export checker, run in `make check` and the new `abi-exports` CI job
- `make tsan` target and CMake `CLOG_ENABLE_TSAN` option to build and run the full test suite under ThreadSanitizer, with `tsan.supp` suppression file
- `tsan` CI job (ubuntu + macOS) running the TSan suite
- MSVC `CLOGX_API` `__declspec(dllexport)`/`dllimport` branch in `log.h` for Windows shared builds
- ABI Stability policy documented in `docs/CONTRIBUTING.md`

### Changed
- `scripts/release.sh` pre-flight safety checks: refuses to release from a dirty worktree or when local HEAD diverges from `origin/master` (fetching the remote first), and prints rollback hints when a run aborts partway
- Documented the release workflow (bump rules, safety checks, manual GitHub Release creation) in `docs/CONTRIBUTING.md`
- Makefile and CMake now pass a version script (`--version-script` / `-exported_symbols_list`) and soname when building the shared library, so only `CLOGX_API` symbols are exported

### Fixed
- SIGPIPE default-ignore: `log_install_signal_handlers()` now sets `SIG_IGN` for `SIGPIPE` on POSIX (saves/restores previous disposition), preventing process crashes when socket sinks encounter broken pipes. New test: `tests/test_sigpipe.c`
- Rotate error propagation: `file_rotate_file()` now returns `-1` for invalid args, `-EACCES` for permission denied, and `-ENOENT` for missing active file. `file_sink.c` checks the return value and logs a warning before retrying reopen instead of silently ignoring failures
- Windows build: guard `test_sigpipe.c` POSIX-only signal-handling code with `#if defined(_WIN32) || defined(_WIN64)` to prevent undefined `sigaction`/`SIGPIPE` compilation errors on MSVC
- macOS TSan build: remove `CLOGX_API` from internal mutex declarations in `clog_port.h` TSan block — `CLOGX_API` is not defined in that header, causing "unknown type name 'CLOGX_API'" on macOS when TSan is enabled
- Mermaid CI: fix npm global path resolution by exporting `NODE_PATH` from `npm root -g` in CI step and rewriting `scripts/check_mermaid.js` to resolve the npm prefix dynamically (NODE_PATH → require.resolve fallback → common system paths)

### Fixed
- Cross-compiler build error in `include/clog_port.h`: GCC failed with `missing binary operator before token "("` when evaluating `__has_feature(thread_sanitizer)` in preprocessor condition. Defined fallback `#ifndef __has_feature #define __has_feature(x) 0 #endif` macro for non-Clang compilers
- Data race in the Prometheus exporter: the HTTP worker thread read `g_prom_running` / `g_prom_server_fd` without holding `g_prom_mutex`, while `start()`/`stop()` wrote them under the lock. The loop now snapshots the shared state under the mutex before blocking on `accept()` and re-checks after an invalid socket, eliminating the unsynchronized read (visible under ThreadSanitizer)
- ThreadSanitizer data race in `log_dispatcher_dispatch_for` on macOS: the `CLOG_MUTEXGUARDED` macro uses `__attribute__((cleanup))` for automatic unlock, which macOS arm64 TSan does not properly recognize as a mutex release. Replaced with explicit `clog_mutex_lock`/`clog_mutex_unlock` calls (same pattern used by the Prometheus exporter fix), eliminating the false-positive race on `file_sink.c:112` (`current_size += written`)
- Documentation: `signal_handler.c` file header overstated signal coverage — claimed `SIGHUP`/`SIGUSR1`/`SIGUSR2` are installed (only `SIGTERM`/`SIGINT` are) and referenced a non-existent `signal_monitor_thread` (signal processing is inline in the write path). Fixed to match actual implementation; corrected Windows fallback description
- Documentation: `README.md` public API listing referenced `logger_writevprintf_internal` (a `static` internal function) instead of `logger_writevprintf` (the exported public API entry point called by `LOGGER_*` macros)
- Documentation: `docs/user_manual.md` §6.1 incorrectly described async queue overflow as "falls back to synchronous logging" — the actual behavior is message drop + fallback callback; §7.3 similarly referenced `logger_writevprintf_internal` instead of `logger_writevprintf`
- Documentation: `docs/CONTRIBUTING.md` and `CHANGELOG.md` attributed `CLOGX_API` to `clog_port.h`; it is actually defined in `log.h` with `#ifndef` re-export guards in other public headers
- Documentation: `include/log.h` `logger_reload()` docstring referenced "SIGHUP signal handler" ambiguously; clarified to "caller-installed SIGHUP handler" since clogx does not install one
- Documentation: `include/log_sink.h` `socket_sink_create` docstring had a missing colon ("Note Requires" → "Note: Requires")

## [0.2.1] - 2026-08-02

### Added
- Native Structured Key-Value (KV) Logging API (`LOG_INFO_KV`, `LOGGER_INFO_KV`, `log_write_kv`, `logger_write_kv`) with typed attribute constructors (`CLOG_KV_INT`, `CLOG_KV_UINT`, `CLOG_KV_FLOAT`, `CLOG_KV_STR`, `CLOG_KV_BOOL`) for high-efficiency structured JSON and text log attribute serialization
- Doxygen API automated documentation generation configuration (`Doxyfile`) and GitHub Actions workflow (`.github/workflows/deploy-docs.yml`) for automatic deployment to GitHub Pages
- Configurable socket connection and TLS handshake timeout (`socket_connect_timeout_ms` YAML key / `connect_timeout_ms` in `socket_writer_config_t` and `socket_sink_create_async_ex`) for non-blocking socket connect and `SSL_connect` select loops
- Winsock `WSACleanup()` pair calls in `socket_destroy()`, `socket_writer_cleanup()`, and `prometheus_exporter_stop()` to guarantee 1:1 match with `clog_net_init()` (`WSAStartup`) and prevent socket handle reference leaks on Windows
- Fast Integer-to-ASCII and timestamp formatting utilities (`core/fast_ascii.h`) utilizing a branch-prediction-friendly 2-digit lookup table (LUT) for `uint32_t`, `int32_t`, fixed 6-digit microsecond padding, and 19-char ISO 8601 datetimes (`YYYY-MM-DD HH:MM:SS`), accompanied by unit tests in `tests/test_fast_ascii.c`
- Fast hex lookup table for trace ID and span ID hex encoding in `core/formatter.c`

### Changed
- Replaced `snprintf` format-string parsing overhead in `core/formatter.c` (`FMT_OP_THREAD`, `FMT_OP_PID`, `FMT_OP_LINE`, `format_json_ex`, `format_otel_json`) and `format_sec_cached` string copies with LUT 2-digit Fast-ASCII conversion and direct memory copies, improving raw logging throughput by ~86%
- Format-string precompilation cache: the pattern compiler (`fmt_compile`) output depends only on the format string, but was re-run on every log call. `core/formatter.c` now keeps a thread-local compiled-opcode cache keyed by the format string — the hot path executes the opcode program directly and only recompiles when the format actually changes (e.g. after `log_reload`). Literal segments reference a private copy inside the cache, so the program is self-contained regardless of the caller's format buffer lifetime
- Lock-free MPSC ring buffers: `core/queue.c` (`mpsc_queue_t`) and `core/socket_async.c` (`socket_ring_buffer_t`) now use atomic CAS on `head` for producer slot claiming instead of mutex-protected critical sections. The producer fast path (`try_put`) is fully lock-free — no mutex is acquired. Consumer blocking uses a semaphore (`items_sem`) instead of condition variables, and a lightweight `drain_mutex` + `drain_cond` pair is retained only for `wait_empty` during shutdown. Capacity is rounded to a power of two for fast bitwise modulo. Each slot carries a per-slot sequence counter (`seq`): the producer release-stores `seq = position + 1` after writing the record, and the consumer acquire-loads `seq` before reading, so records are only consumed after their write is fully visible.
- Centralized platform adaptation code: `get_timestamp()` / `get_thread_id()` from `core/log.c`, `get_now_ms()` from `core/rate_limit.c`, and Windows VT mode helper from `sinks/console_sink.c` now live in `include/clog_port.h`
- Default log format changed from `%msg` to `[%time] [%level] %msg`
- `console_sink_create` / `console_sink_create_stderr` now share a unified internal helper; public API unchanged
- `conanfile.py` reads package version dynamically from `VERSION` file via `set_version()`

### Fixed
- Removed redundant `#include "log.h"` from `core/plugin_loader.c` POSIX section; `log_config.h` is the minimal required header
- `core/prometheus_exporter.c` no longer includes redundant platform socket headers; `WSAStartup` replaced with existing `clog_net_init()`
- Windows plugin loader stubs now include `clog_port.h` for consistency
- Data race in lock-free ring buffers: the consumer could read a slot between the producer's `head` CAS and the record write (a preempted producer), consuming half-written records. Per-slot sequence numbers now gate reads on publication (visible under ThreadSanitizer; `test_queue_try_put` concurrency test failed reliably before the fix)
- `core/socket_async.c`: producer no longer rolls `head` back when the line-copy `malloc` fails — in a concurrent setting that decrement could reclaim another producer's claimed slot and corrupt the ring. The slot is published empty and counted as dropped instead
- Centralized GCC/Clang `__atomic_*` builtins behind `clog_atomic_load_int` / `clog_atomic_store_int` / `clog_atomic_load_u64` / `clog_atomic_store_u64` in `include/clog_port.h` with MSVC `Interlocked*` equivalents so `core/queue.c` and `core/socket_async.c` compile on Windows; zero-initialize `log_record_t` in the write path (`core/log.c`) to avoid reading uninitialized `kv_count`
- macOS CI: POSIX unnamed semaphores are unimplemented on macOS (`sem_init()` always returns `ENOSYS`), which broke `mpsc_queue_create` and `ring_create` and failed every async test. `clog_sem_*` in `include/clog_port.h` now uses fork-safe named semaphores (`sem_open`) on `__APPLE__` — replacing an earlier GCD `dispatch_semaphore_*` attempt that is not fork-safe (child processes would deadlock on the semaphore after `fork()`)
- `logger->async_processing` flag accessed via `clog_atomic_load_int` / `clog_atomic_store_int` instead of plain volatile reads/writes, closing a cross-thread data race between the async worker and `log_async_flush_for`
- JSON and OTel renderers guarantee well-formed output under buffer overflow: when a string field exhausts the line buffer, the line is closed with a valid suffix (`"}` for the JSON root, `"}}` for the OTel attributes object) instead of emitting truncated invalid JSON; a truncating KV attribute closes the object the same way, and a line that cannot even be closed is dropped
- Lost-wakeup deadlock in `socket_ring_get_batch` (`core/socket_async.c`): after `socket_ring_close()` the wake-up post could be consumed by a first `get_batch` returning remaining items, so a second `get_batch` on the now-empty closed ring blocked on the semaphore forever — hanging `socket_writer_stop()` (its final drain calls `get_batch` again) and the `test_socket_async` CI run on macOS and Windows (120s ctest timeout). `get_batch` now terminates immediately when the ring is closed and empty, while still draining remaining items before shutdown
- Release CI (`release.yml` verify matrix): CMake Release builds define `NDEBUG`, which stripped every `assert(...)` — and with it the side-effect expressions inside them (e.g. `assert(socket_ring_put(...) == 0)` never called `put`), leaving the semaphore unposted and `test_socket_async` blocked forever on macOS/Windows (and any Release build). `CMakeLists.txt` now passes `-UNDEBUG` to every test target so asserts stay active in all build types
- Async flush race: the worker set `async_processing` *after* dequeuing a batch, so `log_async_flush_for()` could observe "queue empty && !async_processing" while a batch was still being processed and return before those records reached the sinks — losing the final records when the process exited right after `log_flush()`. `mpsc_queue_get_batch` is split into `mpsc_queue_wait_for_items` + `mpsc_queue_get_batch_try`, and the worker marks the batch in-flight *before* draining (fixes flaky `test_signal_handler` on Linux CI)
- `make test` idempotency: `logs/` output is cleaned before each test run so re-running the suite never fails on stale rotation backups
- Compiler warning cleanup in `core/fast_ascii.h` LUTs and TLS test builds (`-Wall -Wextra -Wconversion` clean)
- Trace-context coverage: tests now exercise async queue-full fallback, rate-limit suppression reporting, and `%trace_id`/`%span_id` formatting paths (branch coverage 97.8%)
- Windows MSVC build failure in `tests/test_coverage_deep.c`: the signal-handler coverage segment used POSIX-only `SIGUSR1` (undeclared under MSVC, C2065). The segment and its `g_caught_sig`/`test_sig_catcher` helpers are now guarded with `#ifndef _WIN32`, matching the file's existing POSIX-only sections. Also casts `send`/`recv` length arguments to `clog_sock_size_t` in `tests/test_prometheus.c` to silence MSVC C4267 `size_t`→`int` warnings

### Deprecated
- Old `log_console_sink_create(bool stderr, bool color)` name referenced in `docs/user_manual.md` updated to current `console_sink_create` / `console_sink_create_stderr` API

## [0.2.0] - 2026-07-30

### Added
- OTLP Sink: native OpenTelemetry Protocol export via `otlp_sink_create(endpoint, service_name)` — JSON log records over HTTP, added programmatically with `log_add_sink()` (no YAML keys)
- Prometheus Metrics Exporter: `clog_prometheus_exporter_start(port)` / `clog_prometheus_exporter_stop()` / `clog_prometheus_render_metrics(buf, size)` exposing per-level counters, queue depth, and dropped/suppressed gauges as Prometheus text format on HTTP `/metrics` (enable via `prometheus_enable` / `prometheus_port` YAML keys)
- Plugin ABI (`include/clogx_plugin.h`): `log_plugin_load(so_path)` / `log_plugin_unload(h)` / `log_plugin_create_sink(h, params_json)` / `log_plugin_scan(dir, out, max)` — load `.so` sink modules at runtime via `dlopen`, with ABI version checks (`CLOGX_PLUGIN_ABI_VERSION`); plugins export `clogx_plugin_desc()` (metadata) and `clogx_plugin_create()` (factory) symbols
- Multi-Instance Logger (`logger_t`): independent `logger_create()` / `logger_destroy()` / `LOGGER_INFO()` etc. — each instance owns its own config, sinks, module, and async worker, isolated from the global default logger
- Coverage Gap Test Suite (`tests/test_coverage_gaps.c`): 9 tests targeting untaken code paths (strerror codes, config parse failure, no-sinks init, async fallback, signal-in-write, thread context update, logger_reload, custom format/time_format)
- Millisecond-Precision Token Bucket Rate Limiter (`core/rate_limit.c`): optimized token replenishment at millisecond granularity (`g_fill_rate = max_per_sec / 1000.0`), eliminating sub-millisecond floating-point arithmetic overhead on hot paths
- POSIX Self-Pipe Signal Handler (`core/signal_handler.c` & `log_get_signal_fd()`): zero-lock signal safety implementation writing signal bytes to a non-blocking pipe (`O_NONBLOCK` | `FD_CLOEXEC`), eliminating all mutex/lock operations inside signal handlers and providing `log_get_signal_fd()` for event loop integration
- POSIX Shell & AWK C-Source Branch Coverage Tool (`scripts/gcov_branch_summary.sh`): zero-dependency bash/awk script parsing `gcov -b` output, calculating true C-source AST branch execution rate (**96.80%** overall across core C files with 8 files at 100%), integrated into Makefile (`make coverage-gcov`), CMake (`coverage-gcov` target), and GitHub Actions CI workflow with threshold enforcement (75%)
- Deep Edge-Case & Error-Path Test Suite (`tests/test_coverage_deep.c`): comprehensive test suite covering sink write failures, async queue overflow/fallback, ultra-long format strings, socket connection failures, TLS error paths, rate limiter token exhaustion and replenishment, invalid YAML syntax errors, formatter truncation loops (1..80 bytes), active signal processing (`SIGTERM`/`SIGINT` `SIG_IGN` trick), and snapshot lifecycle
- LCOV Exclusion Annotations for System Failure Paths: added `LCOV_EXCL_START/STOP` tags to system-level error handling branches (`sigaction`, `realloc`, `clog_thread_create`) requiring OS-level fault injection
- Automated Performance Benchmark CI Workflow (`.github/workflows/benchmark.yml`): weekly schedule and manual trigger running throughput and async vs sync benchmarks, rendering throughput summary to GitHub Step Summary
- Focused Fuzzing Harness (`fuzz/fuzz_pipeline.c`): AFL/libFuzzer test harness targeting formatting, string truncation, and boundary conditions, with dedicated GitHub Actions CI fuzz verification job (`make fuzz-build && echo test | ./build/fuzz_config /dev/stdin`)
- `clang-tidy` Static Analysis Integration: Makefile targets `make tidy` and `make check-tidy` (integrated into `make check`), CMake option `CLOG_ENABLE_CLANG_TIDY=ON`, and CMake custom target `tidy` with a zero-warning policy
- MSVC RAII Mutex Guard: `CLOG_MUTEXGUARDED` macro updated with `__try / __finally` for MSVC (`_MSC_VER`), ensuring safe mutex release across Windows builds

### Fixed
- `clang-analyzer-valist.Uninitialized` false positive: suppressed in `.clang-tidy` and added NOLINT annotation to `vsnprintf` in `core/log.c`
- Windows MSVC CTest process hang in `test_coverage_deep`: guarded POSIX signal handler and `pthread_atfork` tests with `#ifndef _WIN32` and set `catch_signals: false` in temporary test config
- AddressSanitizer 64-byte memory leak in `test_coverage_deep`: added explicit `c_err->destroy(c_err)` call after `log_remove_sink(c_err)`

## [0.1.0] - 2026-07-29

### Added
- Native POSIX Syslog Sink: `syslog_sink_create(ident, facility)` mapping log levels to syslog priorities (`LOG_DEBUG`, `LOG_INFO`, `LOG_WARNING`, `LOG_ERR`, `LOG_CRIT`) with automatic syslog re-connection in child processes after `fork()`
- Thread-Local Mapped Diagnostic Context (MDC): `log_set_thread_context(key, val)`, `log_get_thread_context(key)`, and `log_clear_thread_context()` auto-injecting top-level JSON fields
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

[Unreleased]: https://github.com/quintin-lee/clogx/compare/v0.2.1...HEAD
[0.2.1]: https://github.com/quintin-lee/clogx/releases/tag/v0.2.1
[0.2.0]: https://github.com/quintin-lee/clogx/releases/tag/v0.2.0
[0.1.0]: https://github.com/quintin-lee/clogx/releases/tag/v0.1.0
