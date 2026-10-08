# 热路径零分配化设计 (方案 A)

- 日期: 2026-10-09
- 状态: 设计已确认（三节评审通过），待实现计划
- 约束: 吞吐不回退优先于分配收益；ABI 稳定（不改公有头与公有布局）
- 背景: sync hot-path (+107%) 与 async producer fast-path (+20%) 已落地并刷新
  `benchmarks/.baseline`；`log_record_clone` 已有 256B inline。剩余每条分配:
  `socket_ring_put` 逐条 `malloc`、`redact_apply_one` 命中即 `malloc`。

## §1 目标与范围

目标：在吞吐不回退的前提下，消除热路径每条日志一次的 `malloc`，
降低分配次数、碎片和延迟抖动。

改动面（只动内部，不碰公有头）：

1. `core/socket_async.c` — `socket_ring_slot_t` 加小行 inline 缓冲
   （默认 512B，可配），短行零分配。
2. `core/log.c` — `redact_apply_one` 短消息走调用方栈缓冲，
   超长命中才 `malloc`。
3. formatter 输出缓冲线程局部复用（内部 `static __thread`，公有 API 不变）。

不碰：`log_record_t` / queue 公有布局、async 既有 256B inline 机制、公有 API 签名。

成功标准：`make benchmark` 吞吐不低于当前 baseline；短消息场景分配计数
约 0/条；`make check` 全绿。

## §2 数据流与归属协议

socket ring（核心变化）：

- `slot` 新增 `char inline_line[INLINE]` + `bool heap` 标记（内部结构体，可改）。
- `put`：`len <= INLINE` → `memcpy` 进 `inline_line`，`slot->line` 指向它，
  `heap = false`；超长 → `malloc`，`heap = true`
 （保留现有失败发布空 slot 逻辑）。
- `consumer`：`heap ? free(slot->line) : noop`，`slot->line = NULL` 后再推进
  `tail`。ordering 沿用现有 `seq` release-store，不新增屏障。
- `INLINE` 默认 512（覆盖绝大多数单行日志），创建时可配；
  `capacity × 512` 常驻增量在 YAML 文档注明（默认 8K 档约 +4MB）。

redact（次要变化）：

- 调用方栈上备 `char tmp[1024]`；`redact_apply_one` 改为"写调用方缓冲"语义：
  能装下 → 写 buf 返回指针；装不下 → 内部 `malloc`
 （保留 fail-open：失败返回 NULL，调用方用原文）。
- 零规则 fast path 不变（无 `strstr` 开销）。

formatter：输出缓冲 `static __thread char tls_buf[CLOG_MAX_FORMATTED_SIZE]` 复用（8192B，
见 `include/log_limits.h`），只影响内部
`format_*`；`log_reload` 改 format 字符串时只重编 opcode，不碰缓冲。

## §3 错误处理、测试与回滚

错误处理：

- inline 路径无失败点；堆路径 `malloc` 失败沿用现有语义：
  socket 发布空 slot + `dropped++` 返回 -1；redact 失败 fail-open 用原文。
  无新增失败模式。
- `INLINE=0` 配置视为"关闭 inline"（全堆），逻辑等价旧代码，即天然回滚开关。

测试：

1. 单测：短行往返（`line == inline` 指针、`heap = false`）、长行往返
   （`heap = true`、内容一致）、`malloc` 失败注入（空 slot + dropped）、
   redact 短/长/未命中三态。
2. 压力：8 producers × 短行，断言 `real == ok` 且 consumer 无泄漏（ASan）。
3. 回归：`make check` 全绿 + `make benchmark` 对 baseline（吞吐不回退才合入）。

回滚：任一门禁红 → 关 inline（`INLINE=0`）或 revert 单文件
（改动只涉 2–3 个 `.c` 内部，无头文件依赖）。

## 非目标

- slab 内存池中心化（方案 B，维护成本高，暂不做）。
- 激进 ABI 重构：`inline 256→1K`、queue 预分配大缓冲（方案 C，破 ABI，暂不做）。
- 二进制体积优化：本次只降分配次数，不做裁剪/section GC。
