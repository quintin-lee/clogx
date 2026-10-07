# Sync Hot-Path 吞吐优化 (A+B Newline Pack) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 同步/格式化路径吞吐提升:记录组装减负(module 去锁/seqlock、record 逐字段初始化、pid 缓存、redact 快路)+ 换行并入一次 write,ABI 零变化。

**Architecture:** 只动 3 个文件(`core/log_internal.h` 加 2 个内部字段、`core/log.c` 组装路径、`core/dispatcher.c` 写入合并);formatter/sinks/queue/async/public headers 不碰。性能由 benchmark 证明,语义由回归 + 2 个新确定性测试锁定(perf 重构无红绿 TDD,测试先行锁定语义)。

**Tech Stack:** C99, `clog_atomic_*` (`include/clog_port.h`),seqlock 双翻转 `module_gen`,现有 `pthread_atfork` 钩子。

**Baselines & spec:** [spec](../specs/2026-10-08-sync-hotpath-design.md) §0–§3;基线 `/tmp/bench_sync_before.txt`(sync 436,726 / throughput 432,976 msgs/sec)。

---

## File structure

- Modify `core/log_internal.h:66-68`: `logger_t` Module 区加 `volatile size_t module_gen` + `uint32_t cached_pid`(内部头,不安装,ABI 安全)。
- Modify `core/log.c`: set_module 双 bump (line ~205)、两处写路径 module seqlock 拷贝 (line ~382, ~491)、两处 record 逐字段初始化 + `cached_pid` (line ~387, ~496)、两处 init 设 pid、atfork child 刷新 pid、redact 快路 (line ~873)。
- Modify `core/dispatcher.c:217-226`: 换行原地追加,一次 write。
- Create `tests/test_sync_hotpath.c`: fork-pid 测试 + module 并发 set 内容测试。
- Modify `Makefile` (`TESTS`) + `CMakeLists.txt` (`CLOG_TEST_SOURCES`):注册新测试(纯 public API,不进 internal 列表)。
- Modify `CHANGELOG.md`: `Unreleased` 加条目。

---

### Task 1: 基线确认 + logger_t 加字段

**Files:**
- Modify: `core/log_internal.h:66-68`

- [ ] **Step 1: 确认基线文件存在**

```bash
grep -E "msgs/sec|throughput:" /tmp/bench_sync_before.txt
```

Expected: 三行数字 (sync 436726 / async 416752 / throughput 432976)。若文件丢失,重跑 `make clean && make benchmark > /tmp/bench_sync_before.txt`。

- [ ] **Step 2: Module 区加字段**

```c
    /* ── Module name ── */
    char         module[64];   /**< Module/tag name for %module token. */
    volatile size_t module_gen; /**< Seqlock generation: odd = writer inside. */
    uint32_t     cached_pid;   /**< getpid() cached at init, refreshed atfork. */
    clog_mutex_t module_mutex; /**< Mutex for module name access (slow path). */
```

`logger_t` 仅 `core/log_internal.h` 定义、不安装,加字段 ABI 安全。

---

### Task 2: set_module 双翻转 + 初始化 pid

**Files:**
- Modify: `core/log.c:205-214` (`logger_set_module_internal`),logger init 函数,atfork child

- [ ] **Step 1: set 路径加双 bump**

```c
static void logger_set_module_internal(logger_t *logger, const char *module)
{
    clog_mutex_lock(&logger->module_mutex);
    logger->module_gen++; /* odd: writer inside */
    if (!module || !*module) {
        snprintf(logger->module, sizeof(logger->module), "%s", "main");
    } else {
        snprintf(logger->module, sizeof(logger->module), "%s", module);
    }
    logger->module_gen++; /* even: done */
    clog_mutex_unlock(&logger->module_mutex);
}
```

`volatile size_t` 普通 `++` 在 mutex 内安全;读者用 `clog_atomic_load_sz` (ACQUIRE)。

- [ ] **Step 2: 初始化 cached_pid**

