# journald Sink Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a built-in `journald_sink.c` (native journal protocol, zero new dependencies) with factory declaration, dual-build registration, and a three-layer test.

**Architecture:** One new source file implementing the 4-callback sink vtable (write/flush/destroy/atfork_child), mirroring `sinks/syslog_sink.c`. API-only distribution (factory + `log_add_sink`), no config/dispatcher changes. Non-Linux stub returns NULL.

**Tech Stack:** C99, POSIX `AF_UNIX`/`SOCK_DGRAM`, systemd native journal protocol, existing `make` + CMake harnesses.

**Spec:** `docs/superpowers/specs/2026-10-08-journald-sink-design.md` (commit `d889085`).

---

## File map

- Create: `sinks/journald_sink.c` (Linux impl + `__linux__` guard stub).
- Create: `tests/test_journald_sink.c` (plain `main()`, exit 0/1, diagnostics to **stderr** — `log_destroy()` closes the stdout fd, see prior lesson).
- Modify: `include/log_sink.h` (one factory declaration after `syslog_sink_create`).
- Modify: `Makefile:117` (`SINK_SRCS` append), `Makefile` TESTS list (after `test_syslog_sink`).
- Modify: `CMakeLists.txt` sinks list (after `sinks/syslog_sink.c`), `CLOG_TEST_SOURCES` (after `test_syslog_sink`).

---

### Task 1: Register build entries + write failing test (RED)

**Files:**
- Create: `tests/test_journald_sink.c`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Append sink source to Makefile SINK_SRCS**

Line 117 today:
```make
SINK_SRCS = console_sink.c file_sink.c socket_sink.c custom_sink.c syslog_sink.c otlp_sink.c
```
Change to:
```make
SINK_SRCS = console_sink.c file_sink.c socket_sink.c custom_sink.c syslog_sink.c otlp_sink.c journald_sink.c
```

- [ ] **Step 2: Append test name to Makefile TESTS (after test_syslog_sink)**

Find `test_observability_stats test_syslog_sink test_thread_context` and change to:
```make
        test_observability_stats test_syslog_sink test_journald_sink test_thread_context test_coverage_boost test_mutex_guard_raii test_coverage_deep \
```
(Keep the exact surrounding entries; only insert `test_journald_sink` after `test_syslog_sink`.)

- [ ] **Step 3: Register in CMakeLists.txt (two spots)**

After `sinks/syslog_sink.c` insert `sinks/journald_sink.c`. After the `test_syslog_sink` line in `CLOG_TEST_SOURCES` insert `test_journald_sink`.

- [ ] **Step 4: Write the test file (full content)**

