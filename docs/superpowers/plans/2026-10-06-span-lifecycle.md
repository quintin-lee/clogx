# Span 全链路生命周期 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 `clog_span_start/end/export/join` 四个公开 API（显式 span 栈），让嵌套 span 与跨线程/跨服务透传可测可用。

**Architecture:** 复用现有 thread-local trace 上下文作为栈底 base；在 `core/log.c` 新增深度 16 的 thread-local 槽位栈（零堆分配）；`get/clear`/log-call 快照三处读路径改为"栈顶优先、栈空回落 base"，老用户行为逐字节不变；RNG 按 `getrandom→getentropy→xorshift` 降级。

**Tech Stack:** C99 (`-std=c99 -Wall -Wextra -Wconversion -D_GNU_SOURCE -fvisibility=hidden`)，断言式无框架测试，既有 `check_abi_exports.sh` 双向 ABI 校验。

**Spec:** `docs/superpowers/specs/2026-10-06-span-lifecycle-design.md`（§1–§5；含 flags 修正：base 恒 `0x00`，start 继承父 flags，join 原样透传入站 flags）。

**Spec deviation（本 plan 锁定，执行时不再讨论）：** spec §4 文字写 join"非法返回 `CLOG_ERR_INVALID_ARG`"，但 §1 签名返回 `clog_span_t` 且 typedef 注释明确"0 = 非法"，返回 `-1` 会与之矛盾。故非法输入/栈满时 `clog_span_join` 返回 `0`（与 `start` 栈满返回 0 一致）。另有一处 spec 未覆盖的派生语义：无任何上下文时 `start` 生成全新 trace_id 作为 root span（否则 start 在无 set 时不可用），flags 取 `0x00`。

---

### Task 1: 新建失败测试 + 注册到双构建系统（红灯，不提交）

**Files:**
- Create: `tests/test_span_lifecycle.c`
- Modify: `Makefile` (TESTS 列表，`test_kv_logging` 后追加)
- Modify: `CMakeLists.txt` (`CLOG_TEST_SOURCES`，`test_kv_logging` 后追加；非 INTERNAL 测试，走常规 `clogx` 链接)

- [ ] **Step 1: 写测试文件**（风格仿 `tests/test_otel.c`：`assert` + 每个用例末 `printf`，`main` 顺序调用；每个用例开头 `clog_clear_trace_context()` 隔离 thread-local 状态）

