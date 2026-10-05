# Span 全链路生命周期 (B 方向) Design Spec

- Date: 2026-10-06
- Approach: 方案 1（显式 Span 栈 + token）；方案 2（作用域宏）、方案 3（最小核心）备选未取。
- Status: presented §1–§5 on documented default; awaiting user review of this file.

## 0. 背景与目标

clogx 已有单线程内的 trace 上下文能力：thread-local `set/get/clear`、
`clog_set_trace_context_hex`（入站 `traceparent` 解析）、`TRACEPARENT` 环境变量解析
（`core/formatter.c:parse_traceparent`）、JSON 输出非零 `trace_id`/`span_id`
（`core/formatter.c:481-490`）、MDC 线程 map。缺的是**出站传播 + span 派生/嵌套/跨线程**，
即真正的全链路。本 spec 只做这一块。

Goal: 调用方能 `start`（入栈）/`end`（出栈）管理嵌套 span，用 `export`/`join`
把上下文传给下游服务或工作线程；每条 log record 自动带对 ids（复用现有快照逻辑）。

Non-goals: span 命名/span 事件上报（属于 tracer，不属于 logger）、采样决策、
`start` 之外的 RNG 可注入性、Windows 专用实现（现有可移植代码复用即可）。

## 1. API 表面（新增 4 个公开符号）

```c
typedef uint32_t clog_span_t;                              /* token，0 = 非法 */
CLOGX_API clog_span_t clog_span_start(void);
CLOGX_API clogx_errno_t clog_span_end(clog_span_t tok);
CLOGX_API clogx_errno_t clog_span_export(char *buf, size_t len);
CLOGX_API clog_span_t clog_span_join(const char *traceparent);
```

- 刻意无 `name` 参数（YAGNI；见 Non-goals）。
- RNG 生成器为内部函数，不占 ABI。
- 声明放 `include/log.h` 现有 trace 区之后，Doxygen 英文注释与现有风格一致。

## 2. 栈语义（向后兼容是硬要求）

- 线程内定长栈，深度 **16**；槽位 = `trace_id[16]` + `span_id[8]` + `flags(1)`，
  静态数组，零堆分配。与现有 thread-local trace 上下文存同一处。
- `clog_set_trace_context[_hex]` 写**栈底 base**；`start` 继承 base/栈顶的 trace_id、
  生成全新 span_id 后压栈；栈空时 `get` 返回 base —— **从不调新 API 的老用户行为逐字节不变**。
- `clog_clear_trace_context` 同时清空 base 和栈（请求边界语义）。
- `end` 必须 LIFO：token 编码为压栈后的栈深度（1-based），`end` 要求
  `tok == 当前深度`，否则返回 `CLOG_ERR_INVALID_ARG` 且**不改任何状态**。
  `clear` 后深度归零，旧 token 自然全部失效。
- 栈满时 `start` 返回 0（非法 token），不崩。
- async 队列在 log-call 时刻快照 ids（现有 formatter 逻辑），天然线程安全；
  跨线程只走显式 `export`/`join`，无线程间魔法。

## 3. span_id 生成（内部）

- 优先级：`getrandom()`（Linux）→ `getentropy()`（macOS）→ fallback xorshift64
 （seed = 时间ns ^ pid ^ 自增计数器）。文档写明：span id 要唯一性而非密码学强度。
- 全零规避：8 字节全零则最后一位置 `0x01`（确定性，无循环）。
- `-Wconversion` 下窄化一律显式 cast。
- 测试不断言具体值，只断言非零 + child ≠ parent，故无需 RNG 注入。

## 4. Handoff 格式（复用，不发明）

- 沿用 W3C `traceparent`：`00-<32hex>-<16hex>-<flags>`，复用现有 hex 编解码 helper。
- `export` 要求 `len >= 56`（55 字符 + NUL），`buf == NULL` 或不足返回
  `CLOG_ERR_INVALID_ARG`。
- `join` 按 `parse_traceparent` 同款规则校验版本/长度/hex 合法性；非法返回
  `CLOG_ERR_INVALID_ARG` 且不改状态；合法则设为 parent 并**直接派生 child 入栈**、
  返回 token（服务端 accept 一步到位）。

## 5. ABI / 版本 / 测试 / 文档影响

- ABI：+4 符号同步进 `clogx.map` global 块 + `clogx.exports`
  （`scripts/check_abi_exports.sh` 双向校验，macOS 下划线修饰由构建 existing 逻辑处理），
  符号数 67 → 71。
- 版本：向后兼容新增 ⇒ **MINOR bump**（CONTRIBUTING 语义化版本规则）；
  实际 bump 走 `scripts/release.sh`，留给 plan 阶段执行，不在本 spec 动 `VERSION`。
- 测试：新建 `tests/test_span_lifecycle.c`（无框架纯 C；同时注册 Makefile `TESTS`
  与 CMake `CLOG_TEST_SOURCES`，参考 `tests/test_otel.c` 现有 trace 用例风格）。
  用例：嵌套 ids 正确（child 继承 trace_id、span_id 不同且非零）、错序 `end`
  报错且状态不动、16 深溢出返回 0、`export`/`join` roundtrip、
  非法 `traceparent` 被拒且状态不动、`clear` 连栈清空、老 `set/get` 行为不变。
- 文档：`docs/user_manual.md` 加一小节（span 模型 + 跨线程 recipe）；
  `CHANGELOG.md` Unreleased 下 `### Added` 记一笔。
- 验证门：`make check` 全绿（含 format/tidy/ABI 校验 + 全测试），与 Q3 周期同一标准。
