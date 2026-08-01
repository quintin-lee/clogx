# P0/P1 Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the P0/P1 issues found in the 2026-08-01 project analysis: make the test suite idempotent, finalize the in-progress Windows atomic portability wrappers, close the `async_processing` cross-thread data race, guarantee valid JSON output under buffer truncation, and align dispatcher documentation with the actual mutex-based implementation.

**Architecture:** Five small, independently-committable fixes across build plumbing (`Makefile`, `tests/`), the portability layer (`include/clog_port.h`), the async hot path (`core/async.c`), the JSON renderers (`core/formatter.c`), and documentation (`include/dispatcher.h`, `core/dispatcher.c`). No new files, no new dependencies, no API changes. Each task ends with a green test run.

**Tech Stack:** C99, GCC/Clang on POSIX + MSVC on Windows, Makefile + CMake (both must stay green), libyaml, pthread.

## Global Constraints

- C99; compile flags `-std=c99 -Wall -Wextra -Wconversion -D_GNU_SOURCE -fvisibility=hidden` — new/modified code must be warning-free (or `make check` fails).
- Formatting: `.clang-format` (LLVM base, 4-space indent, 100 col, Linux braces, right pointer alignment, sorted includes). Run `make check-format`; if it fails, run `make format` and re-check the diff scope before committing.
- clang-tidy zero-warning policy (`WarningsAsErrors: '*'`), including the custom `clogx-unused-includes` check — no new `#include`s that are unused.
- Both build systems must stay in sync — this plan adds **no** new source files, so no test-registration changes are needed.
- Tests assume cwd = repo root and write configs to `build/`, logs to `logs/`.
- Commits follow **conventional commits with gitmoji** (see `git log`). Exact commit messages are given per task.
- Non-trivial changes add a `CHANGELOG.md` entry under `[Unreleased]` — exact entries are given per task.
- Branch is `master`. Do not touch files outside the task lists.
- **The working tree currently contains the Task 1 changes as uncommitted modifications** (`core/log.c`, `core/queue.c`, `core/socket_async.c`, `include/clog_port.h`). NEVER discard or revert them — Task 1 is exactly that work. If executing in a fresh worktree from `master`, apply the Task 1 code from this plan first.

## File Structure

- `include/clog_port.h` — add `clog_atomic_load_int` / `clog_atomic_store_int` / `clog_atomic_load_u64` / `clog_atomic_store_u64` wrappers (already present uncommitted; Task 1 reviews/finalizes).
- `core/queue.c`, `core/socket_async.c` — use the wrappers instead of raw `__atomic_*` (already converted uncommitted; Task 1 reviews/finalizes).
- `core/log.c` — zero-init `log_record_t` in write path (already present uncommitted; Task 1 reviews/finalizes).
- `Makefile` — `test` target cleans `logs/` (Task 2).
- `tests/test_kv_logging.c` — self-cleanup of its log file (Task 2).
- `core/async.c` — atomic access to `logger->async_processing` (Task 3).
- `core/log_internal.h` — field doc comment update (Task 3).
- `core/formatter.c` — truncation-safe JSON renderers (Task 4).
- `tests/test_json_formatter.c` — truncation regression test (Task 4).
- `include/dispatcher.h`, `core/dispatcher.c` — documentation alignment (Task 5).

---

### Task 1: Finalize Windows atomic portability wrappers

**Files:**
- Modify (already modified in working tree — review + commit): `include/clog_port.h`, `core/queue.c`, `core/socket_async.c`, `core/log.c`
- Modify: `CHANGELOG.md`

**Interfaces:**
- Consumes: nothing (this is the base work).
- Produces: `clog_atomic_load_int(const volatile int *ptr) -> int`, `clog_atomic_store_int(volatile int *ptr, int val)`, `clog_atomic_load_u64(const volatile uint64_t *ptr) -> uint64_t`, `clog_atomic_store_u64(volatile uint64_t *ptr, uint64_t val)` — used by Task 3 and already by `core/queue.c` / `core/socket_async.c`.

- [ ] **Step 1: Confirm the working-tree state**