```c
/**
 * @file test_span_lifecycle.c
 * @brief Unit tests for explicit span stack: start/end/export/join.
 */

#include "clog_port.h"
#include "log.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int is_zero(const uint8_t *id, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (id[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static void test_nested_ids(void)
{
    uint8_t tid[16];
    uint8_t sid[8];
    uint8_t child_tid[16];
    uint8_t child_sid[8];
    clog_span_t t1;
    clog_span_t t2;

    clog_clear_trace_context();
    t1 = clog_span_start();
    assert(t1 == 1);
    clog_get_trace_context(tid, sid);
    assert(!is_zero(tid, 16));
    assert(!is_zero(sid, 8));

    t2 = clog_span_start();
    assert(t2 == 2);
    clog_get_trace_context(child_tid, child_sid);
    assert(memcmp(tid, child_tid, 16) == 0);
    assert(memcmp(sid, child_sid, 8) != 0);
    assert(!is_zero(child_sid, 8));

    assert(clog_span_end(t2) == CLOG_OK);
    assert(clog_span_end(t1) == CLOG_OK);
    printf("test_nested_ids passed\n");
}

static void test_out_of_order_end(void)
{
    uint8_t before[8];
    uint8_t after[8];
    uint8_t tid[16];
    clog_span_t t1;
    clog_span_t t2;

    clog_clear_trace_context();
    t1 = clog_span_start();
    t2 = clog_span_start();
    clog_get_trace_context(tid, before);

    assert(clog_span_end(t1) == CLOG_ERR_INVALID_ARG);
    clog_get_trace_context(tid, after);
    assert(memcmp(before, after, 8) == 0);

    assert(clog_span_end(t2) == CLOG_OK);
    assert(clog_span_end(t1) == CLOG_OK);
    assert(clog_span_end(t1) == CLOG_ERR_INVALID_ARG);
    assert(clog_span_end(0) == CLOG_ERR_INVALID_ARG);
    printf("test_out_of_order_end passed\n");
}

static void test_depth_overflow(void)
{
    clog_span_t toks[16];
    int i;

    clog_clear_trace_context();
    for (i = 0; i < 16; i++) {
        toks[i] = clog_span_start();
        assert(toks[i] == (clog_span_t)(i + 1));
    }
    assert(clog_span_start() == 0);
    for (i = 15; i >= 0; i--) {
        assert(clog_span_end(toks[i]) == CLOG_OK);
    }
    assert(clog_span_end(toks[0]) == CLOG_ERR_INVALID_ARG);
    printf("test_depth_overflow passed\n");
}

static void test_export_join_roundtrip(void)
{
    char outward[64];
    char reinward[64];
    uint8_t tid1[16];
    uint8_t sid1[8];
    uint8_t tid2[16];
    uint8_t sid2[8];
    clog_span_t t1;
    clog_span_t t2;

    clog_clear_trace_context();
    t1 = clog_span_start();
    assert(clog_span_export(outward, sizeof(outward)) == CLOG_OK);
    assert(strlen(outward) == 55);
    assert(outward[0] == '0' && outward[1] == '0' && outward[2] == '-');

    t2 = clog_span_join(outward);
    assert(t2 == 2);
    clog_get_trace_context(tid2, sid2);
    clog_span_end(t2);
    clog_get_trace_context(tid1, sid1);
    assert(memcmp(tid1, tid2, 16) == 0);
    assert(memcmp(sid1, sid2, 8) != 0);

    assert(clog_span_export(reinward, sizeof(reinward)) == CLOG_OK);
    assert(memcmp(outward, reinward, 36) == 0);
    assert(memcmp(outward + 36, reinward + 36, 16) != 0);

    assert(clog_span_end(t1) == CLOG_OK);
    printf("test_export_join_roundtrip passed\n");
}

static void test_flags_passthrough(void)
{
    char outward[64];
    clog_span_t t;

    clog_clear_trace_context();
    t = clog_span_join("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
    assert(t == 1);
    assert(clog_span_export(outward, sizeof(outward)) == CLOG_OK);
    assert(strcmp(outward, "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01") != 0);
    assert(strstr(outward, "4bf92f3577b34da6a3ce929d0e0e4736") != NULL);
    assert(strcmp(outward + 53, "01") == 0);
    assert(clog_span_end(t) == CLOG_OK);
    printf("test_flags_passthrough passed\n");
}

static void test_export_rejected_when_empty(void)
{
    char buf[64];
    char sentinel[64];

    clog_clear_trace_context();
    memset(sentinel, 0x5a, sizeof(sentinel));
    memcpy(buf, sentinel, sizeof(buf));
    assert(clog_span_export(buf, sizeof(buf)) == CLOG_ERR_INVALID_ARG);
    assert(memcmp(buf, sentinel, sizeof(buf)) == 0);
    assert(clog_span_export(NULL, 64) == CLOG_ERR_INVALID_ARG);
    assert(clog_span_export(buf, 55) == CLOG_ERR_INVALID_ARG);
    printf("test_export_rejected_when_empty passed\n");
}

static void test_join_rejected(void)
{
    uint8_t tid[16];
    uint8_t sid[8];

    clog_clear_trace_context();
    assert(clog_span_join(NULL) == 0);
    assert(clog_span_join("garbage") == 0);
    assert(clog_span_join("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7") == 0);
    assert(clog_span_join("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902BX-01") == 0);
    assert(clog_span_join("00_4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01") == 0);
    clog_get_trace_context(tid, sid);
    assert(is_zero(tid, 16));
    assert(is_zero(sid, 8));
    printf("test_join_rejected passed\n");
}

static void test_clear_empties_stack(void)
{
    uint8_t tid[16];
    uint8_t sid[8];
    clog_span_t t1;
    clog_span_t t2;

    clog_clear_trace_context();
    t1 = clog_span_start();
    t2 = clog_span_start();
    (void)t1;
    clog_clear_trace_context();
    clog_get_trace_context(tid, sid);
    assert(is_zero(tid, 16));
    assert(is_zero(sid, 8));
    assert(clog_span_end(t2) == CLOG_ERR_INVALID_ARG);
    assert(clog_span_export(NULL, 0) == CLOG_ERR_INVALID_ARG);
    printf("test_clear_empties_stack passed\n");
}

static void test_legacy_set_get_unchanged(void)
{
    uint8_t tid[16];
    uint8_t sid[8];
    uint8_t base_tid[16];
    uint8_t base_sid[8];
    clog_span_t t;

    clog_clear_trace_context();
    assert(clog_set_trace_context_hex("4bf92f3577b34da6a3ce929d0e0e4736",
                                      "00f067aa0ba902b7") == CLOG_OK);
    clog_get_trace_context(base_tid, base_sid);
    assert(base_tid[0] == 0x4b && base_sid[1] == 0xf0);

    t = clog_span_start();
    clog_get_trace_context(tid, sid);
    assert(memcmp(tid, base_tid, 16) == 0);
    assert(memcmp(sid, base_sid, 8) != 0);
    assert(clog_span_end(t) == CLOG_OK);
    clog_get_trace_context(tid, sid);
    assert(memcmp(tid, base_tid, 16) == 0);
    assert(memcmp(sid, base_sid, 8) == 0);

    clog_clear_trace_context();
    printf("test_legacy_set_get_unchanged passed\n");
}

int main(void)
{
    test_nested_ids();
    test_out_of_order_end();
    test_depth_overflow();
    test_export_join_roundtrip();
    test_flags_passthrough();
    test_export_rejected_when_empty();
    test_join_rejected();
    test_clear_empties_stack();
    test_legacy_set_get_unchanged();
    printf("all span lifecycle tests passed!\n");
    return 0;
}
```