```c
/**
 * @file test_journald_sink.c
 * @brief Tests for the native journald sink: factory semantics + journal round-trip.
 *
 * Layer 1 (runs everywhere): factory NULL/ident validation, platform skip.
 * Layer 2 (journal machines only): write one ERROR record with a unique token,
 * flush, then read it back via journalctl and check token + PRIORITY=3.
 * Missing socket or missing journalctl -> skip whole layer, exit 0.
 */

#include "log.h"
#include "log_sink.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define JOURNAL_SOCKET_PATH "/run/systemd/journal/socket"

static int g_failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);            \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

static int has_journal_socket(void)
{
    return access(JOURNAL_SOCKET_PATH, W_OK) == 0;
}

static int has_journalctl(void)
{
    /* system(3) return is shell status; check exit code of the command. */
    int rc = system("journalctl --version >/dev/null 2>&1");
    return rc == 0;
}

static void test_factory_bad_ident(void)
{
#if defined(__linux__)
    if (!has_journal_socket()) {
        fprintf(stderr, "SKIP: no journal socket, factory test skipped\n");
        return;
    }
    log_sink_t *bad = journald_sink_create("has\nnewline");
    CHECK(bad == NULL, "ident with newline must be rejected");
    /* bad is NULL on success path; nothing to destroy. */
#else
    log_sink_t *s = journald_sink_create("test_clogx");
    CHECK(s == NULL, "non-Linux factory must return NULL");
    fprintf(stderr, "SKIP: journald sink not supported on this platform\n");
#endif
}

static void test_round_trip(void)
{
#if defined(__linux__)
    char token[128];
    char cmd[512];
    char line[1024];
    FILE *fp;
    int   found_token   = 0;
    int   found_prio    = 0;
    int   journal_calls = 0;

    if (!has_journal_socket() || !has_journalctl()) {
        fprintf(stderr, "SKIP: no journal socket or journalctl, round-trip skipped\n");
        return;
    }
    if (log_init(NULL) != 0) {
        fprintf(stderr, "FAIL: log_init failed\n");
        g_failures++;
        return;
    }
    log_sink_t *sink = journald_sink_create("test_clogx_journald");
    if (!sink) {
        fprintf(stderr, "FAIL: factory returned NULL despite journal socket\n");
        g_failures++;
        log_destroy();
        return;
    }
    if (log_add_sink(sink) != 0) {
        fprintf(stderr, "FAIL: log_add_sink failed\n");
        g_failures++;
        log_destroy();
        return;
    }
    snprintf(token, sizeof(token), "clogx_journald_%d", (int)getpid());
    LOG_ERROR("%s", token);
    log_flush();

    snprintf(cmd,
             sizeof(cmd),
             "journalctl --since \"1 minute ago\" -t test_clogx_journald -o verbose 2>/dev/null");
    fp = popen(cmd, "r");
    if (!fp) {
        fprintf(stderr, "FAIL: popen journalctl failed\n");
        g_failures++;
        log_destroy();
        return;
    }
    while (fgets(line, sizeof(line), fp)) {
        journal_calls++;
        if (strstr(line, token)) {
            found_token = 1;
        }
        if (strstr(line, "PRIORITY=3")) {
            found_prio = 1;
        }
    }
    pclose(fp);
    log_destroy();
    CHECK(journal_calls > 0, "journalctl returned no output (access denied?)");
    CHECK(found_token, "token not found in journal");
    CHECK(found_prio, "PRIORITY=3 not found in journal");
#else
    fprintf(stderr, "SKIP: round-trip test needs Linux with systemd\n");
#endif
}

int main(void)
{
    test_factory_bad_ident();
    test_round_trip();
    if (g_failures == 0) {
        fprintf(stderr, "journald sink test passed\n");
        return 0;
    }
    fprintf(stderr, "journald sink test FAILED (%d failures)\n", g_failures);
    return 1;
}
```

- [ ] **Step 5: Run build to verify RED (factory undefined)**

Run: `make build/test_journald_sink 2>&1 | head -5`
Expected: FAIL with `undefined reference to journald_sink_create` (test compiles, link fails — header has no declaration yet and no sink source exists).

- [ ] **Step 6: Commit red state**

```bash
git add tests/test_journald_sink.c Makefile CMakeLists.txt
git commit -m "test(journald): 🔴 add failing journald sink test + build registration"
```

---

### Task 2: Factory declaration in log_sink.h

**Files:**
- Modify: `include/log_sink.h` (after `syslog_sink_create` declaration, ~line 425)

- [ ] **Step 1: Add declaration with doxygen**

```c
/**
 * @brief Create a native systemd-journal sink (Linux only).
 *
 * Sends each log record as one datagram to `/run/systemd/journal/socket`
 * using the journal native protocol, with `PRIORITY` (mapped from the
 * record level the same way as the syslog sink), `SYSLOG_IDENTIFIER`,
 * and `MESSAGE` fields. No libsystemd dependency.
 *
 * @param[in] ident Journal SYSLOG_IDENTIFIER value (e.g. "my_app").
 *                  NULL defaults to "clogx". Must not contain '\n'.
 *
 * @return New sink, or NULL on allocation failure, when @p ident contains
 *         '\n', when the journal socket is unavailable, or on non-Linux
 *         platforms (Windows/macOS stub).
 *
 * Note Oversized MESSAGE payloads are truncated to `240 * 1024` bytes.
 *       Runtime send failures return -1 from write (no in-sink retry).
 */
CLOGX_API log_sink_t *journald_sink_create(const char *ident);
```

- [ ] **Step 2: Verify header still parses (compile a trivial TU)**