Run: `git status --short && git diff --stat`
Expected: exactly these 4 modified files, no others:
```
 core/log.c          |  2 ++
 core/queue.c        | 14 +++++++-------
 core/socket_async.c | 12 ++++++------
 include/clog_port.h | 48 ++++++++++++++++++++++++++++++++++++++++++++++++
```
If extra files are modified, stop and report — do not commit anything else.

- [ ] **Step 2: Review the wrapper additions in `include/clog_port.h`**

`git diff include/clog_port.h` must add exactly these four functions (plus the section comment):

```c
/* ── Atomic int operations (for closed flags in ring buffers) ── */

static inline int clog_atomic_load_int(const volatile int *ptr)
{
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
#elif defined(_WIN32) || defined(_WIN64)
    return (int)InterlockedCompareExchange((LONG *)ptr, 0, 0);
#else
    return *ptr;
#endif
}

static inline void clog_atomic_store_int(volatile int *ptr, int val)
{
#if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(ptr, val, __ATOMIC_RELEASE);
#elif defined(_WIN32) || defined(_WIN64)
    InterlockedExchange((LONG *)ptr, (LONG)val);
#else
    *ptr = val;
#endif
}

/* ── Atomic uint64_t operations with acquire/release ordering (for seq counters) ── */

static inline uint64_t clog_atomic_load_u64(const volatile uint64_t *ptr)
{
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
#elif defined(_WIN32) || defined(_WIN64)
    return (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)ptr, 0, 0);
#else
    return *ptr;
#endif
}

static inline void clog_atomic_store_u64(volatile uint64_t *ptr, uint64_t val)
{
#if defined(__GNUC__) || defined(__clang__)
    __atomic_store_n(ptr, val, __ATOMIC_RELEASE);
#elif defined(_WIN32) || defined(_WIN64)
    InterlockedExchange64((volatile LONG64 *)ptr, (LONG64)val);
#else
    *ptr = val;
#endif
}
```

Review checklist (verify each, do not modify unless something fails):
1. GCC/Clang branch uses `__atomic_*` with `ACQUIRE`/`RELEASE` — must match the memory ordering the old direct `__atomic_load_n(..., __ATOMIC_ACQUIRE)` calls used in `core/queue.c` / `core/socket_async.c`.
2. MSVC load emulation via `InterlockedCompareExchange(ptr, 0, 0)` returns the current value (Exchange/Comperand are both 0 → pure read). Correct.
3. `InterlockedCompareExchange64` requires an 8-byte-aligned destination — `mpsc_slot_t.seq` is a `uint64_t` struct member, which MSVC aligns to 8 bytes on both x86 and x64. Correct.
4. The `#else` fallback (plain `*ptr`) is best-effort for exotic compilers — acceptable, do not extend it.

- [ ] **Step 3: Review the conversion diff in `core/queue.c` and `core/socket_async.c`**

`git diff core/queue.c core/socket_async.c` must be a 1:1 mechanical replacement, no logic changes:
- `__atomic_load_n(&q->closed, __ATOMIC_ACQUIRE)` → `clog_atomic_load_int(&q->closed)`
- `__atomic_store_n(&q->closed, 1, __ATOMIC_RELEASE)` → `clog_atomic_store_int(&q->closed, 1)`
- `__atomic_store_n(&slot->seq, (uint64_t)head + 1, __ATOMIC_RELEASE)` → `clog_atomic_store_u64(&slot->seq, (uint64_t)head + 1)`
- `__atomic_load_n(&slot->seq, __ATOMIC_ACQUIRE)` → `clog_atomic_load_u64(&slot->seq)`
- `__atomic_fetch_add(&ring->dropped, 1, __ATOMIC_RELAXED)` → `clog_atomic_inc64(&ring->dropped)`
- Same replacements in `core/socket_async.c` for `ring->closed` / `slot->seq` / `ring->dropped`.

And `git diff core/log.c` must add exactly two lines: `memset(&record, 0, sizeof(record));` after `log_record_t record;` in `logger_writevprintf_internal` (~line 289) and `supp_rec.kv_count = 0;` in the rate-limit suppression block (~line 323).

- [ ] **Step 4: Build and run the full test suite**

Run: `make -j$(nproc) && make test`
Expected: build clean (no warnings), all 43 tests pass. Note: if `test_kv_logging` fails with `Assertion 'count == 50'`, that is the known Task 2 defect — record it and continue (Task 2 fixes it); every OTHER test must pass.

