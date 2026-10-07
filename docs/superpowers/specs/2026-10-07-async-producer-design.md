# Async 生产者减负 (Producer Fast-Path) 设计

> 方向: 性能优化 · async 高吞吐 · 方案A 生产者减负 · 状态:设计已批准,待写 plan
> 日期: 2026-10-07 · 现状基线: ABI 74 符号,VERSION 0.4.0,benchmark 基线见 `benchmarks/.baseline`

**Goal:** 在语义零变化(满队列 drop + fallback 回调 + stats 口径不变)前提下,砍掉 async
生产者热路径的三笔税:CAS 重试风暴、per-record `sem_post` syscall、短消息 malloc/free。
成功标准:吞吐优先,允许动 ABI(`log_record_t` 加 inline 区)。

**Architecture:** 不动 dispatcher/sinks/formatter;只改生产者侧三处
(`core/queue.c` 声明+唤醒,`core/async.c` 打包/free,`log_record_t` 布局),
consumer 侧 `get_batch_try` 与 `wait_for_items` loop 结构复用,加双重检查防唤醒丢失。

**Tech Stack:** C99, `clog_atomic_*`(见 `clog_port.h`),POSIX sem(`clog_sem_t`),
`__attribute__((aligned(64)))` cacheline 隔离,阈值宏走 `include/log_limits.h` 可覆盖风格。

---

## §1 队列声明:CAS-loop → fetch_add + cacheline 隔离

现状(`core/queue.c:131-153`):`mpsc_queue_try_put` 用 CAS-loop 抢 `head`,N 生产者并发时
同时只有一个成功,其余重试,重试量随生产者数线性增长。

1. `head` 声明改为 `atomic_fetch_add(&q->head, 1)`,一次原子操作拿到 slot,wait-free,无重试。
2. 满队列判断分两段:fetch_add 前乐观预检 `head - tail >= capacity`(命中直接返回 `-1`,
   与今天同语义);fetch_add 后复检,仍超额者**发布 tombstone**(合法 `seq` + record 置
   `is_tombstone = true` 专用位,随 §3 同一批 record 布局改动引入;consumer 见之跳过 dispatch、
   计 `dropped_queue_full`、无 owned 内存可 free),绝不碰 `head`(`head` 单调递增,
   回拨会与并发 claim 冲突)。超额只发生在满队列边界,每次耗一 slot,consumer 跳过后 tail
   照常推进收敛。不可静默弃槽:consumer 在首个未发布 slot 处 `break` 等待
   (`core/queue.c:241-243`),弃槽会导致忙等空转。
   上层 drop + `async_fallback_cb` 语义与今天一致。
3. `mpsc_queue_t`(`include/queue.h:103`)内 `head`/`tail`/`count`/`closed` 按 64B 对齐隔离,
   C99 用编译器属性(参考 `clog_port.h` 已有封装风格);`count` 字段删除(ABI 可动):
   生产者不再 `fetch_add`、consumer 不再 `fetch_sub`(单边停更会导致 `size_t` 下溢回绕),
   depth/stats 统一用 `head - tail` 快照口径(drain 路径已在用此口径,`core/queue.c:267`)。
4. 内存序:claim 用 acq-rel;slot payload 发布仍走现有 `seq` release-store,
   consumer acquire 不变,happens-before 链条不断。

不碰:slot 结构、consumer 侧、`close`/`flush` 语义。

## §2 唤醒合并:per-record sem_post → parked 标志

现状(`core/queue.c:149`):每入队一条就 `sem_post(&items_sem)`,consumer 每批只
`sem_wait` 一次(`wait_for_items`,`core/queue.c:185`)。高吞吐下每条 log 一次 futex syscall,
且计数长期堆积。

1. 新增 `consumer_parked` 原子标志:consumer 在 `wait_for_items` 真正阻塞前置 1,
   拿到任务后清 0。
2. 生产者入队后仅当 `consumer_parked == 1` 时 `sem_post`,且用 CAS 1→0 认领 post,
   避免 N 生产者同时 post 惊群;consumer 醒着批量消费时生产者零 syscall。
3. 防丢失:post 与 parked 置位之间复查一次 `head-tail > 0`(经典 check-then-park 双重检查,
   复用现有 loop 结构);最坏多一次 spurious wakeup,现有 loop 已容忍,consumer 不可能睡死。

## §3 小消息 inline:短 log 零 malloc

现状(`core/async.c:52-74`):async 为防栈指针悬空,每条 record 深拷贝堆内存,
consumer 再逐条 `free`;而绝大多数 message < 256B, malloc/free 是纯开销。

1. `log_record_t` 内嵌 `char inline_buf[CLOG_MAX_INLINE_SIZE]`(ABI 可动,允许改);
   打包总拷贝量 ≤ 阈值时,claim slot 成功后把字符串**直接拷入 `slot->rec.inline_buf`**,
   指针就地指向 slot 区内偏移再发布 `seq`,超限走现有 heap block 路径。
   (不可指向生产者栈上 record 的 inline 区:进 slot 的是 struct 值拷贝,栈指针会悬空。)
2. 出队 `batch[i] = slot->rec` 值拷贝后若为 inline(指针落在 slot inline 区间内),
   做一次 rebase:指针改为指向 `batch[i]` 自身 `inline_buf` 的对应偏移(纯指针加减,
   struct 拷贝已带走全部字节,无二次 `memcpy`);此后 batch 自包含,slot 可立即复用。
   `log_record_free_owned` 加判:指针落在**自身** `inline_buf` 区间 → 跳过 `free`
   (纯地址比较,无分支预测压力)。
3. 阈值默认 256,宏 `CLOG_MAX_INLINE_SIZE` 进 `include/log_limits.h`, `#ifndef` 可覆盖,
   支持 `-D` 改写,对齐 `CLOG_MAX_MESSAGE_SIZE` 等现有风格;最终值由 benchmark 校准。
4. fork 安全不受影响:子进程重建 worker,不牵涉堆所有权跨进程;fallback/截断语义不变。

## §4 错误处理与验证

- 满队列:§1 tombstone 发布 + 生产者返回 `-1`,drop/callback/stats 口径零变化。
- 唤醒丢失:§2 双重检查兜底;inline 误判不可能(地址区间比较;栈上 `batch[]` 为 consumer 私有,
  record 进 slot 后地址稳定)。
- 验证流程:先 `make benchmark` 留现状底(`benchmark_async_vs_sync` +
  `benchmark_throughput`,多生产者配置),优化后对比,以 committed `benchmarks/.baseline`
  为裁判,达标才刷新基线(默认 30% 回归阈值)。
- `make check` 全门禁:format → clang-tidy(含 `clogx-unused-includes`) → clean 重建 → 全量 test;
  另新增多生产者压力测试(8 线程 × 100k 条,drop 计数自洽、顺序无关性断言)。
- 范围锁死:只动 `core/queue.c`、`core/async.c`、record 布局、`log_limits.h` 加宏;
  dispatcher/sinks/formatter 不碰。

## §5 非目标

- 不做分片队列/多 consumer(方案C,另立 spec)。
- 不做 slab/arena 通用分配器(方案B,另立 spec)。
- 不改 `benchmarks/.baseline` 阈值机制本身;不碰 ABI 版本号(发版时按 CONTRIBUTING 统一走)。