Run: `make build/test_journald_sink 2>&1 | head -5`
Expected: FAIL, now with `undefined reference` at link stage only (declaration visible, no definition yet) — or missing `sinks/journald_sink.c` build error. Either link-stage failure counts as still-RED.

Do NOT commit (fold into Task 3 commit).

---

### Task 3: Implement sinks/journald_sink.c

**Files:**
- Create: `sinks/journald_sink.c`

- [ ] **Step 1: Write the full implementation**

```c
/**
 * @file journald_sink.c
 * @brief Native systemd-journal sink (no libsystemd dependency).
 *
 * ## Design
 *
 * Each log record is sent as one `AF_UNIX`/`SOCK_DGRAM` datagram to
 * `/run/systemd/journal/socket` per the journal native protocol
 * (https://systemd.io/JOURNAL_NATIVE_PROTOCOL):
 * `PRIORITY=<n>\nSYSLOG_IDENTIFIER=<ident>\nMESSAGE=<text>\n`, with the
 * binary-safe form (`NAME\n` + LE64 length + raw bytes + `\n`) when
 * MESSAGE contains an embedded newline.
 *
 * PRIORITY mapping reuses the syslog sink table: TRACE/DEBUG->7,
 * INFO->6, WARN->4, ERROR->3, FATAL->2, unknown->6.
 *
 * ## Failure policy
 *
 * Factory fails (NULL) when the socket is unavailable. Runtime send
 * failures return -1 with no retry; drop accounting stays with the
 * dispatcher. Oversized MESSAGE is truncated to 240*1024 bytes.
 *
 * ## Platform
 *
 * Linux only. Other platforms return NULL from the factory.
 */

#include "clogx_plugin.h"
#include "log_sink.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define JOURNALD_SOCKET_PATH "/run/systemd/journal/socket"
/** @brief MESSAGE truncation cap for oversized records. */
#define JOURNALD_MAX_MESSAGE (240u * 1024u)

typedef struct {
    int   fd;
    char *ident;
} journald_sink_data_t;

static int journald_priority_from_text(const char *buf)
{
    if (strstr(buf, "[TRACE]") || strstr(buf, "[DEBUG]")) {
        return 7;
    }
    if (strstr(buf, "[WARN]") || strstr(buf, "[WARNING]")) {
        return 4;
    }
    if (strstr(buf, "[ERROR]")) {
        return 3;
    }
    if (strstr(buf, "[FATAL]")) {
        return 2;
    }
    return 6;
}

static void journald_store_le64(unsigned char *dst, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++) {
        dst[i] = (unsigned char)(v & 0xffu);
        v >>= 8;
    }
}

static int journald_write(log_sink_t *sink, const char *buf, size_t len)
{
    journald_sink_data_t *data = (journald_sink_data_t *)sink->private_data;
    char                  prio_line[32];
    int                   prio_len;
    size_t                msg_len;
    size_t                total;
    int                   use_binary;
    char                 *packet;
    size_t                off = 0;

    if (!data || data->fd < 0 || !buf) {
        return -1;
    }
    /* Strip the single trailing newline the dispatcher appends for streams. */
    msg_len = len;
    while (msg_len > 0 && (buf[msg_len - 1] == '\n' || buf[msg_len - 1] == '\r')) {
        msg_len--;
    }
    if (msg_len > JOURNALD_MAX_MESSAGE) {
        msg_len = JOURNALD_MAX_MESSAGE;
    }
    prio_len =
        snprintf(prio_line, sizeof(prio_line), "PRIORITY=%d\n", journald_priority_from_text(buf));
    if (prio_len < 0) {
        return -1;
    }
    use_binary = (memchr(buf, '\n', msg_len) != NULL);
    total      = (size_t)prio_len + strlen("SYSLOG_IDENTIFIER=") + strlen(data->ident) + 1u;
    if (use_binary) {
        total += strlen("MESSAGE\n") + 8u + msg_len + 1u;
    } else {
        total += strlen("MESSAGE=") + msg_len + 1u;
    }
    packet = (char *)malloc(total);
    if (!packet) {
        return -1;
    }
    memcpy(packet + off, prio_line, (size_t)prio_len);
    off += (size_t)prio_len;
    memcpy(packet + off, "SYSLOG_IDENTIFIER=", strlen("SYSLOG_IDENTIFIER="));
    off += strlen("SYSLOG_IDENTIFIER=");
    memcpy(packet + off, data->ident, strlen(data->ident));
    off += strlen(data->ident);
    packet[off++] = '\n';
    if (use_binary) {
        memcpy(packet + off, "MESSAGE\n", strlen("MESSAGE\n"));
        off += strlen("MESSAGE\n");
        journald_store_le64((unsigned char *)(packet + off), (uint64_t)msg_len);
        off += 8u;
        memcpy(packet + off, buf, msg_len);
        off += msg_len;
    } else {
        memcpy(packet + off, "MESSAGE=", strlen("MESSAGE="));
        off += strlen("MESSAGE=");
        memcpy(packet + off, buf, msg_len);
        off += msg_len;
    }
    packet[off++] = '\n';
    if (off != total) {
        free(packet);
        return -1;
    }
    if (send(data->fd, packet, off, MSG_NOSIGNAL) != (ssize_t)off) {
        free(packet);
        return -1;
    }
    free(packet);
    return (int)len;
}

static void journald_flush(log_sink_t *sink)
{
    (void)sink;
}

static void journald_close_fd(journald_sink_data_t *data)
{
    if (data->fd >= 0) {
        close(data->fd);
        data->fd = -1;
    }
}

static void journald_destroy(log_sink_t *sink)
{
    if (!sink) {
        return;
    }
    journald_sink_data_t *data = (journald_sink_data_t *)sink->private_data;
    if (data) {
        journald_close_fd(data);
        free(data->ident);
        free(data);
    }
    free(sink);
}

static int journald_connect(void)
{
    struct sockaddr_un addr;
    int                fd = socket(AF_UNIX, SOCK_DGRAM, 0);

    if (fd < 0) {
        return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", JOURNALD_SOCKET_PATH);
    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void journald_atfork_child(log_sink_t *sink)
{
    journald_sink_data_t *data;

    if (!sink || !sink->private_data) {
        return;
    }
    data = (journald_sink_data_t *)sink->private_data;
    journald_close_fd(data);
    data->fd = journald_connect();
}

/**
 * @brief Create a native systemd-journal sink. See log_sink.h for contract.
 */
log_sink_t *journald_sink_create(const char *ident)
{
    log_sink_t           *sink;
    journald_sink_data_t *data;
    const char           *name = ident ? ident : "clogx";

    if (strchr(name, '\n')) {
        return NULL;
    }
    sink = (log_sink_t *)malloc(sizeof(log_sink_t));
    if (!sink) {
        return NULL;
    }
    data = (journald_sink_data_t *)malloc(sizeof(journald_sink_data_t));
    if (!data) {
        free(sink);
        return NULL;
    }
    data->ident = strdup(name);
    if (!data->ident) {
        free(data);
        free(sink);
        return NULL;
    }
    data->fd = journald_connect();
    if (data->fd < 0) {
        free(data->ident);
        free(data);
        free(sink);
        return NULL;
    }
    sink->abi_version  = CLOGX_PLUGIN_ABI_VERSION;
    sink->write        = journald_write;
    sink->flush        = journald_flush;
    sink->destroy      = journald_destroy;
    sink->atfork_child = journald_atfork_child;
    sink->private_data = data;
    sink->min_level    = LOG_LEVEL_TRACE;
    return sink;
}

#else /* not __linux__ */

log_sink_t *journald_sink_create(const char *ident)
{
    (void)ident;
    return NULL;
}

#endif
```