- [ ] **Step 5: Add the CHANGELOG entry**

In `CHANGELOG.md` under `## [Unreleased]` → `### Fixed`, append (keep the bullet list style):

```markdown
- Centralized GCC/Clang `__atomic_*` builtins behind `clog_atomic_load_int` / `clog_atomic_store_int` / `clog_atomic_load_u64` / `clog_atomic_store_u64` in `include/clog_port.h` with MSVC `Interlocked*` equivalents so `core/queue.c` and `core/socket_async.c` compile on Windows; zero-initialize `log_record_t` in the write path (`core/log.c`) to avoid reading uninitialized `kv_count`
```

- [ ] **Step 6: Commit**

```bash
git add include/clog_port.h core/queue.c core/socket_async.c core/log.c CHANGELOG.md
git commit -m "fix(port): 🐛 wrap GCC __atomic builtins in clog_port.h with MSVC Interlocked equivalents"
```

---

### Task 2: Make `make test` idempotent

**Files:**
- Modify: `Makefile` (`test` target, ~line 184)
- Modify: `tests/test_kv_logging.c` (`test_kv_async_mode`, ~line 68)
- Test: run `make test` twice in a row

**Interfaces:**
- Consumes: nothing.
- Produces: `make test` and `./build/test_kv_logging` are both re-runnable without manual `logs/` cleanup.

- [ ] **Step 1: Reproduce the defect (evidence)**

Run: `make build/test_kv_logging && ./build/test_kv_logging && ./build/test_kv_logging`
Expected: first run PASSES, second run FAILS with `test_kv_logging: tests/test_kv_logging.c:100: Assertion 'count == 50' failed.` — because `logs/test_kv_async.log` accumulates (file sink opens with `"ab"`) and the test asserts an exact line count.

- [ ] **Step 2: Fix the Makefile `test` target**

In `Makefile`, change the `test` target (currently):

```make
test: $(TEST_BINS)
	@mkdir -p logs
	@status=0; \
```

to:

```make
test: $(TEST_BINS)
	@mkdir -p logs
	@rm -f logs/*.log
	@status=0; \
```

(`rm -f logs/*.log` runs once before any test binary, so tests that write-then-read their own file in the same run are unaffected.)

- [ ] **Step 3: Make the KV test self-cleaning (defense in depth)**

In `tests/test_kv_logging.c`, `test_kv_async_mode()`, immediately after the function's opening brace (before `log_config_t cfg = {0};` at line 69), add:

```c
    remove("logs/test_kv_async.log"); /* append-mode sink accumulates across runs */
```

(`remove()` is C89 stdio — portable to Windows CI, unlike `unlink()`.)

- [ ] **Step 4: Verify the fix**

Run:
```bash
make build/test_kv_logging && ./build/test_kv_logging && ./build/test_kv_logging
make test && make test
```
Expected: `test_kv_logging` passes twice in a row; `make test` passes twice in a row (all tests green both times).

- [ ] **Step 5: Commit**

```bash
git add Makefile tests/test_kv_logging.c
git commit -m "fix(tests): 🐛 make make test idempotent by cleaning logs/ before each run"
```

---

### Task 3: Make `async_processing` atomic (close data race)

**Files:**
- Modify: `core/async.c` (lines 190, 196, 253)
- Modify: `core/log_internal.h` (line 48, comment only)
- Test: async test binaries; best-effort TSan build

**Interfaces:**
- Consumes: `clog_atomic_store_int(volatile int *ptr, int val)` and `clog_atomic_load_int(const volatile int *ptr)` from Task 1.
- Produces: `logger->async_processing` is only ever accessed via atomic load/store — no plain cross-thread access remains.

- [ ] **Step 1: Confirm the current (racy) access points**

Run: `grep -rn "async_processing" core/`
Expected: exactly 4 hits — declaration in `core/log_internal.h:48` (`volatile int async_processing;`) and three accesses in `core/async.c` (lines 190, 196, 253). The worker thread writes at 190/196; `log_async_flush_for` polls at 253 from a different thread — plain `volatile` reads/writes are a C11 data race (UB), even though x86 happens to behave.

- [ ] **Step 2: Convert the three accesses in `core/async.c`**

