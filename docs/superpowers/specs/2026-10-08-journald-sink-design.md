# journald Sink Design

> Status: approved (S1/S2/S3 section-by-section). Next: writing-plans → implementation.

**Goal:** Add a built-in journald sink that delivers clogx logs to systemd-journal with structured fields, in one line: `log_add_sink(journald_sink_create("my_app"))`.

**Approach:** Native `AF_UNIX`/`SOCK_DGRAM` client of `/run/systemd/journal/socket` per the official [Native Journal Protocol](https://systemd.io/JOURNAL_NATIVE_PROTOCOL). Zero new dependencies. API-only sink (factory + `log_add_sink`), following the syslog/OTLP precedent — no YAML config, no dispatcher/queue changes.

**Constraints:** C99, `-Wconversion` clean, `CLOGX_API` export for the single new factory, existing benchmarks untouched (sink is add-only; hot path has zero modifications).

---

## S1 — Scope, Interface, Success Criteria

### New files

- `sinks/journald_sink.c` — the sink (Linux-only implementation + `_WIN32`/`__APPLE__` NULL stub).
- `tests/test_journald_sink.c` — three-layer test (factory semantics, journal round-trip, skip paths).

### Modified files (exact registration points)

- `include/log_sink.h` — add `journald_sink_create` declaration with doxygen (next to `syslog_sink_create`).
- `Makefile:117` — append `journald_sink.c` to `SINK_SRCS`.
- `Makefile` TESTS list (after `test_syslog_sink`) — append `test_journald_sink`.
- `CMakeLists.txt:118` (after `sinks/syslog_sink.c`) — add `sinks/journald_sink.c`.
- `CMakeLists.txt:280` (after `test_syslog_sink`) — add `test_journald_sink`.

### Explicitly out of scope

- `core/config.c` (no YAML keys; API-only like syslog/OTLP).
- `core/dispatcher.c`, `core/queue.c`, `core/async.c` (zero hot-path changes; `benchmarks/.baseline` is NOT refreshed).
- Sealed-memfd channel for oversized entries (truncation instead, see S2).
- libsystemd linkage (rejected: new build dependency for ~30 fewer lines of code).
- External `.so` plugin packaging (rejected: premature; revisit when the sink ecosystem grows).

### Public API (one function)

```c
CLOGX_API log_sink_t *journald_sink_create(const char *ident);
```

- `ident`: journal `SYSLOG_IDENTIFIER` value. NULL defaults to `"clogx"`.
- Returns NULL when: allocation fails, `ident` contains `\n` (rejected at creation since journal keys/values framing forbids it in the identifier path), socket cannot be created/connected (no journal on this machine), or the platform is non-Linux.
- Ownership follows the standard sink contract: caller passes to `log_add_sink`/`logger_add_sink`; destroyed by `log_destroy`.

### Success criteria

- `make test` fully green; `make check` (format + clang-tidy) zero warnings; C99 + `-Wconversion` clean.
- On a journal machine: the round-trip test writes one ERROR record and reads it back via `journalctl` with matching token and `PRIORITY=3`.
- On journal-less CI / non-Linux: the test skips gracefully (exit 0, "skipped" notice), following the `test_syslog_sink.c` `#else` pattern.

---

## S2 — Wire Protocol Details

### Transport

- `socket(AF_UNIX, SOCK_DGRAM, 0)` created once in the factory, `connect()`-ed to `/run/systemd/journal/socket`, reused for the sink lifetime.
- One datagram per log record. `flush` is a no-op (datagrams are delivered on send). `destroy` closes the fd. `atfork_child` closes and re-creates the socket (same pattern as syslog's `closelog`/`openlog`).

### Datagram layout (journal native protocol)

Per-entry fields, in order:

```
PRIORITY=<0-7>\n
SYSLOG_IDENTIFIER=<ident>\n
MESSAGE=<formatted text, trailing newline stripped>\n
```

- `PRIORITY` mapping reuses the syslog table verbatim (`sinks/syslog_sink.c`): `[TRACE]`/`[DEBUG]`→7, `[INFO]`→6, `[WARN]`/`[WARNING]`→4, `[ERROR]`→3, `[FATAL]`→2, no prefix→6. Both sinks stay behavior-consistent.
- `MESSAGE` containing an embedded `\n` uses the binary-safe form: `MESSAGE\n` + 8-byte unaligned little-endian length + raw bytes + `\n`. This is the only field that takes the binary branch (`SYSLOG_IDENTIFIER` is validated `\n`-free at creation; `PRIORITY` is a single digit).
- Trailing `\n` of the formatted line is stripped before framing (the dispatcher appends one for stream sinks; journal entries must not carry it).

### Size policy

- Journal datagrams are size-limited; oversized entries are truncated: `MESSAGE` is cut to `240 * 1024` bytes and the sink returns the actually-sent byte count. Truncation (not silent drop, not memfd) is documented in the factory doxygen.

### Failure semantics

- Factory time, socket missing/unconnectable → factory returns NULL (caller falls back to `file_sink`; documented).
- Runtime `sendto` failure (e.g. journald restart window) → `write` returns -1; drop accounting stays with the dispatcher's existing dropped-counter logic, no in-sink retry (aligned with the socket sink's lazy philosophy, minus reconnect since datagrams are connectionless).

---

## S3 — Test Strategy and Gates

`tests/test_journald_sink.c`, three layers:

1. **Factory semantics (runs everywhere):** non-Linux → expect NULL + skip notice; Linux with socket → creation succeeds, `log_add_sink` returns 0; `ident` containing `\n` → factory returns NULL.
2. **Round-trip (journal machines only):** log one ERROR record with a unique token (`clogx_journald_<pid>_<seq>`), `log_flush`, then `journalctl --since "<test-start>" SYSLOG_IDENTIFIER=<ident> --grep <token>` must show the token with `PRIORITY=3`. A second record exercises the multi-line `MESSAGE` binary branch, verified via `journalctl -o verbose` showing the complete `MESSAGE` field. Missing socket or missing `journalctl` → whole layer skips, exit 0.
3. **No packet-level unit test:** rejected adding a test-only factory override (would widen the API surface). Framing correctness is covered by the round-trip layer.

### Gates

- Registered in both `make test` loop and ctest; TSan/ASan clean (no shared state on the send path — one fd per sink, serialized by the worker).
- `make check` green: clang-format + clang-tidy + the custom unused-includes check.
- `test_syslog_sink`, `test_otel`, and all existing tests untouched; no benchmark re-baselining (add-only change).

---

## Open verification notes (done during design, kept for the implementer)

- Protocol wording verified against systemd upstream `docs/JOURNAL_NATIVE_PROTOCOL.md` (datagram = one entry; `KEY=value\n`; binary form `KEY\n` + LE64 + data + `\n`; empty-payload + sealed-memfd is the only alternative channel — out of scope here).
- Verified on dev machine: `journalctl` (systemd 261), libsystemd, and `/run/systemd/journal/socket` all present, so the round-trip layer will execute locally.
- Registration line numbers above refer to master at the time of writing; re-check with `grep -n "syslog_sink" Makefile CMakeLists.txt` if they drift.
