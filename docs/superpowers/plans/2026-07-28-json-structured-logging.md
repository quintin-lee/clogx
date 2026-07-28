# Native JSON Structured Logging Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement native JSON structured logging (`format: "json"`) rendering valid, single-line JSON log objects with RFC 8259 string escaping.

**Architecture:** Extend `log_config_t` and `core/config.c` with `LOG_FORMAT_JSON` mode. Add JSON string escaper and JSON formatting logic in `core/formatter.c`.

**Tech Stack:** C99, POSIX.

---

### Task 1: Add `log_format_type_t` and Config Parser Extensions

**Files:**
- Modify: `include/log_config.h:20-45`
- Modify: `core/config.c:130-220`
- Test: `tests/test_pipeline.c`

- [ ] **Step 1: Update `include/log_config.h`**

Add `log_format_type_t` enum (`LOG_FORMAT_TEXT`, `LOG_FORMAT_JSON`) and `log_format_type_t format_type` field in `log_config_t`.

- [ ] **Step 2: Update `core/config.c`**

In `parse_config_file()`, set `cfg->format_type = LOG_FORMAT_JSON` when `format` equals `"json"` or `"JSON"`.

- [ ] **Step 3: Verify build**

Run: `make clean && make test`
Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add include/log_config.h core/config.c
git commit -m "feat(config): add log_format_type_t and format: json parser support"
```

---

### Task 2: Implement JSON Escaper and JSON Formatter in `core/formatter.c`

**Files:**
- Modify: `include/log_formatter.h:20-55`
- Modify: `core/formatter.c:40-190`
- Create: `tests/test_json_formatter.c`
- Modify: `Makefile`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add `test_json_formatter.c`**

Create `tests/test_json_formatter.c` testing JSON output structure, quote/newline/backslash escaping, and numeric fields.

- [ ] **Step 2: Add JSON escaping and formatting in `core/formatter.c`**

Implement `append_json_escaped_string()` and `format_json()` in `core/formatter.c`. When `format` is `"json"`, `log_formatter_format()` delegates to `format_json()`.

- [ ] **Step 3: Register `test_json_formatter` in `Makefile` and `CMakeLists.txt`**

Add `test_json_formatter` to `TESTS` list in `Makefile` and `CLOG_TEST_SOURCES` in `CMakeLists.txt`.

- [ ] **Step 4: Verify test suite**

Run: `make clean && make test`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add include/log_formatter.h core/formatter.c tests/test_json_formatter.c Makefile CMakeLists.txt
git commit -m "feat(formatter): implement native JSON structured logging format"
```

---

### Task 3: Full System Verification

- [ ] **Step 1: Run standard test suite and format check**

Run: `make check`
Expected: ALL PASS

- [ ] **Step 2: Run ASan test suite**

Run: `make test-asan`
Expected: ALL PASS

- [ ] **Step 3: Run UBSan test suite**

Run: `make test-ubsan`
Expected: ALL PASS