`core/async.c:190` — change:

```c
        logger->async_processing = 1;
```
to:
```c
        clog_atomic_store_int(&logger->async_processing, 1);
```

`core/async.c:196` — change:

```c
        logger->async_processing = 0;
```
to:
```c
        clog_atomic_store_int(&logger->async_processing, 0);
```

`core/async.c:253` — change:

```c
    while (logger->async_processing) {
```
to:
```c
    while (clog_atomic_load_int(&logger->async_processing)) {
```

(Keep the field declared `volatile int` — the wrapper signatures require `volatile int *`, and the extra `volatile` is harmless. `core/async.c` already includes `clog_port.h` via `log_internal.h`, so no new `#include` is needed.)

- [ ] **Step 3: Update the field comment**

In `core/log_internal.h:48`, change:

```c
    volatile int  async_processing; /**< 1 while draining a batch. */
```
to:
```c
    volatile int  async_processing; /**< 1 while draining a batch; accessed only via clog_atomic_load_int/store_int. */
```

- [ ] **Step 4: Build and run async tests**

Run:
```bash
make build/test_async_lifecycle build/test_async_edge build/test_async_fallback build/test_async_reload build/test_multi_instance build/test_queue_try_put
./build/test_async_lifecycle && ./build/test_async_edge && ./build/test_async_fallback && ./build/test_async_reload && ./build/test_multi_instance && ./build/test_queue_try_put
```
Expected: all PASS.

- [ ] **Step 5 (best-effort): TSan verification**

Run:
```bash
make clean
make test CC=clang CFLAGS='-std=c99 -Wall -Wextra -Iinclude -O1 -g -D_GNU_SOURCE -fsanitize=thread -fno-omit-frame-pointer' EXTRA_LDFLAGS='-fsanitize=thread' 2>&1 | tail -20
```
Expected: all tests pass with no ThreadSanitizer reports. **If the TSan link fails** (unsupported toolchain/environment), note it and skip — do not chase it; the atomic conversion is verified by review + the normal suite. Restore the normal build afterwards: `make clean && make -j$(nproc)`.

- [ ] **Step 6: Add the CHANGELOG entry**

In `CHANGELOG.md` under `## [Unreleased]` → `### Fixed`, append:

```markdown
- `logger->async_processing` flag accessed via `clog_atomic_load_int` / `clog_atomic_store_int` instead of plain volatile reads/writes, closing a cross-thread data race between the async worker and `log_async_flush_for`
```

- [ ] **Step 7: Commit**

```bash
git add core/async.c core/log_internal.h CHANGELOG.md
git commit -m "fix(core): 🔒 make async_processing flag atomic to close worker/flush data race"
```

---

### Task 4: Keep JSON lines valid when truncated

**Files:**
- Modify: `core/formatter.c` — `append_json_escaped_string` (line 184), new helper `json_close_object`, `append_json_kv` (line 330), `format_json_ex` (line 389), `format_otel_json` (line 592)
- Modify: `tests/test_json_formatter.c` — new test `test_json_truncation_stays_valid`
- Test: `make build/test_json_formatter && ./build/test_json_formatter`; full `make test`

**Interfaces:**
- Consumes: nothing new.
- Produces: `append_json_escaped_string(char **out, size_t *remaining, const char *str) -> int` (0 = complete, -1 = truncated); `json_close_object(char *buf, char **out, size_t *remaining, const char *suffix) -> int` (line length or -1); `append_json_kv(char **out, size_t *remaining, const clog_kv_t *kv) -> int` (0 = ok, 1 = string truncated — caller must close, -1 = drop line).

- [ ] **Step 1: Write the failing regression test**

In `tests/test_json_formatter.c`:
- Add `#include "log_limits.h"` to the include block (needed for `CLOG_MAX_MESSAGE_SIZE` / `CLOG_MAX_FORMATTED_SIZE`).
- Add this function after `test_format_cache_switch` (before `main`):

