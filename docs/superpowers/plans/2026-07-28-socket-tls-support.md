# Socket Sink TLS Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add optional OpenSSL TLS encryption support for socket sink via `#ifdef CLOG_USE_TLS` with Makefile (`TLS=1`) and CMake (`CLOG_ENABLE_TLS=ON`) integration.

**Architecture:** Extend `log_config_t` and `core/config.c` parser with `socket_tls`, `socket_tls_ca_file`, `socket_tls_skip_verify`. Refactor `sinks/socket_sink.c` to wrap TCP connection with OpenSSL `SSL_CTX` / `SSL_connect` / `SSL_write` when `CLOG_USE_TLS` is defined.

**Tech Stack:** C99, POSIX Sockets, OpenSSL (`-lssl -lcrypto`), Make, CMake.

---

### Task 1: Extend `log_config_t`, `log_sink.h`, and `core/config.c` for TLS Options

**Files:**
- Modify: `include/log_config.h:40-48`
- Modify: `include/log_sink.h:45-65`
- Modify: `core/config.c:42-260`
- Test: `tests/test_socket_sink.c`

- [ ] **Step 1: Update `include/log_config.h` with TLS fields**

Add `socket_tls`, `socket_tls_ca_file`, and `socket_tls_skip_verify` to `log_config_t`.

- [ ] **Step 2: Update `include/log_sink.h` with `socket_sink_create_tls` prototype**

Add `socket_sink_create_tls(const char *host, int port, bool use_tls, const char *ca_file, bool skip_verify)` prototype.

- [ ] **Step 3: Update `core/config.c` parser**

Add handlers for `socket_tls`, `socket_tls_ca_file`, `tls_ca_file`, `socket_tls_skip_verify`, `tls_skip_verify` in `g_config_keys` and `parse_config_file()`. Pass these parameters when creating socket sink in `log_dispatcher_init` and `log_dispatcher_build_snapshot`.

- [ ] **Step 4: Verify build**

Run: `make clean && make test`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/log_config.h include/log_sink.h core/config.c core/dispatcher.c
git commit -m "feat(config): add socket TLS configuration options and parser support"
```

---

### Task 2: Implement TLS Encryption in `sinks/socket_sink.c`

**Files:**
- Modify: `sinks/socket_sink.c`
- Test: `tests/test_socket_sink.c`

- [ ] **Step 1: Write test case in `tests/test_socket_sink.c`**

Add unit test checking `socket_sink_create_tls` initialization with plain TCP fallback and parameters.

- [ ] **Step 2: Implement OpenSSL TLS connection and IO logic in `sinks/socket_sink.c`**

Wrap `socket_connect`, `socket_write`, `socket_destroy` with `#ifdef CLOG_USE_TLS` for `SSL_CTX`, `SSL_connect`, `SSL_write`, `SSL_shutdown`, `SSL_free`. Add non-TLS warning fallback when `CLOG_USE_TLS` is not defined.

- [ ] **Step 3: Verify build and test**

Run: `make clean && make test`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add sinks/socket_sink.c tests/test_socket_sink.c
git commit -m "feat(sink): implement optional OpenSSL TLS support in socket_sink"
```

---

### Task 3: Update `Makefile` and `CMakeLists.txt` for TLS Build Support

**Files:**
- Modify: `Makefile`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Update `Makefile` with `TLS ?= 0` rule**

Add `TLS ?= 0` check. When `TLS=1`, append `-DCLOG_USE_TLS` to `CFLAGS` and `-lssl -lcrypto` to `LDFLAGS`.

- [ ] **Step 2: Update `CMakeLists.txt` with `CLOG_ENABLE_TLS` option**

Add `option(CLOG_ENABLE_TLS "Enable OpenSSL TLS support for socket sink" OFF)` and `find_package(OpenSSL REQUIRED)` logic. Add `include/log_limits.h` to `PUBLIC_HEADERS`.

- [ ] **Step 3: Verify Makefile TLS build**

Run: `make clean && make TLS=1 test`
Expected: PASS

- [ ] **Step 4: Verify CMake build**

Run: `cmake -B build_cmake -DCLOG_ENABLE_TLS=ON && cmake --build build_cmake`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add Makefile CMakeLists.txt
git commit -m "build: add TLS build support to Makefile and CMakeLists.txt"
```

---

### Task 4: Final Verification & Clean Check

- [ ] **Step 1: Run full check**

Run: `make check`
Expected: ALL PASS

- [ ] **Step 2: Clean up CMake build dir**

Run: `rm -rf build_cmake`
Expected: PASS
