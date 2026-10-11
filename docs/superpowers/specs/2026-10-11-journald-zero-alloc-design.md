# journald 写路径零分配设计

- 日期： 2026-10-11
- 前置工作： 热路径零分配（socket 256B inline + redact 512B ping-pong，`perf/alloc-zero` 已合入）、`core/log.c` 分区整理（`tidy/logc-partitions` 已合入）。
- 本轮定向： 性能深挖 → 范围锁定 journald（方案 A：最小 diff，无新测试）。

## §1 目标 / 范围 / 非目标

- **目标**： `sinks/journald_sink.c` 的 `journald_write` 目前每条记录一次 `malloc(total)` + 一次 `free`（第 123/142/146/149 行）。小消息改栈缓冲，实现零分配。
- **范围**： 只动 `journald_write`（89–151 行）。阈值 512B，与已落地的 redact ping-pong / socket framed 栈缓冲统一口径。
- **非目标**： 不动 factory（冷路径）、不动 journal 协议字节格式、不新增测试文件、不改任何头文件与 ABI。
- **硬约束**： 零行为变化（失败仍 `return -1`，成功仍返回 `(int)len`）；`make test` 全绿 + `make check` 干净。

全库剩余分配扫描结论（定范围依据）： sink 的 `malloc` 全在 `*_create`（冷路径）；dispatcher 快照在 reload（冷路径）；socket/log/async 剩下的全是 by-design 的大消息 heap 回退；唯一热路径逐条分配即 journald packet。

## §2 改动细节（内存协议）

```c
char  stack_buf[512];
packet = (total <= sizeof(stack_buf)) ? stack_buf : (char *)malloc(total);
if (!packet) return -1;   /* 大消息 malloc 失败仍 -1，原语义不变 */
```

三处 `free(packet)`（`off != total` 防御分支、send 失败分支、正常返回前）全部改为：

```c
if (packet != stack_buf) free(packet);
```

安全性说明： `total` 在 117–122 行已完全算定（prio + ident + framing + msg_len），之后 `off` 只增不减；栈/堆选择只影响内存来源，不影响 `off != total` 校验语义。典型消息（prio ~11B + ident ~10B + framing ~30B + 正文）<300B 走栈；`JOURNALD_MAX_MESSAGE` 240KB 超大消息走原 malloc 路径，栈不爆。

备选方案记录： 方案 B（A + socketpair 单测，需 `#include "../sinks/journald_sink.c"` 新测试手法）曾被推荐但用户未选；方案 C（不做）已否决。

## §3 错误处理 / 验证 / 回滚

- **错误处理**： 无新失败模式。栈路径永不失败；堆回退失败走原 `return -1`；`off != total` 防御分支保留，先条件 free 再 `return -1`（顺序与原来一致）。
- **验证**： `make build/test_journald_sink && ./build/test_journald_sink`（无 journal 环境 SKIP，有则跑 round-trip）＋ 全量 `make test`（exit 0）＋ `make check`（format/tidy；mermaid-jsdom 环境失败已知且与本分支无关）＋ `nm -D` ABI 符号数对照（无头文件改动，必过，走形式）。
- **回滚**： 单 commit，`git revert` 即可；分支策略沿用上一轮（功能分支 → fast-forward 回 master）。