```c
static void test_json_truncation_stays_valid(void)
{
    /* A message of quotes + control bytes escapes to far more than
     * CLOG_MAX_FORMATTED_SIZE. The renderer must close the line with a
     * valid suffix instead of emitting truncated invalid JSON. */
    remove("logs/json_trunc_test.log");

    log_config_t cfg   = {0};
    cfg.level          = LOG_LEVEL_INFO;
    cfg.console_enable = 0;
    cfg.file_enable    = 1;
    snprintf(cfg.file_path, sizeof(cfg.file_path), "logs/json_trunc_test.log");
    cfg.format = "json";

    logger_t *logger = logger_create_from_config(&cfg);
    assert(logger != NULL);

    char big[CLOG_MAX_MESSAGE_SIZE];
    for (size_t i = 0; i < sizeof(big) - 1; i++) {
        big[i] = (i % 2) ? '"' : (char)0x01; /* mixes \" and \u0001 escapes */
    }
    big[sizeof(big) - 1] = '\0';
    LOGGER_INFO(logger, "%s", big);

    logger_flush(logger);
    logger_destroy(logger);

    FILE *f = fopen("logs/json_trunc_test.log", "rb");
    assert(f != NULL);
    char   buf[CLOG_MAX_FORMATTED_SIZE + 16];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    /* Strip the newline appended by the dispatcher. */
    if (n > 0 && buf[n - 1] == '\n') {
        buf[--n] = '\0';
    }

    assert(buf[0] == '{');
    assert(n >= 3);
    assert(strcmp(buf + n - 2, "\"}") == 0); /* ends with a closed string + object brace */
    printf("test_json_truncation_stays_valid PASSED\n");
}
```

- In `main()`, call it between `test_format_cache_switch();` and `remove(LOG_PATH);`:

```c
    test_format_cache_switch();
    test_json_truncation_stays_valid();
```

- [ ] **Step 2: Run test to verify it fails**

Run: `make build/test_json_formatter && ./build/test_json_formatter 2>&1 | tail -5`
Expected: assertion failure (`assert(strcmp(buf + n - 2, "\"}") == 0)` fails) — today the line ends mid-escape with no closing quote, e.g. `..."message":"\u0001\u0001\u0001` + newline.

- [ ] **Step 3: Change `append_json_escaped_string` to report truncation**

In `core/formatter.c`, replace the entire function (currently `static void append_json_escaped_string(char **out, size_t *remaining, const char *str)`, lines ~184-257) with:

```c
/**
 * @brief RFC 8259 JSON string escaping for log field values.
 *
 * Writes @p str to @p out with proper JSON escapes: `\"`, `\\`,
 * `\b`, `\f`, `\n`, `\r`, `\t`, and `\u00XX` for control bytes
 * below 0x20. Output is always null-terminated within @p remaining.
 *
 * @param[in,out] out        Output pointer (advanced past written bytes).
 * @param[in,out] remaining  Bytes remaining in buffer (decremented).
 * @param[in]     str        Input string (NULL treated as empty).
 * @return 0 when @p str was written completely, -1 when the output
 *         buffer ran out before @p str was exhausted (truncated).
 */
static int append_json_escaped_string(char **out, size_t *remaining, const char *str)
{
    if (!str) {
        str = "";
    }
    while (*str && *remaining > 1) {
        unsigned char c = (unsigned char)*str++;
        if (c == '"') {
            if (*remaining <= 2) {
                break;
            }
            *(*out)++ = '\\';
            *(*out)++ = '"';
            *remaining -= 2;
        } else if (c == '\\') {
            if (*remaining <= 2) {
                break;
            }
            *(*out)++ = '\\';
            *(*out)++ = '\\';
            *remaining -= 2;
        } else if (c == '\b') {
            if (*remaining <= 2) {
                break;
            }
            *(*out)++ = '\\';
            *(*out)++ = 'b';
            *remaining -= 2;
        } else if (c == '\f') {
            if (*remaining <= 2) {
                break;
            }
            *(*out)++ = '\\';
            *(*out)++ = 'f';
            *remaining -= 2;
        } else if (c == '\n') {
            if (*remaining <= 2) {
                break;
            }
            *(*out)++ = '\\';
            *(*out)++ = 'n';
            *remaining -= 2;
        } else if (c == '\r') {
            if (*remaining <= 2) {
                break;
            }
            *(*out)++ = '\\';
            *(*out)++ = 'r';
            *remaining -= 2;
        } else if (c == '\t') {
            if (*remaining <= 2) {
                break;
            }
            *(*out)++ = '\\';
            *(*out)++ = 't';
            *remaining -= 2;
        } else if (c < 0x20) {
            if (*remaining <= 6) {
                break;
            }
            int n = snprintf(*out, *remaining, "\\u00%02x", c);
            if (n > 0 && (size_t)n < *remaining) {
                *out += n;
                *remaining -= (size_t)n;
            } else {
                break;
            }
        } else {
            *(*out)++ = (char)c;
            (*remaining)--;
        }
    }
    **out = '\0';
    return (*str == '\0') ? 0 : -1;
}
```

(Only the signature and the final `return` changed — the escaping body is identical, so escaped output is byte-for-byte unchanged.)

- [ ] **Step 4: Add the `json_close_object` helper**

In `core/formatter.c`, immediately after `append_json_escaped_string` (before the `g_hex_lut` definition), add:

```c
/**
 * @brief Close a JSON object line after a field string was truncated.
 *
 * Appends @p suffix — e.g. `"}` to close a root object, `"}}` to close
 * the OTel attributes object — and returns the total line length.
 * Returns -1 when the suffix does not fit; the caller must drop the line.
 */
