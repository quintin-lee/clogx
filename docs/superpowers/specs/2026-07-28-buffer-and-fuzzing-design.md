# Design Spec: Configurable Buffer Limits and AFL Fuzzing Integration

**Date**: 2026-07-28  
**Topic**: Buffer Limit Optimizations and AFL Fuzzing Integration  
**Status**: Approved  

---

## 1. Overview

This design addresses two key areas in `clog`:
1. **Configurable Buffer Limits**: Replaces hardcoded buffer sizes (`1024`, `2048`, `4096`, `512`, `256`) with centralized, macro-configurable buffer limits in a new header file `include/log_limits.h`, paired with safe truncation guarantees.
2. **AFL / libFuzzer Integration**: Adds dedicated fuzzing targets in `fuzz/` along with `Makefile` rules (`fuzz-build`, `fuzz-run`) to enable continuous fuzz testing of YAML configuration parsing and log line formatting logic.

---

## 2. Component & Interface Design

### 2.1 New Header: `include/log_limits.h`

Create `include/log_limits.h` defining configurable default limits wrapped in `#ifndef` guards so downstream projects or compiler flags can override them:

```c
#ifndef LOG_LIMITS_H
#define LOG_LIMITS_H

#ifndef CLOG_MAX_MESSAGE_SIZE
#define CLOG_MAX_MESSAGE_SIZE 4096
#endif

#ifndef CLOG_MAX_FORMATTED_SIZE
#define CLOG_MAX_FORMATTED_SIZE 8192
#endif

#ifndef CLOG_MAX_COLORED_SIZE
#define CLOG_MAX_COLORED_SIZE 16384
#endif

#ifndef CLOG_MAX_FORMAT_SIZE
#define CLOG_MAX_FORMAT_SIZE 1024
#endif

#ifndef CLOG_MAX_PATH_SIZE
#define CLOG_MAX_PATH_SIZE 512
#endif

#endif /* LOG_LIMITS_H */
```

### 2.2 Updating Subsystems with `log_limits.h`

1. **`include/log_config.h`**:
   - Includes `"log_limits.h"`.
   - Replaces `char file_path[256]` with `char file_path[CLOG_MAX_PATH_SIZE]`.
   - Replaces `char socket_host[256]` with `char socket_host[CLOG_MAX_PATH_SIZE]`.

2. **`core/log.c`**:
   - Includes `"log_limits.h"`.
   - Uses `char message[CLOG_MAX_MESSAGE_SIZE]` in `log_writevprintf`.
   - Clamps `vsnprintf` output and safely appends `...` if `ret >= CLOG_MAX_MESSAGE_SIZE`.

3. **`core/formatter.c`**:
   - Includes `"log_limits.h"`.
   - Replaces static array dimensions `g_default_format[512]` and `g_format_buf[512]` with `CLOG_MAX_FORMAT_SIZE`.
   - `append_token()` explicitly enforces remaining buffer space checks to ensure zero-out-of-bounds writing.

4. **`core/dispatcher.c`**:
   - Includes `"log_limits.h"`.
   - Uses `formatted_buf[CLOG_MAX_FORMATTED_SIZE]` and `colored_buf[CLOG_MAX_COLORED_SIZE]`.
   - Gracefully falls back to plain `formatted_buf` if ANSI color rendering output exceeds `CLOG_MAX_COLORED_SIZE`.

---

## 3. AFL / Fuzzing Architecture

### 3.1 Fuzz Targets (`fuzz/`)

1. **`fuzz/fuzz_config.c`**:
   - Accepts input buffer from `stdin` or AFL file input.
   - Writes input to a temporary config file or feeds it to YAML parsing routines (`log_config_init`).
   - Cleans up and destroys config state (`log_destroy`) after each run.

2. **`fuzz/fuzz_formatter.c`**:
   - Reads input buffer from `stdin`.
   - Splices input into a `log_record_t` structure and format string.
   - Calls `log_formatter_format` to fuzz token replacement against arbitrary binary/malformed format strings.

3. **Seed Corpus (`fuzz/seeds/`)**:
   - `fuzz/seeds/config_valid.yaml`: Standard log YAML configuration file.
   - `fuzz/seeds/config_nested.yaml`: Complex nested YAML structure.
   - `fuzz/seeds/formatter_sample.txt`: Format strings with various `%` tokens.

### 3.2 `Makefile` Integration

- Target `fuzz-build`: Compiles `fuzz/fuzz_config.c` and `fuzz/fuzz_formatter.c` using `afl-gcc` or `clang -fsanitize=fuzzer,address`.
- Target `fuzz-config`: Runs `afl-fuzz -i fuzz/seeds -o fuzz/out_config -- ./build/fuzz_config @@`.
- Target `fuzz-formatter`: Runs `afl-fuzz -i fuzz/seeds -o fuzz/out_formatter -- ./build/fuzz_formatter @@`.

---

## 4. Verification Plan

1. **Unit Tests**:
   - Run existing test suite `make clean && make test` to ensure zero regression.
   - Add new boundary tests in `tests/test_boundary_config.c` testing messages longer than 1024/4096 bytes and path sizes.
2. **Sanitizers**:
   - Run `make test-asan` and `make test-ubsan` to verify memory safety under AddressSanitizer and UndefinedBehaviorSanitizer.
3. **Fuzzing Harness Check**:
   - Build `fuzz-build` and run 100+ iterations with seed input to confirm harnesses operate without immediate crashes.