Conventions honored: sorted includes (`clogx_plugin.h` < `log_sink.h` < `stdint` < `stdio` < `stdlib` < `string`; system headers after, alphabetical: `sys/socket.h` < `sys/un.h` < `unistd.h`); no `as any`-style casts beyond the `sockaddr` cast (required by API, same as socket_sink); `MSG_NOSIGNAL` avoids SIGPIPE on datagram send; `LOG_LEVEL_TRACE` comes via `log_record.h` included by `log_sink.h`.

- [ ] **Step 2: Build the new test (expect GREEN link)**

Run: `make build/test_journald_sink 2>&1 | head -10`
Expected: clean build, zero warnings (`-Wall -Wextra -Wconversion`).

- [ ] **Step 3: Run the test (expect PASS)**

Run: `./build/test_journald_sink; echo "exit=$?"`
Expected: `journald sink test passed` on stderr, `exit=0`. On this dev machine (systemd 261 + socket present) the round-trip layer executes: if `journalctl` output check fails due to read permissions, the CHECK reports it — do NOT weaken the check; investigate (likely need `journalctl --user`? No — system journal read may need group membership; if so, document and use `sudo`? Never silently skip a failing assertion).

- [ ] **Step 4: Commit**

```bash
git add sinks/journald_sink.c include/log_sink.h
git commit -m "feat(sink): ✨ add native journald sink (zero-dep datagram client)"
```

