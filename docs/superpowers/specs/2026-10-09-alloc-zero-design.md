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

1. `core/socket_async.c` — `socket_ring_slot_t` 加 256B inline 缓冲
   （固定值，对齐 async 已有 256B inline 前例；短行零分配，超长走堆）。
   归属标记经内部 `socket_ring_get_batch` 批 API 传递（内部头，可改）。
2. `core/log.c` — KV 路径 `redact_apply_one` 拆为 count+copy 两阶段，
   短消息写调用方 512B ping-pong 栈缓冲，超长命中才 `malloc`。
   （文本路径已是原地改写，不动；STR KV 值保持堆路径不变。）
3. writer 线程换行 framing `malloc`（`core/socket_async.c:655,701`）→
   512B 栈缓冲 + 超长回退堆。
   （修正：`core/formatter.c` 经 grep 确认零堆分配，原 formatter/TLS
   两项取消；TLS 发送路径亦无逐条分配。）

不碰：`log_record_t` / queue 公有布局、async 既有 256B inline 机制、公有 API 签名。

成功标准：`make benchmark` 吞吐不低于当前 baseline；短消息场景分配计数
约 0/条；`make check` 全绿。

## §2 数据流与归属协议

socket ring（核心变化）：

- `slot` 新增 `char inline_buf[256]` + `int heap` 标记（内部结构体，可改）。
- `put`：`len < 256` → `memcpy` 进 `inline_buf`，`slot->line` 指向它，
  `heap = 0`；超长 → `malloc`，`heap = 1`
  （保留现有失败发布空 slot 逻辑）。
- `consumer`：`heap ? free : noop`（标记经批 API 传递，`destroy` 余量同理），
  ordering 沿用现有 `seq` release-store，不新增屏障。
- 固定 256（对齐 async inline 前例；默认 8K 档常驻增量约 +2MB，
  在"减体积/资源占用"维度下优于 512；不做配置项，YAGNI）。

redact（次要变化）：

- `logger_write_kv_internal` 帧备 `char tmp_a/tmp_b[512]` 传给 KV message
  脱敏链（多规则 ping-pong，任一超长规则回退堆）；`redact_apply_one`
  改为"两阶段写调用方缓冲"语义：
  能装下 → 写 buf 返回指针；装不下 → 内部 `malloc`
  （保留 fail-open：失败返回 NULL，调用方用原文）。
- 零规则 fast path 不变（无 `strstr` 开销）。

## §3 错误处理、测试与回滚

错误处理：

- inline 路径无失败点；堆路径 `malloc` 失败沿用现有语义：
  socket 发布空 slot + `dropped++` 返回 -1；redact 失败 fail-open 用原文。
   无新增失败模式。

测试：

1. 单测：短行往返（`line == inline` 指针、`heap = false`）、长行往返
   （`heap = true`、内容一致）、`malloc` 失败注入（空 slot + dropped）、
   redact 短/长/未命中三态。
2. 压力：8 producers × 短行，断言 `real == ok` 且 consumer 无泄漏（ASan）。
3. 回归：`make check` 全绿 + `make benchmark` 对 baseline（吞吐不回退才合入）。

回滚：任一门禁红 → revert 本分支提交（改动只涉 `core/socket_async.c`、
`core/log.c`、内部头 `include/socket_async.h` + 两测试文件，无公有依赖）。

## 非目标

- slab 内存池中心化（方案 B，维护成本高，暂不做）。
- 激进 ABI 重构：`inline 256→1K`、queue 预分配大缓冲（方案 C，破 ABI，暂不做）。
- 二进制体积优化：本次只降分配次数，不做裁剪/section GC。
