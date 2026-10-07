# 同步/格式化路径吞吐优化 (Sync Hot-Path) Design Spec

- **日期**:2026-10-08
- **范围**:同步/格式化路径,吞吐优先,**ABI 不动**
- **前置**:async fast-path 实现在 worktree 分支 `feat/async-producer-fastpath` (commit `0885ad6`,未合并);本 spec 只动同步路径,与其无冲突
- **选型**:A(记录组装减负)+ B 的换行合并打包;不动 `dispatcher_mutex` 锁粒度,不动 formatter 本体

## §0 背景与依据

formatter 本体已精益(opcode 预编译 + 全局缓存 `formatter.c:868-871`、按秒时间缓存、手写 `u32toa`),真瓶颈在记录组装与 sink 写入。实测核实:

1. module 拷贝:每条记录 `module_mutex` lock + `snprintf("%s")` (`core/log.c:382-385`);module 只在 init/set 时写(`logger_set_module_internal`, `core/log.c:205`)。
2. `memset` 整个 `log_record_t` (`core/log.c:388`):struct 已膨胀到 ~1KB(含 256B `inline_buf` + kv 数组),sync 路径用不上这两块。
3. pid:POSIX `clog_getpid()` 是裸 `getpid()` syscall (`include/clog_port.h:317`),无缓存。
4. redact:零规则也要走 `g_redact_mutex` lock/unlock (`core/log.c:873-880`)。
5. 双 write:每 sink 每条 `write(content)` + 条件 `write("\n")` (`core/dispatcher.c:223-226`)。
6. 不值得动的:`pthread_self()` 读 TCB 无 syscall(tid 不缓存);`clock_gettime` 走 vDSO(timestamp 不动)。

## §1 目标 / 基线 / 成功标准 / 范围锁死

- **目标**:同步/格式化路径吞吐优先,ABI 不动(`log_record_t` 不加字段、导出符号不变)。
- **基线**(`/tmp/bench_sync_before.txt`,master 现状,clean 后实测):sync 436,726 / async 416,752 / throughput 432,976 msgs/sec。完工 clean 后重跑 diff,只看 sync 与 throughput 两项。
- **成功标准**:sync 提升且零回退;`make check` 全绿(mermaid-check 已知在干净 master 同败,环境缺 jsdom,除外)。
- **范围锁死**:只动 `core/log.c`(记录组装)、`core/dispatcher.c`(换行合并);sinks 各实现、queue/async、public headers 一律不碰。
- **语义红线**:输出字节流与今天完全一致(含换行位置、JSON/OTLP 字段顺序);level 过滤、redact、rotation 行为不变。

## §2 设计

### S2.1 module 拷贝去锁化 (`core/log.c`)

logger 新增原子 `module_gen`(set 路径加 1)。写路径:无锁读 gen → `memcpy` 64B 到栈上 `module_buf` → 复核 gen 未变则采用;变了才回退加锁重拷。`snprintf("%s")` 换 `memcpy`,消除 format 解析。set 并发写竞争由 gen 复核 + 回退覆盖,不引入新不正确性。

### S2.2 record 逐字段初始化 (`core/log.c`)

`memset(&record, 0, sizeof(record))` 改为只初始化实际使用的 10 个字段(`level/timestamp/tid/pid/file/func/line/module/tag/message` + `kv_count = 0`),`inline_buf`/kv 数组不再清零。async clone 路径只读 `i < kv_count` 的 kv 项(已核实),sync 路径字符串均为栈/静态所有、无 free 语义,安全。

### S2.3 pid 缓存 (`core/log.c` + atfork)

pid 存入 logger,init 时取值,fork 后经现有 atfork 钩子刷新。fork 正确性由 S3 专项测试覆盖。

### S2.4 redact 零规则快路 (`core/log.c`)

先原子读 `g_redact_count`,为零直接返回,不碰 `g_redact_mutex`。规则非零时走旧路径,行为不变。

### S2.5 换行并入一次 write (`core/dispatcher.c`)

format 后若尾字符非 `\n` 且 `len + 1 < bufsize`,原地追加后一次 `write`;容量不够(整 8K 行)才回退两次 write。colored 路径同理(16K buf)。输出字节流逐字节一致。

### 明确不碰

formatter 本体、sink 各实现、`dispatcher_mutex` 全局串行(动它即 B 完全体,留待后轮)、queue/async、public headers。

## §3 验证方案

- **benchmark**:完工 clean 后重跑 `make benchmark`,与基线 diff,只看 sync 与 throughput。
- **门禁**:`make check` 全绿(mermaid-check 除外,同 §1)。
- **回归**:现有全套件;若无 module 并发单测,plan 阶段补一个确定性单测(set module 后并发写,只断言内容正确)。
- **fork 专项**:fork 子进程打一条,断言 record pid 为子进程 pid(覆盖 S2.3 atfork 刷新)。