- [ ] **Step 2: 注册到 Makefile** — `TESTS` 列表末尾 `test_kv_logging` 后追加 `test_span_lifecycle`（同一行续接，反斜杠换行风格不变）。

- [ ] **Step 3: 注册到 CMakeLists.txt** — `CLOG_TEST_SOURCES` 末尾 `test_kv_logging` 后追加 `test_span_lifecycle`（常规链接，不进 `CLOG_INTERNAL_TESTS`，只用公开 API）。

- [ ] **Step 4: 构建验证红灯** — 从 repo root 运行：

```bash
make build/test_span_lifecycle
```

Expected: 编译测试文件本身成功（只依赖 `log.h` 声明，暂无），链接失败 `undefined reference to clog_span_start`（及 end/export/join）。红灯符合预期，**不提交**（红状态不进 git）。

---

### Task 2: 公开头声明（`include/log.h`）

**Files:**
- Modify: `include/log.h`（`clog_set_trace_context_hex` 声明 `);` 之后、第 627 行 plugin 注释之前插入；`uint32_t` 可用，`stdint.h` 已包含）

- [ ] **Step 1: 插入声明块**（Doxygen 英文注释，与现有 trace 区风格一致；`@ref` 指向既有 setter）

```c
/**
 * @brief Opaque token identifying a pushed span frame (1-based stack depth).
 *
 * Value 0 is never a valid token; it signals "no span" (stack full on
 * @ref clog_span_start, or rejected input on @ref clog_span_join).
 */
typedef uint32_t clog_span_t;

/**
 * @brief Push a new child span frame onto the calling thread's span stack.
 *
 * Inherits the parent frame's trace ID (or the thread-local base context set
 * via @ref clog_set_trace_context when the stack is empty; a fresh trace ID
 * is generated when no context exists at all) and assigns a newly generated
 * span ID. The parent frame's flags are inherited. Log records written while
 * this frame is on top carry its IDs.
 *
 * @return 1-based depth token for @ref clog_span_end, or 0 when the stack
 * (depth 16) is full. Never fails otherwise.
 *
 * Note Thread-local: only affects the calling thread. Zero heap allocation.
 */
CLOGX_API clog_span_t clog_span_start(void);

/**
 * @brief Pop the top span frame. Must be called LIFO with the matching token.
 *
 * @param[in] tok Token returned by @ref clog_span_start or @ref clog_span_join.
 * @return CLOG_OK on success, CLOG_ERR_INVALID_ARG when `tok` is 0 or does not
 * equal the current depth. On failure no state is modified.
 */
CLOGX_API clogx_errno_t clog_span_end(clog_span_t tok);

/**
 * @brief Export the current trace context as a W3C `traceparent` string.
 *
 * Exports the top frame's IDs (or the base context when the stack is empty)
 * in `00-<32hex>-<16hex>-<flags>` form for propagation to downstream services
 * or worker threads (pair with @ref clog_span_join).
 *
 * @param[out] buf Caller buffer receiving the NUL-terminated string.
 * @param[in] len Buffer size in bytes; must be >= 56 (55 chars + NUL).
 * @return CLOG_OK on success, CLOG_ERR_INVALID_ARG when `buf` is NULL,
 * `len` < 56, or no active (non-zero trace ID) context exists. On failure
 * `buf` is left untouched.
 */
CLOGX_API clogx_errno_t clog_span_export(char *buf, size_t len);

/**
 * @brief Accept an inbound `traceparent` and push a derived child frame.
 *
 * Validates dash positions/length/hex with the same strictness as the
 * `traceparent` handling in the formatter; the inbound flags field is
 * preserved verbatim (no sampling decision is made). The inbound span ID is
 * validated but not retained (frames carry their own span ID).
 *
 * @param[in] traceparent `00-<32hex>-<16hex>-<flags>` string, >= 55 chars.
 * @return Token for @ref clog_span_end, or 0 on NULL/short/malformed input
 * or a full stack. On failure no state is modified.
 */
CLOGX_API clog_span_t clog_span_join(const char *traceparent);
```