static int json_close_object(char *buf, char **out, size_t *remaining, const char *suffix)
{
    size_t n = strlen(suffix);
    if (*remaining < n + 1) {
        return -1;
    }
    memcpy(*out, suffix, n);
    *out += n;
    *remaining -= n;
    **out = '\0';
    return (int)(*out - buf);
}
```

- [ ] **Step 5: Update `append_json_kv` to signal string truncation**

In `core/formatter.c`, replace the whole `append_json_kv` function (lines ~330-363) with:

```c
/**
 * @brief Append one typed KV attribute: `,"key":value`.
 *
 * @return 0 on success; 1 when a `"`-delimited string was truncated
 *         (the caller must close the object via json_close_object);
 *         -1 on failure (the caller must drop the line).
 */
static int append_json_kv(char **out, size_t *remaining, const clog_kv_t *kv)
{
    if (!kv || !kv->key) {
        return 0;
    }
    if (append_buf_str(out, remaining, ",\"", 2) != 0) {
        return -1;
    }
    if (append_json_escaped_string(out, remaining, kv->key) != 0) {
        return 1; /* key string truncated — caller closes with '"' + closer */
    }
    if (append_buf_str(out, remaining, "\":", 2) != 0) {
        return -1;
    }
    switch (kv->type) {
    case CLOG_KV_TYPE_INT:
        return append_buf_i64(out, remaining, kv->val.i64);
    case CLOG_KV_TYPE_UINT:
        return append_buf_u64(out, remaining, kv->val.u64);
    case CLOG_KV_TYPE_FLOAT: {
        char fbuf[32];
        snprintf(fbuf, sizeof(fbuf), "%.6g", kv->val.f64);
        return append_buf_str(out, remaining, fbuf, strlen(fbuf));
    }
    case CLOG_KV_TYPE_STR:
        if (append_buf_str(out, remaining, "\"", 1) != 0) {
            return -1;
        }
        if (append_json_escaped_string(out, remaining, kv->val.str ? kv->val.str : "") != 0) {
            return 1; /* value string truncated — caller closes with '"' + closer */
        }
        return append_buf_str(out, remaining, "\"", 1);
    case CLOG_KV_TYPE_BOOL:
        return append_buf_str(out, remaining, kv->val.b ? "true" : "false", kv->val.b ? 4 : 5);
    default:
        return 0;
    }
}
```

- [ ] **Step 6: Make `format_json_ex` truncation-safe**

In `core/formatter.c`, `format_json_ex` (lines ~389-473), replace the five escaped-string call sites and the KV loop:

1. Module (currently `append_json_escaped_string(&out, &remaining, record->module ? record->module : "");`):

```c
    if (append_json_escaped_string(&out, &remaining, record->module ? record->module : "") != 0) {
        return json_close_object(buf, &out, &remaining, "\"}");
    }
```

2. File (currently after the `\",\"file\":\"` append):

```c
    if (append_json_escaped_string(&out, &remaining, record->file ? record->file : "") != 0) {
        return json_close_object(buf, &out, &remaining, "\"}");
    }
```

