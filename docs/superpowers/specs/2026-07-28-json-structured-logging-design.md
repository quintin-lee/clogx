# Design Spec: Native JSON Structured Logging

**Date**: 2026-07-28  
**Topic**: JSON Structured Logging Support (`format: "json"`)  
**Status**: Approved  

---

## 1. Overview

This design adds native single-line JSON structured logging capability to `clogx`. When configured with `format: "json"` (or `format: "JSON"`), log records are rendered as fully escaped, valid JSON objects containing all metadata fields (`timestamp`, `level`, `module`, `tag`, `file`, `line`, `func`, `thread`, `pid`, `message`), enabling log shippers like ELK, Loki, and Fluentd to ingest logs directly.

---

## 2. Configuration & API Extensions

### 2.1 Configuration Struct (`include/log_config.h`)

Add `log_format_type_t` enum and field to `log_config_t`:

```c
typedef enum {
    LOG_FORMAT_TEXT = 0,
    LOG_FORMAT_JSON
} log_format_type_t;

typedef struct {
    log_level_t level;
    bool async;
    int queue_size;
    bool color;
    log_format_type_t format_type; /**< LOG_FORMAT_TEXT or LOG_FORMAT_JSON */
    const char *format;
    const char *time_format;
    ...
} log_config_t;
```

### 2.2 YAML Parsing (`core/config.c`)

In `parse_config_file()`, when parsing `format` or `format_type`:
- If `val` equals `"json"` or `"JSON"`, set `cfg->format_type = LOG_FORMAT_JSON`.
- Otherwise set `cfg->format_type = LOG_FORMAT_TEXT`.

---

## 3. Formatting Engine (`core/formatter.c`)

### 3.1 JSON Escaper (`append_json_escaped_string`)

Implements RFC 8259 JSON string escaping:
- `"` -> `\"`
- `\` -> `\\`
- `\b` -> `\b`, `\f` -> `\f`, `\n` -> `\n`, `\r` -> `\r`, `\t` -> `\t`
- ASCII control characters `< 0x20` -> `\u00XX`

### 3.2 JSON Output Structure (`format_json`)

Outputs single-line JSON object:

```json
{"timestamp":"2026-07-28 14:44:25.123456","level":"INFO","module":"main","file":"test.c","line":42,"func":"main","thread":123,"pid":456,"tag":"auth","message":"hello \"world\"\nnext line"}
```

- Standard buffer boundary checks ensure safe truncation without buffer overflow.

---

## 4. Build System & Test Plan

1. **Unit Test (`tests/test_json_formatter.c`)**:
   - Tests JSON output structure, field presence, and escaping of quotes, backslashes, newlines, and control chars.
2. **Build Integration**:
   - Register `test_json_formatter` in `Makefile` and `CMakeLists.txt`.
3. **Verification**:
   - `make clean && make test`
   - `make test-asan`
   - `make test-ubsan`