找到 logger 初始化函数(命令定位):

```bash
grep -n "initialized = true" core/log.c
```

在每个初始化点(默认 logger + instance 创建)加:

```c
logger->cached_pid = clog_getpid();
logger->module_gen = 0;
```

- [ ] **Step 3: atfork child 刷新 pid**

`core/log.c` atfork child handler (`log_atfork_child`,~line 1179,已有 `log_dispatcher_atfork_child_for` 那几行处)加:

```c
g_default_logger.cached_pid = clog_getpid();
```

注:仅 default logger 有 atfork 覆盖;instance logger fork 语义与今天一致(今天也是 live getpid —— 不,改后 instance fork 会 stale!处理:instance 创建时若检测 `getpid() != cached_pid` 则刷新? simpler:写路径用 `cached_pid` 前不做检查,但 instance 的 fork 本就要求用户重建 logger(查 README fork 约束,若无此约束则 Step 3 改为 atfork 刷新全部已知 logger —— 先 grep 确认 instance fork 约束,若无约束,本 step 改为遍历刷新并在 plan 执行时以实际代码为准)。**执行者注意**:先跑 `grep -rn -i "fork" README.md docs/ | head`,有"fork 后重建"约束则只刷 default,无则刷全部。

- [ ] **Step 4: 编译验证**

```bash
make build/libclogx.a && echo STRUCT_OK
```

---

### Task 3: 两处写路径 module seqlock + record 逐字段初始化

**Files:**
- Modify: `core/log.c:382-385` (macro 路径),`core/log.c:491-494` (kv/instance 路径,函数名以实际为准,紧随 `record.message = msg ? msg : ""` 那一处)

两处做**完全相同**的替换(重复代码是故意的,两处调用者不同,勿合 helper 掩盖调用点差异——其实就是同样三行,直接贴两遍)。

- [ ] **Step 1: macro 路径 (line ~382) 替换为**

```c
    char module_buf[64];
    size_t gen1 = clog_atomic_load_sz(&logger->module_gen);
    if ((gen1 & 1) == 0) {
        memcpy(module_buf, logger->module, sizeof(module_buf));
        size_t gen2 = clog_atomic_load_sz(&logger->module_gen);
        if (gen1 != gen2) {
            goto module_locked;
        }
    } else {
module_locked:
        clog_mutex_lock(&logger->module_mutex);
        memcpy(module_buf, logger->module, sizeof(module_buf));
        clog_mutex_unlock(&logger->module_mutex);
    }
```

`goto` 跳入分支是本文件既有风格(错误处理常用),label 放 else 内合法。`memcpy` 替代 `snprintf("%s")`,64B 定长,无截断语义变化(module 本就 64B 数组)。

- [ ] **Step 2: kv/instance 路径 (line ~491) 贴同样三行**(把 `module_buf` 声明保留,替换 lock/snprintf/unlock 三行)。

- [ ] **Step 3: 两处 `memset(&record, 0, sizeof(record))` 改逐字段初始化**

macro 路径 (line ~387):

```c
    log_record_t record;
    record.level     = level;
    record.timestamp = clog_get_timestamp_us();
    record.tid       = clog_get_thread_id();
    record.pid       = logger->cached_pid;
    record.file      = file;
    record.func      = func;
    record.line      = line;
    record.module    = module_buf;
    record.tag       = NULL;
    record.message   = message;
    record.kv_count  = 0;
```

删 `memset` 行。kv/instance 路径同理,但保留其 `record.message = msg ? msg : ""` 与随后的 `record.kv_count = count` 赋值(即删 memset,加其余 9 个字段赋值 + `record.pid = logger->cached_pid`)。

安全性:`inline_buf`/kv 数组残留数据无人读取(clone 只读 `i < kv_count`;kv 路径 count 来自调用者;free 路径 sync 栈 record 不走 free_owned)。`trace_id/span_id` 原 memset 清零,改后残留?`span_current_ids(record.trace_id, record.span_id)` 紧随其后全覆盖写入(两处皆有,macro line ~399),无需预零。

- [ ] **Step 4: 编译验证**

```bash
make build/libclogx.a && echo WRITE_PATH_OK
```

---

### Task 4: redact 零规则快路

**Files:**
- Modify: `core/log.c:786` (声明),`core/log.c:873-880` (函数),写入处 (line ~793-813)

- [ ] **Step 1: `g_redact_count` 改 volatile**

```c
static volatile size_t g_redact_count = 0;
```

- [ ] **Step 2: 快路**

```c
static void redact_message_if_needed(char *buf, size_t buf_size)
{
    if (clog_atomic_load_sz(&g_redact_count) == 0) {
        return; /* No rules: zero-cost (no mutex). */
    }
    clog_mutex_lock(&g_redact_mutex);
    if (g_redact_count > 0) {
        redact_apply_locked(buf, buf_size);
    }
    clog_mutex_unlock(&g_redact_mutex);
}
```

写者(`clog_redact_add/clear`,持 mutex)在 `++`/`=0` 处同步改用 `clog_atomic_store_sz(&g_redact_count, ...)`(三处:line ~796 取 slot 后的 `++` 改 fetch_add 或 store 旧值+1 —— 原代码是 `slot = &rules[g_redact_count]; ...; g_redact_count++`,改为 `size_t n = clog_atomic_load_sz(...); slot = &rules[n]; ...; clog_atomic_store_sz(&g_redact_count, n + 1)`;clear 的 `= 0` 改 store)。mutex 内复核保留,防 add/clear 竞争。

- [ ] **Step 3: 编译 + redact 相关测试**

```bash
make build/libclogx.a && make build/test_redact 2>/dev/null && ./build/test_redact || grep -rln "redact" tests/*.c
```

以实际文件名跑对应测试(先 `ls tests | grep -i redact`,plan 写时测试名未完全确认,执行者以实测为准,expect pass)。

---

### Task 5: dispatcher 换行合并

**Files:**
- Modify: `core/dispatcher.c:217-226`

- [ ] **Step 1: 原地追加换行**

```c
        const char *write_buf = formatted_buf;
        size_t      write_len = (size_t)len;
        char        use_buf[CLOG_MAX_FORMATTED_SIZE];
        if (colored_len > 0 && console_sink_is_color_enabled(sink)) {
            write_buf = colored_buf;
            write_len = (size_t)colored_len;
        }
```

等等——`formatted_buf`/`colored_buf` 是调用者栈数组,不能保证尾部有空位。正确做法:format 返回后先规范化到一次 write 的语义,保持**两处** buffer 都只读。重写循环体为:

```c
        const char *write_buf = formatted_buf;
        size_t      write_len = (size_t)len;
        if (colored_len > 0 && console_sink_is_color_enabled(sink)) {
            write_buf = colored_buf;
            write_len = (size_t)colored_len;
        }
        if (write_len > 0 && write_buf[write_len - 1] != '\n') {
            /* Fast path: newline appended in a stack staging copy — one write syscall. */
            char   staged[CLOG_MAX_FORMATTED_SIZE + 1];
            size_t cap = (write_buf == colored_buf) ? sizeof(colored_buf) : sizeof(formatted_buf);
            if (write_len + 1 < cap && write_len + 1 < sizeof(staged)) {
                memcpy(staged, write_buf, write_len);
                staged[write_len]     = '\n';
                staged[write_len + 1] = '\0';
                sink->write(sink, staged, write_len + 1);
            } else {
                /* Full-size line: fall back to two writes (byte-identical output). */
                sink->write(sink, write_buf, write_len);
                sink->write(sink, "\n", 1);
            }
        } else {
            sink->write(sink, write_buf, write_len);
        }
```

16KB `staged` 栈数组:dispatcher 已有两个大栈 buf(8K+16K),再加 16K 到 40KB —— 线程默认栈 8MB,安全;但 `-Wframe-larger-than` 若开启会告警(查 Makefile 无此 flag,可行)。或者复用:把 staged 定为 `CLOG_MAX_COLORED_SIZE + 1` 保证两种 buf 都装下,cap 判断取对应 buf 大小(已写)。输出字节与今天完全一致(含回退路径)。

- [ ] **Step 2: 编译验证**

```bash
make build/libclogx.a && echo DISPATCH_OK
```

---

### Task 6: 语义锁定测试(先行,绿-绿)

**Files:**
- Create: `tests/test_sync_hotpath.c`
- Modify: `Makefile` `TESTS` + `CMakeLists.txt` `CLOG_TEST_SOURCES` (纯 public API,不进 internal)

- [ ] **Step 1: 写测试**(无框架风格,仿 `test_module_trunc.c`)

```c
/** fork 后 pid 正确 + 并发 set_module 内容正确. */
#include "log.h" (实际 public 头名以 repo 为准:is #include "clogx/log.h"? 执行者先确认 tests 现有文件的 include 行,照抄)
```

两个用例:
  a. `fork_pid`:init default logger(写文件 sink 到 `logs/pid_test.log`? 用已有测试的 sink 构造方式,照抄最近的 file-sink 测试),fork,子进程打一条带 `%pid` 的日志,父 wait,读文件断言含子进程 pid 字符串(子进程 `getpid()` 转字符串比对)。
  b. `module_race`:8 线程 × 10k 条,每 1k 条穿插一次 `log_set_module`(实际 set API 名以 `grep -n "void log_set_module\|logger_set_module" include/log.h` 为准),consumer 侧不断言 module 值(允许交错),只断言:无 crash、无截断乱码(每行以 `\n` 结尾且含 `pressure` 关键字)。内容正确性由 seqlock 保证,本测试只锁"不崩 + 行完整"。

- [ ] **Step 2: 注册并跑绿**(perf 重构,语义测试前后都应绿;若红,停下修实现,不猜)

```bash
make build/test_sync_hotpath && ./build/test_sync_hotpath
```

Expected: exit 0。

---

### Task 7: 全门禁 + benchmark 对比 + 提交

- [ ] **Step 1: 全量测试**

```bash
make test
```

Expected: EXIT 0。

- [ ] **Step 2: 完整门禁**

```bash
make check
```

Expected: 全绿(mermaid-check 已知环境失败除外,需与 master 表现一致)。

- [ ] **Step 3: benchmark 对比**

```bash
make clean && make benchmark > /tmp/bench_sync_after.txt 2>&1; grep -E "msgs/sec|throughput:" /tmp/bench_sync_after.txt
```

Expected: sync 与 throughput 高于基线 (436,726 / 432,976),零回退。**基线不自动刷新**(`benchmarks/.baseline` 变动需单独 commit,报数字给用户定)。

- [ ] **Step 4: CHANGELOG + 提交**

CHANGELOG `Unreleased` 加条目(仿 async 轮措辞),然后:

```bash
git add core/log.c core/dispatcher.c core/log_internal.h tests/test_sync_hotpath.c Makefile CMakeLists.txt CHANGELOG.md
git commit -m "feat(sync): ⚡ hot-path assembly + single-write newline (module seqlock, no memset, cached pid)"
```

---

## Self-Review

1. **Spec coverage:** S2.1(Task 2+3)✔ S2.2(Task 3)✔ S2.3(Task 2+3+6a)✔ S2.4(Task 4)✔ S2.5(Task 5)✔ §3 验证(Task 6+7)✔ 范围两文件+测试/构建✔。
2. **Placeholder scan:** 无 TBD/TODO;两处"以实际为准"均为带确切 grep 命令的定位步骤,非开放占位。
3. **Type consistency:** `module_gen(volatile size_t)`/`cached_pid(uint32_t)`/`g_redact_count(volatile size_t)` 三处同名;`clog_atomic_load_sz/store_sz` 已在 `clog_port.h` 存在(queue.c 在用);seqlock 双翻转读写配对。