3. Func (currently after the `,\"func\":\"` append):

```c
    if (append_json_escaped_string(&out, &remaining, record->func ? record->func : "") != 0) {
        return json_close_object(buf, &out, &remaining, "\"}");
    }
```

4. Tag (currently after the `,\"tag\":\"` append):

```c
    if (append_json_escaped_string(&out, &remaining, record->tag ? record->tag : "") != 0) {
        return json_close_object(buf, &out, &remaining, "\"}");
    }
```

5. Message (currently an unchecked call followed by a `\"` append):

```c
    if (append_json_escaped_string(&out, &remaining, record->message ? record->message : "") != 0) {
        return json_close_object(buf, &out, &remaining, "\"}");
    }
```

6. KV loop (currently `if (append_json_kv(&out, &remaining, &record->kv[i]) != 0) { return -1; }`):

```c
    for (size_t i = 0; i < record->kv_count; i++) {
        int r = append_json_kv(&out, &remaining, &record->kv[i]);
        if (r == 1) {
            return json_close_object(buf, &out, &remaining, "\"}");
        }
        if (r != 0) {
            return -1;
        }
    }
```

Note the C literal `"\"" "}"` — written `"\"}"` in source — is the 2-character string `"}` (close quote + close brace). With step 6, a truncated line looks like `{"timestamp":"...","message":"\u0001\u0001"}` — valid JSON, message visibly cut at the end.

- [ ] **Step 7: Make `format_otel_json` truncation-safe**

In `core/formatter.c`, `format_otel_json` (lines ~592-682), apply the same pattern — the suffix depends on nesting depth (`"}` for `body`, `"}}` for attributes fields):

1. Body (currently `append_json_escaped_string(&out, &remaining, record->message ? record->message : "");`):

```c
    if (append_json_escaped_string(&out, &remaining, record->message ? record->message : "") != 0) {
        return json_close_object(buf, &out, &remaining, "\"}");
    }
```

2. Module (inside `attributes`):

```c
    if (append_json_escaped_string(&out, &remaining, record->module ? record->module : "") != 0) {
        return json_close_object(buf, &out, &remaining, "\"}}");
    }
```

3. File:

```c
    if (append_json_escaped_string(&out, &remaining, record->file ? record->file : "") != 0) {
        return json_close_object(buf, &out, &remaining, "\"}}");
    }
```

4. Func:

```c
    if (append_json_escaped_string(&out, &remaining, record->func ? record->func : "") != 0) {
        return json_close_object(buf, &out, &remaining, "\"}}");
    }
```

5. Tag:

```c
    if (append_json_escaped_string(&out, &remaining, record->tag ? record->tag : "") != 0) {
        return json_close_object(buf, &out, &remaining, "\"}}");
    }
```

6. KV loop (inside `attributes`):

```c
    for (size_t i = 0; i < record->kv_count; i++) {
        int r = append_json_kv(&out, &remaining, &record->kv[i]);
        if (r == 1) {
            return json_close_object(buf, &out, &remaining, "\"}}");
        }
        if (r != 0) {
            return -1;
        }
    }
```

- [ ] **Step 8: Run the new test**

Run: `make build/test_json_formatter && ./build/test_json_formatter 2>&1 | tail -6`
Expected: all four tests PASS, including `test_json_truncation_stays_valid PASSED`.

- [ ] **Step 9: Run the full suite + OTel regression**

Run: `make test`
Expected: all 43 tests pass, including `test_otel`, `test_kv_logging`, `test_pipeline`.

- [ ] **Step 10: Add the CHANGELOG entry**

In `CHANGELOG.md` under `## [Unreleased]` → `### Fixed`, append:

```markdown
- JSON and OTel renderers guarantee well-formed output under buffer overflow: when a string field exhausts the line buffer, the line is closed with a valid suffix (`"}` for the JSON root, `"}}` for the OTel attributes object) instead of emitting truncated invalid JSON; a truncating KV attribute closes the object the same way, and a line that cannot even be closed is dropped
```

- [ ] **Step 11: Commit**

```bash
git add core/formatter.c tests/test_json_formatter.c CHANGELOG.md
git commit -m "fix(formatter): 🐛 emit valid JSON when a field overflows the line buffer"
```

---