---

### Task 4: Full gates + benchmark guard

**Files:** none (verification only).

- [ ] **Step 1: Run the full test suite**

Run: `make test 2>&1 | tail -5`
Expected: `EXIT 0` equivalent (all binaries pass, including `test_journald_sink`).

- [ ] **Step 2: Run make check**

Run: `make check 2>&1 | tail -8`
Expected: format + clang-tidy + unused-includes clean. Known pre-existing failure: `mermaid-check` (missing jsdom, fails identically on clean master) — if it is the ONLY failure, note it and continue.

- [ ] **Step 3: TSan run**

Run: `make tsan 2>&1 | tail -5` (or the repo's documented TSan target; check `Makefile` `tsan` target name first with `grep -n "^tsan\|^test-tsan" Makefile`).
Expected: full suite green under ThreadSanitizer, including the new test.

- [ ] **Step 4: Confirm benchmarks unmoved (add-only change)**

Run the benchmark binary once and compare sync/async/throughput against committed `benchmarks/.baseline` (tolerance: within run-to-run noise, ~5%). No `make benchmark-baseline` refresh — this change must not move hot-path numbers.
Expected: numbers within noise of baseline.

- [ ] **Step 5: Commit (only if gates 1-4 pass; nothing to commit if clean — record results in the final report)**

No code changes expected in this task. If `make format` touched files in earlier tasks, amend them into the Task 3 commit family with a separate `style:` commit.

---

## Self-Review

**1. Spec coverage:**
- S1 new files → Task 1 (test) + Task 3 (sink); registration lines → Task 1 steps 1–3; factory contract (NULL cases, default ident) → Task 2 + Task 3 factory + Task 1 layer-1 test; success criteria → Task 4.
- S2 transport/datagram layout/PRIORITY table/binary branch/truncation/failure semantics/atfork → all in Task 3 code (framing, `journald_priority_from_text`, `JOURNALD_MAX_MESSAGE`, `journald_connect` NULL-on-missing, `journald_atfork_child`).
- S3 three layers + gates + no-benchmark-refresh → Task 1 test file + Task 4 steps 1–4. The rejected test-only factory override is documented by its absence.
- Spec deviation (one, pre-approved pattern): S3 says "journalctl --since --grep"; the plan uses `-o verbose` + in-test `strstr` instead of `--grep` so PRIORITY can be checked in the same pass — strictly stronger, same skip semantics.

**2. Placeholder scan:** no TBD/TODO/"appropriate error handling" — every error path has explicit code (`malloc` NULL checks, `snprintf` < 0, `off != total` internal guard, `send` short-write).

**3. Type consistency:** `journald_sink_create(const char *ident)` identical in header, source, test; `JOURNALD_MAX_MESSAGE` is `unsigned` vs `size_t msg_len` comparison — `msg_len > JOURNALD_MAX_MESSAGE` promotes safely (unsigned → size_t, no narrowing, `-Wconversion` clean); `send()` return `ssize_t` compared against `(ssize_t)off` (explicit cast, no implicit narrowing); `prio_len` int from `snprintf` checked `< 0` before use as `size_t`.

---

**Plan complete and saved to `docs/superpowers/plans/2026-10-08-journald-sink-plan.md`. Two execution options:**

**1. Subagent-Driven (recommended)** - I dispatch a fresh subagent per task, review between tasks, fast iteration

**2. Inline Execution** - Execute tasks in this session using executing-plans, batch execution with checkpoints

**Which approach?**