- [ ] **Step 2: 头文件语法验证**

```bash
cc -std=c99 -Wall -Wextra -Wconversion -Iinclude -fsyntax-only include/log.h
```

Expected: exit 0，无输出。**不提交**（单独头文件提交无意义，随实现一起提交）。

---

### Task 3: 核心实现（`core/log.c`）：栈状态 + RNG + start/end

**Files:**
- Modify: `core/log.c`（第 65–67 行 thread-local 区之后加栈状态；`clog_get_trace_context`/`clog_clear_trace_context` 改造；两处快照点 `301–307`/`423–429` 改调 helper；`set` 函数零改动）

- [ ] **Step 1: 加栈状态与内部 helper**（紧随第 67 行之后；`bool` 可用，`string.h` 已包含；`<sys/random.h>` 按平台守卫引入，clang-format sorted-include 顺序为 stdarg/stdio/stdlib/string/sys-random/time/unistd——本文件无 unistd 需求，不加）

```c
#if defined(__linux__) || defined(__APPLE__)
#include <sys/random.h>
#endif
```

插在 `#include <string.h>` 与 `#include <time.h>` 之间。状态与 helper：

```c
#define CLOG_SPAN_MAX_DEPTH 16

typedef struct {
    uint8_t trace_id[16];
    uint8_t span_id[8];
    uint8_t flags;
} clog_span_slot_t;

static clog_thread_local clog_span_slot_t g_span_stack[CLOG_SPAN_MAX_DEPTH];
static clog_thread_local unsigned           g_span_depth = 0;

static bool span_is_zero(const uint8_t *id, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (id[i] != 0) {
            return false;
        }
    }
    return true;
}

/* Top frame wins; empty stack falls back to the legacy base context. */
static void span_current_ids(uint8_t trace_id[16], uint8_t span_id[8])
{
    if (g_span_depth > 0) {
        const clog_span_slot_t *top = &g_span_stack[g_span_depth - 1u];
        memcpy(trace_id, top->trace_id, 16);
        memcpy(span_id, top->span_id, 8);
    } else if (g_has_thread_trace_context) {
        memcpy(trace_id, g_thread_trace_id, 16);
        memcpy(span_id, g_thread_span_id, 8);
    } else {
        memset(trace_id, 0, 16);
        memset(span_id, 0, 8);
    }
}

/* Uniqueness, not cryptographic strength (documented in the user manual). */
static void span_random_bytes(uint8_t *out, size_t n)
{
#if defined(__linux__)
    {
        ssize_t got = getrandom(out, n, 0);
        if (got == (ssize_t)n) {
            return;
        }
    }
#endif
#if defined(__APPLE__)
    if (getentropy(out, n) == 0) {
        return;
    }
#endif
    {
        uint64_t s = clog_get_timestamp_us() ^ ((uint64_t)clog_getpid() << 32) ^
                     (uint64_t)(uintptr_t)out;
        size_t   i = 0;
        if (s == 0) {
            s = 0x9e3779b97f4a7c15ULL;
        }
        while (i < n) {
            size_t chunk;
            size_t k;
            s ^= s << 13;
            s ^= s >> 7;
            s ^= s << 17;
            if (s == 0) {
                s = 0x9e3779b97f4a7c15ULL;
            }
            chunk = (n - i) < 8 ? (n - i) : 8;
            for (k = 0; k < chunk; k++) {
                out[i + k] = (uint8_t)((s >> (k * 8)) & 0xffU);
            }
            i += chunk;
        }
    }
}

static void span_new_span_id(uint8_t span_id[8])
{
    span_random_bytes(span_id, 8);
    if (span_is_zero(span_id, 8)) {
        span_id[7] = 0x01;
    }
}
```