### Task 5: Align dispatcher documentation with the implementation

**Files:**
- Modify: `include/dispatcher.h` (Architecture + Thread Safety sections, dispatch doc note)
- Modify: `core/dispatcher.c` (file-header comment, lines ~9-20)
- Test: grep-based verification; `make check-format`; `make test`

**Interfaces:**
- Consumes: nothing.
- Produces: documentation that matches the mutex-based implementation; no references to the non-existent `log_snapshot_get` / `log_snapshot_release` APIs.

- [ ] **Step 1: Fix the stale claims in `core/dispatcher.c`**

The file header currently says (lines 8-12):

```c
 * 1. A **snapshot** of the current sinks is read under a reader-writer lock.
 * 2. The message is formatted **once** using `log_formatter_format`.
 * 3. The formatted string is written to every sink in the snapshot.
 * 4. For async mode, the deep-copied record is queued instead of dispatching.
```

Replace line 1 with:

```c
 * 1. The sink array is read under `dispatcher_mutex` (the dispatcher lock).
```

And lines 18-20 currently say:

```c
 * When `log_reload` is called, the new config is applied atomically:
 * the dispatcher lock is held while the old sink list is freed and replaced.
 * `log_snapshot_get` returns the snapshot without locking; the caller is
 * responsible for calling `log_snapshot_release` when done.
```

Replace with:

```c
 * When `log_reload` is called, a fresh sink set is built off-line via
 * `log_dispatcher_build_snapshot_for`, then swapped in atomically by
 * `log_dispatcher_commit_snapshot_for` while `dispatcher_mutex` is held;
 * the old sinks are destroyed after the swap.
```

- [ ] **Step 2: Fix the stale claims in `include/dispatcher.h`**

1. In the `## Architecture` section, change `(guarded by a read-write lock)` to `(guarded by the dispatcher mutex)` (the parenthetical at the end of step 3).

2. In `## Thread Safety`, change:

```c
 * - @ref log_dispatcher_dispatch is thread-safe (read-locked).
```
to:
```c
 * - @ref log_dispatcher_dispatch is thread-safe (serialised by the
 *   dispatcher mutex; the line is formatted before the lock is taken).
```

3. In the `log_dispatcher_dispatch` doc block, change `@note Thread-safe (acquires a read lock).` to `@note Thread-safe (serialised by the dispatcher mutex).`

- [ ] **Step 3: Verify no stale references remain**

Run: `grep -rn "read lock\|read-locked\|log_snapshot_get\|log_snapshot_release\|reader-writer" include/dispatcher.h core/dispatcher.c`
Expected: no output.

- [ ] **Step 4: Formatting + regression**

Run: `make check-format && make test`
Expected: `check-format` clean (no diff), all tests pass (comments-only change must not alter behavior).

- [ ] **Step 5: Commit**

```bash
git add include/dispatcher.h core/dispatcher.c
git commit -m "docs(dispatcher): 📝 align dispatcher docs with mutex-based implementation"
```

---

### Task 6: Full quality gate

**Files:**
- No code changes (fix fallout only if the gate finds it)
- Test: full project gate

**Interfaces:**
- Consumes: Tasks 1-5.
- Produces: a verified, gate-clean master.

- [ ] **Step 1: Run the complete gate**

Run: `make check`
Expected: format check → clang-tidy → custom unused-includes check → clean rebuild → all tests, all green. (This is the same gate CI runs before merge.)

- [ ] **Step 2: Verify the CHANGELOG has one entry per fix**

Run: `grep -n "clog_atomic\|async_processing\|valid JSON\|idempotent\|make test" CHANGELOG.md | head`
Expected: Task 1 (clog_atomic), Task 3 (async_processing), Task 4 (valid JSON) entries visible under `[Unreleased] → Fixed`. Tasks 2 and 5 are test/docs-only — no CHANGELOG entry required (matching existing project practice).

- [ ] **Step 3: Final commit (only if the gate required fixes)**

If `make check` forced any edits, commit them:

```bash
git add -A
git commit -m "chore: 🧹 fix fallout from full quality gate"
```

If no edits were needed, make no commit.

- [ ] **Step 4: Report**

Summarize: the 5 commits produced, gate result, and any deviations (e.g. TSan skipped).
