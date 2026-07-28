# Buffer Limits and AFL Fuzzing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Centralize buffer limit definitions into `include/log_limits.h` with safe boundary handling, and integrate AFL/libFuzzer fuzzing harnesses into `fuzz/` with Makefile targets.

**Architecture:** Define `#ifndef` macro constants in `include/log_limits.h` used by `log_config.h`, `log.c`, `formatter.c`, and `dispatcher.c`. Add standalone fuzz harnesses in `fuzz/` for config parsing and line formatting.

**Tech Stack:** C99, POSIX pthread, libyaml, AFL / libFuzzer (clang / afl-gcc).

---

### Task 1: Define `include/log_limits.h` and Update `include/log_config.h`

**Files:**
- Create: `include/log_limits.h`
- Modify: `include/log_config.h:9-47`
- Test: `tests/test_boundary_config.c`

- [ ] **Step 1: Create `include/log_limits.h`**

```c
/**
 * @file log_limits.h
 * @brief Configurable maximum buffer size constants.
 */

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

- [ ] **Step 2: Update `include/log_config.h` to include `log_limits.h` and use `CLOG_MAX_PATH_SIZE`**

Include `"log_limits.h"` in `include/log_config.h` and replace `char file_path[256]` and `char socket_host[256]` with `char file_path[CLOG_MAX_PATH_SIZE]` and `char socket_host[CLOG_MAX_PATH_SIZE]`.

- [ ] **Step 3: Verify build**

Run: `make clean && make test`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add include/log_limits.h include/log_config.h
git commit -m "feat: introduce log_limits.h for configurable buffer limits"
```

---

### Task 2: Refactor `core/log.c`, `core/formatter.c`, and `core/dispatcher.c` to use `log_limits.h`

**Files:**
- Modify: `core/log.c:13-145`
- Modify: `core/formatter.c:11-180`
- Modify: `core/dispatcher.c:9-138`
- Test: `tests/test_boundary_config.c`

- [ ] **Step 1: Write boundary test in `tests/test_boundary_config.c` for oversized log messages**

Add a test in `tests/test_boundary_config.c` that writes a message longer than 2000 characters and verifies it formats without memory errors or out-of-bounds crashes.

- [ ] **Step 2: Update `core/log.c` to use `CLOG_MAX_MESSAGE_SIZE`**

Include `"log_limits.h"` in `core/log.c`. Replace `char message[1024]` with `char message[CLOG_MAX_MESSAGE_SIZE]` in `log_writevprintf`. Update truncation logic to handle `CLOG_MAX_MESSAGE_SIZE`.

- [ ] **Step 3: Update `core/formatter.c` to use `CLOG_MAX_FORMAT_SIZE`**

Include `"log_limits.h"` in `core/formatter.c`. Replace `g_default_format[512]` and `g_format_buf[512]` with `CLOG_MAX_FORMAT_SIZE`.

- [ ] **Step 4: Update `core/dispatcher.c` to use `CLOG_MAX_FORMATTED_SIZE` and `CLOG_MAX_COLORED_SIZE`**

Include `"log_limits.h"` in `core/dispatcher.c`. Replace `formatted_buf[2048]` with `CLOG_MAX_FORMATTED_SIZE` and `colored_buf[4096]` with `CLOG_MAX_COLORED_SIZE`. Fall back to uncolored output if `colored_buf` size exceeds `CLOG_MAX_COLORED_SIZE`.

- [ ] **Step 5: Run tests and verify**

Run: `make clean && make test`
Expected: PASS

- [ ] **Step 6: Commit**

```bash
git add core/log.c core/formatter.c core/dispatcher.c tests/test_boundary_config.c
git commit -m "refactor: apply log_limits.h buffer sizes across logging pipeline"
```

---

### Task 3: Add AFL / Fuzzing Targets in `fuzz/` and Update `Makefile`

**Files:**
- Create: `fuzz/fuzz_config.c`
- Create: `fuzz/fuzz_formatter.c`
- Create: `fuzz/seeds/valid_config.yaml`
- Modify: `Makefile`

- [ ] **Step 1: Create `fuzz/fuzz_config.c`**

Create `fuzz/fuzz_config.c` reading from `stdin` or AFL input file to test `log_config_init` and parser resilience.

```c
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "log.h"
#include "log_config.h"

int main(int argc, char **argv) {
    const char *filepath = "fuzz_tmp_config.yaml";
    if (argc > 1) {
        filepath = argv[1];
    } else {
        FILE *f = fopen(filepath, "wb");
        if (!f) return 0;
        char buf[1024];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0) {
            fwrite(buf, 1, n, f);
        }
        fclose(f);
    }

    log_init(filepath);
    log_destroy();

    if (argc <= 1) {
        unlink(filepath);
    }
    return 0;
}
```

- [ ] **Step 2: Create `fuzz/fuzz_formatter.c`**

Create `fuzz/fuzz_formatter.c` testing `log_formatter_init` and `log_formatter_format` with arbitrary binary/string input.

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log_formatter.h"
#include "log_record.h"

int main(void) {
    char fmt[1024];
    if (!fgets(fmt, sizeof(fmt), stdin)) {
        return 0;
    }

    log_formatter_init(fmt, "%Y-%m-%d %H:%M:%S");

    log_record_t rec = {
        .level = LOG_LEVEL_INFO,
        .timestamp = 1600000000000000ULL,
        .tid = 1234,
        .pid = 5678,
        .file = "fuzz_test.c",
        .func = "fuzz_func",
        .line = 42,
        .module = "fuzz_mod",
        .tag = "fuzz_tag",
        .message = "fuzz_message_content"
    };

    char out[8192];
    log_formatter_format(&rec, out, sizeof(out));
    log_formatter_reset();

    return 0;
}
```

- [ ] **Step 3: Create seed file `fuzz/seeds/valid_config.yaml`**

```yaml
log:
  level: DEBUG
  async: false
  format: "[%time] [%level] [%module] %msg"
  console_enable: true
  file_enable: false
```

- [ ] **Step 4: Add `fuzz-build`, `fuzz-config`, and `fuzz-formatter` to `Makefile`**

Add build rules for `build/fuzz_config` and `build/fuzz_formatter` in `Makefile`.

- [ ] **Step 5: Verify build**

Run: `make fuzz-build`
Expected: Successfully builds `build/fuzz_config` and `build/fuzz_formatter`.

- [ ] **Step 6: Commit**

```bash
git add fuzz/ Makefile
git commit -m "feat: add AFL fuzzing harnesses and Makefile targets"
```

---

### Task 4: Complete System Verification

- [ ] **Step 1: Run standard unit test suite**

Run: `make clean && make test`
Expected: ALL PASS

- [ ] **Step 2: Run ASan test suite**

Run: `make test-asan`
Expected: ALL PASS

- [ ] **Step 3: Run UBSan test suite**

Run: `make test-ubsan`
Expected: ALL PASS

- [ ] **Step 4: Run fuzz harness smoke check**

Run: `./build/fuzz_formatter < fuzz/seeds/valid_config.yaml && ./build/fuzz_config fuzz/seeds/valid_config.yaml`
Expected: Exit code 0 without crash or sanitizer errors.