- [ ] **Step 2: 实现 start/end**（放在 `clog_clear_trace_context` 定义之后、`parse_hex_nibble` 之前；复用同文件 `memcpy`/`memset` 风格）

```c
clog_span_t clog_span_start(void)
{
    uint8_t trace_id[16];
    uint8_t flags = 0x00;

    if (g_span_depth >= CLOG_SPAN_MAX_DEPTH) {
        return 0;
    }
    if (g_span_depth > 0) {
        const clog_span_slot_t *top = &g_span_stack[g_span_depth - 1u];
        memcpy(trace_id, top->trace_id, 16);
        flags = top->flags;
    } else if (g_has_thread_trace_context) {
        memcpy(trace_id, g_thread_trace_id, 16);
    } else {
        span_random_bytes(trace_id, 16);
        if (span_is_zero(trace_id, 16)) {
            trace_id[15] = 0x01;
        }
    }

    {
        clog_span_slot_t *slot = &g_span_stack[g_span_depth];
        memcpy(slot->trace_id, trace_id, 16);
        span_new_span_id(slot->span_id);
        slot->flags = flags;
        g_span_depth++;
    }
    return (clog_span_t)g_span_depth;
}

clogx_errno_t clog_span_end(clog_span_t tok)
{
    if (tok == 0 || tok != (clog_span_t)g_span_depth) {
        return CLOG_ERR_INVALID_ARG;
    }
    {
        clog_span_slot_t *top = &g_span_stack[g_span_depth - 1u];
        memset(top, 0, sizeof(*top));
    }
    g_span_depth--;
    return CLOG_OK;
}
```

- [ ] **Step 3: 改造 get/clear/两处快照**（三处精确替换；`set` 系列不动）

替换 `clog_get_trace_context` 函数体为：

```c
void clog_get_trace_context(uint8_t trace_id[16], uint8_t span_id[8])
{
    if (g_span_depth > 0) {
        const clog_span_slot_t *top = &g_span_stack[g_span_depth - 1u];
        if (trace_id) {
            memcpy(trace_id, top->trace_id, 16);
        }
        if (span_id) {
            memcpy(span_id, top->span_id, 8);
        }
        return;
    }
    if (trace_id) {
        if (g_has_thread_trace_context) {
            memcpy(trace_id, g_thread_trace_id, 16);
        } else {
            memset(trace_id, 0, 16);
        }
    }
    if (span_id) {
        if (g_has_thread_trace_context) {
            memcpy(span_id, g_thread_span_id, 8);
        } else {
            memset(span_id, 0, 8);
        }
    }
}
```

替换 `clog_clear_trace_context` 函数体为：

```c
void clog_clear_trace_context(void)
{
    memset(g_thread_trace_id, 0, 16);
    memset(g_thread_span_id, 0, 8);
    memset(g_span_stack, 0, sizeof(g_span_stack));
    g_span_depth                  = 0;
    g_has_thread_trace_context = false;
}
```

（注意对齐：该文件用列对齐风格，`=` 对齐到同列；写完跑 `make format` 兜底。）

两处快照点（`logger_writevprintf_internal` 与 `logger_write_kv_internal` 内各一段 `if (g_has_thread_trace_context) {...} else {...}`）统一替换为一行：

```c
    span_current_ids(record.trace_id, record.span_id);
```

- [ ] **Step 4: 构建并跑新测试**

```bash
make build/test_span_lifecycle && ./build/test_span_lifecycle
```

Expected: 构建成功，输出 9 行 `... passed` + `all span lifecycle tests passed!`，exit 0。**不提交**（export/join 缺失会导致 roundtrip 用例链接失败——本步预期仍然红灯；若全绿说明 Task 4 代码已被误写，停下检查）。

---

### Task 4: 核心实现（`core/log.c`）：export/join，转绿提交

**Files:**
- Modify: `core/log.c`（接 Task 3 的 `clog_span_end` 之后）

- [ ] **Step 1: 实现 export/join**（hex 编码用本地小表，不引 formatter 内部静态函数；`-Wconversion` 下标一律显式 `(size_t)` cast）

```c
clogx_errno_t clog_span_export(char *buf, size_t len)
{
    static const char hexd[] = "0123456789abcdef";
    uint8_t           trace_id[16];
    uint8_t           span_id[8];
    uint8_t           flags = 0x00;
    size_t            pos   = 0;
    size_t            i;

    if (buf == NULL || len < 56) {
        return CLOG_ERR_INVALID_ARG;
    }
    span_current_ids(trace_id, span_id);
    if (g_span_depth > 0) {
        flags = g_span_stack[g_span_depth - 1u].flags;
    }
    if (span_is_zero(trace_id, 16)) {
        return CLOG_ERR_INVALID_ARG;
    }
    buf[pos++] = '0';
    buf[pos++] = '0';
    buf[pos++] = '-';
    for (i = 0; i < 16; i++) {
        buf[pos++] = hexd[(size_t)((trace_id[i] >> 4) & 0x0f)];
        buf[pos++] = hexd[(size_t)(trace_id[i] & 0x0f)];
    }
    buf[pos++] = '-';
    for (i = 0; i < 8; i++) {
        buf[pos++] = hexd[(size_t)((span_id[i] >> 4) & 0x0f)];
        buf[pos++] = hexd[(size_t)(span_id[i] & 0x0f)];
    }
    buf[pos++] = '-';
    buf[pos++] = hexd[(size_t)((flags >> 4) & 0x0f)];
    buf[pos++] = hexd[(size_t)(flags & 0x0f)];
    buf[pos++] = '\0';
    return CLOG_OK;
}

clog_span_t clog_span_join(const char *traceparent)
{
    uint8_t tid[16];
    uint8_t flags;
    size_t  i;

    if (traceparent == NULL || strlen(traceparent) < 55 ||
        g_span_depth >= CLOG_SPAN_MAX_DEPTH) {
        return 0;
    }
    if (traceparent[2] != '-' || traceparent[35] != '-' || traceparent[52] != '-') {
        return 0;
    }
    for (i = 0; i < 16; i++) {
        int hi = parse_hex_nibble(traceparent[3 + i * 2]);
        int lo = parse_hex_nibble(traceparent[3 + i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return 0;
        }
        tid[i] = (uint8_t)((hi << 4) | lo);
    }
    for (i = 0; i < 8; i++) {
        if (parse_hex_nibble(traceparent[36 + i * 2]) < 0 ||
            parse_hex_nibble(traceparent[36 + i * 2 + 1]) < 0) {
            return 0;
        }
    }
    {
        int hi = parse_hex_nibble(traceparent[53]);
        int lo = parse_hex_nibble(traceparent[54]);
        if (hi < 0 || lo < 0) {
            return 0;
        }
        flags = (uint8_t)((hi << 4) | lo);
    }

    {
        clog_span_slot_t *slot = &g_span_stack[g_span_depth];
        memcpy(slot->trace_id, tid, 16);
        span_new_span_id(slot->span_id);
        slot->flags = flags;
        g_span_depth++;
    }
    return (clog_span_t)g_span_depth;
}
```

（说明：parent span 字段只做合法性校验、不存储——slot 无 parent 位，spec §2 槽位定义如此；校验失败直接 return 0，状态不动。）

- [ ] **Step 2: 构建并跑新测试转绿**

```bash
make build/test_span_lifecycle && ./build/test_span_lifecycle
```

Expected: 9 行 passed + `all span lifecycle tests passed!`，exit 0。

- [ ] **Step 3: 回归旧 trace 测试**

```bash
make build/test_otel && ./build/test_otel
```

Expected: `all otel tests passed!`，exit 0（快照/get/clear 改造未破坏老行为）。

- [ ] **Step 4: format + 提交**

```bash
make format && git add include/log.h core/log.c tests/test_span_lifecycle.c Makefile CMakeLists.txt && git commit -m "feat(trace): ✨ add explicit span stack start/end/export/join"
```

Expected: commit 成功（message 仿既有 `feat` + gitmoji 风格；若 hook 报 format 问题，先修再提交）。

---

### Task 5: ABI 文件 + 导出校验（67→71）

**Files:**
- Modify: `clogx.map`（`clog_set_trace_context_hex;` 之后、`console_sink_create;` 之前插入 4 行，字母序）
- Modify: `clogx.exports`（相同相对位置插入相同 4 行；`check_abi_exports.sh` 要求两文件逐行一致）

- [ ] **Step 1: 两文件各加 4 行**

```
    clog_span_end;
    clog_span_export;
    clog_span_join;
    clog_span_start;
```

（map 文件带 4 空格缩进 + 分号；exports 文件无缩分号，照原格式。）

- [ ] **Step 2: 重建 .so 并校验**

```bash
make build/libclogx.so && ./scripts/check_abi_exports.sh
```

Expected: `OK: ABI exports match (71 symbols)`。若报 `NOT exported` 说明实现函数缺 `CLOGX_API` 或拼写错；若报 `NOT listed` 说明漏加或顺序错——修到 OK 为止，不绕过。

- [ ] **Step 3: 提交**

```bash
git add clogx.map clogx.exports && git commit -m "feat(abi): ✨ expose 4 span symbols, 67→71"
```

---

### Task 6: 文档（manual §6.7 + CHANGELOG）+ 提交

**Files:**
- Modify: `docs/user_manual.md`（TOC `66` 行后加 `67` 条目；`66` 正文 SIGPIPE 段落后、`---` + `## 7. API Reference` 之前加小节；不碰 mermaid 图）
- Modify: `CHANGELOG.md`（`## [Unreleased]` 下现有 `### Added` 块追加一笔）

- [ ] **Step 1: TOC 加条目** — `   - [Signal Handling...](#66-...)` 行后加：

```
   - [Span Lifecycle and Cross-Thread Propagation](#67-span-lifecycle-and-cross-thread-propagation)
```

- [ ] **Step 2: 正文加 §6.7**（无 mermaid，`make check` 的 mermaid-check 不受影响）

```markdown
### 67 Span Lifecycle and Cross-Thread Propagation

Thread-local `set/get/clear` covers a single span. For nesting and handoff,
use the explicit span stack (depth 16, zero heap allocation):

```c
#include "log.h"

clog_span_t outer = clog_span_start();  /* inherits base trace, new span id */
clog_span_t inner = clog_span_start();  /* child of outer */
LOG_INFO("inside inner span");
clog_span_end(inner);                   /* LIFO: token must equal top depth */
clog_span_end(outer);
```

`clog_span_end` rejects out-of-order tokens with `CLOG_ERR_INVALID_ARG` and
leaves state untouched. `clog_clear_trace_context()` empties the base and the
whole stack (request-boundary semantics).

Cross-thread / cross-service handoff is explicit — no inter-thread magic.
Export in the producer, `join` in the consumer (server-side accept pushes a
derived child and returns its token in one step):

```c
/* producer thread / upstream service */
char tp[64];
if (clog_span_export(tp, sizeof(tp)) == CLOG_OK) {
    queue_push(tp);  /* hand the 55-char traceparent string over */
}

/* consumer thread / downstream service */
clog_span_t t = clog_span_join(tp);
if (t != 0) {
    LOG_INFO("handling request under propagated trace");
    clog_span_end(t);
}
```

Notes: span IDs favor uniqueness over cryptographic strength
(`getrandom`/`getentropy`, xorshift fallback); flags are passed through
verbatim, never decided; exporting with no active context is rejected.
```

- [ ] **Step 3: CHANGELOG 加条目** — `### Added` 块末追加：

```
- Span lifecycle API: `clog_span_start`/`clog_span_end` (explicit LIFO span stack, depth 16) plus `clog_span_export`/`clog_span_join` for W3C `traceparent` handoff across threads/services; ABI surface grows 67 → 71 symbols.
```

- [ ] **Step 4: 提交**

```bash
git add docs/user_manual.md CHANGELOG.md && git commit -m "docs: 📝 span lifecycle section and changelog"
```

---

### Task 7: MINOR bump（`scripts/release.sh`，不 push）

**Files:** 脚本自改（`VERSION` 等；工作区必须干净，Task 6 已提交保证此点）

- [ ] **Step 1: 先 dry-run 探路**

```bash
./scripts/release.sh --dry-run minor
```

Expected: exit 0 并打印将 bump 到 0.4.0 的预览。若 exit 非零（如本地 HEAD 与 `origin/master` 不同步被拒）：**停下并报告**，不强行 bump、不 `--push`，本 Task 标记为阻塞跳过（bump 留到发版时做，代码与文档不受影响）。

- [ ] **Step 2: 真跑（仅 dry-run 通过时）**

```bash
./scripts/release.sh minor && git log --oneline -1 && git tag --list 'v*' | tail -3
```

Expected: 新 commit + 本地 annotated tag（如 `v0.4.0`），无 `--push`（发版推送由用户决定）。

---

### Task 8: 全门禁 `make check` 收尾

- [ ] **Step 1: 全量验证**

```bash
make check
```

Expected: 以 `=== check passed ===` 结尾（含 format/tidy（如有工具）、ABI 校验、全测试；已知 mermaid-check 在本机缺 `mmdc` 时自动跳过——本次文档无 mermaid 变更，若它运行且失败，先用 `git stash` 在 pristine HEAD 复现，确认为预先存在环境问题则如实报告、不修）。

- [ ] **Step 2: 最终状态确认**

```bash
git status --short && git log --oneline -8
```

Expected: 工作区干净，一串 span 相关 commits +（若 Task 7 通过）release commit/tag。报告完成。
