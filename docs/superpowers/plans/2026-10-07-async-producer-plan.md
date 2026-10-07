# Async 生产者减负 (Producer Fast-Path) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 [spec](../specs/2026-10-07-async-producer-design.md) §1–§4 实现生产者 fast-path 三件套(fetch_add 声明 + parked-flag 唤醒合并 + record 内嵌 256B),语义零变化。

**Architecture:** 只动 5 个文件(`include/clog_port.h` 加 2 个原子 helper、`include/queue.h` 结构体布局、`core/queue.c` 入队/等待、`include/log_record.h` + `include/log_limits.h` record 布局、`core/async.c` 打包/free/worker),dispatcher/sinks/formatter 不碰;consumer 侧复用现有 loop 加双重检查。

**Tech Stack:** C99, `clog_atomic_*` (`include/clog_port.h`),`clog_sem_t`,cacheline 对齐属性,阈值宏 `CLOG_MAX_INLINE_SIZE`。

**Spec deviation (1 处,已核实):** spec §1 写 "consumer 见 tombstone 计 `dropped_queue_full`"——但写侧 `core/log.c:430-431` 已对每次 `log_async_write_for != 0`(含 tombstone 路径的 `-1`)计数一次,worker 再计会 double-count。因此 worker 只跳过 dispatch、不计数,"stats 口径不变"依然成立。

---

## File structure

- Modify `include/clog_port.h`: 新增 `clog_atomic_fetch_add_sz_ar` (acq-rel) 与 `clog_atomic_cas_int`,紧跟现有同类 helper,同款三分支风格(GCC/Clang `_WIN32` fallback)。
- Modify `include/queue.h`: `mpsc_queue_t` 删 `count`、加 `consumer_parked`、head/tail/closed/parked 各占 64B 对齐行;同步 `@brief` 文档(CAS-loop 描述改为 fetch_add)。
- Modify `core/queue.c`: `try_put` 改 fetch_add + tombstone + 生产者侧 rebase + 条件 post;`wait_for_items` 改 parked + 双重检查;`get_batch_try` 加 consumer 侧 rebase、删 `count` 操作;`create` 初始化新字段。
- Modify `include/log_limits.h`: 加 `CLOG_MAX_INLINE_SIZE` (默认 256) + 表格行。
- Modify `include/log_record.h`: 加 `is_tombstone` + `inline_buf[CLOG_MAX_INLINE_SIZE]` + 更新 Size 文档。
- Modify `core/async.c`: `log_record_clone` 加 inline 路径;`log_record_free_owned` 加 inline 跳过;worker 跳过 tombstone;depth 改 head-tail;atfork 更新重置字段。
- Create `tests/test_async_producer_pressure.c`: 8 生产者 × 100k 压测(TDD 先行)。
- Modify `Makefile` (`TESTS`) + `CMakeLists.txt` (`CLOG_TEST_SOURCES` + `CLOG_INTERNAL_TESTS`): 注册新测试(用内部 `mpsc_queue_*` 符号,故进 internal 列表,与 `test_queue_try_put` 同例)。

---

### Task 1: 记录优化前 benchmark 基线

- [ ] **Step 1: 运行现状 benchmark 并存档**

```bash
make benchmark > /tmp/bench_before.txt 2>&1; tail -20 /tmp/bench_before.txt
```

Expected: `benchmark_async_vs_sync` 与 `benchmark_throughput` 正常输出 throughput 数字,存档供 Task 13 对比。

---

### Task 2: `clog_port.h` 加 2 个原子 helper

**Files:**
- Modify: `include/clog_port.h` (fetch_add 附近 ~line 534,store_int 附近 ~line 594)

- [ ] **Step 1: 在 `clog_atomic_fetch_add_sz` 之后加 acq-rel 版本**

```c
static inline size_t clog_atomic_fetch_add_sz_ar(volatile size_t *ptr, size_t n)
{
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_fetch_add(ptr, n, __ATOMIC_ACQ_REL);
#elif defined(_WIN32) || defined(_WIN64)
    return (size_t)InterlockedExchangeAdd64((volatile LONG64 *)ptr, (LONG64)n);
#else
    size_t old = *ptr;
    *ptr += n;
    return old;
#endif
}
```

Rationale: 现有 `fetch_add_sz` 是 ACQUIRE-only(line 526),spec §1 要求 claim 用 acq-rel;不改旧函数(其他调用者语义不变),新增 `_ar` 后缀版本。

- [ ] **Step 2: 在 `clog_atomic_store_int` 之后加 int CAS**

```c
static inline int clog_atomic_cas_int(volatile int *ptr, int *expected, int desired)
{
#if defined(__GNUC__) || defined(__clang__)
    return __atomic_compare_exchange_n(
        ptr, expected, desired, 1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
#elif defined(_WIN32) || defined(_WIN64)
    int old = InterlockedCompareExchange((LONG *)ptr, desired, *expected);
    if (old == *expected) {
        return 1;
    }
    *expected = old;
    return 0;
#else
    if (*ptr == *expected) {
        *ptr = desired;
        return 1;
    }
    *expected = *ptr;
    return 0;
#endif
}
```

用途:Task 5 的 parked-flag 1→0 认领。Windows 分支照抄 `clog_atomic_load_int` 的 `(LONG *)ptr` cast 风格。

- [ ] **Step 3: 编译验证**

```bash
make build/libclogx.a && echo PORT_OK
```

Expected: `PORT_OK`,零警告(`-Wconversion` 下 `Interlocked*` 返回值都有显式 cast,已处理)。

---

### Task 3: `queue.h` 结构体布局 + 文档

**Files:**
- Modify: `include/queue.h:103-115` (struct),`queue.h:21-23,49,92-101` (doc)

- [ ] **Step 1: 加 cacheline 对齐宏(结构体前)**

```c
#if defined(_MSC_VER)
#define CLOG_CACHELINE_ALIGNED __declspec(align(64))
#else
#define CLOG_CACHELINE_ALIGNED __attribute__((aligned(64)))
#endif
```

`queue.h` 是内部头文件(不安装),MSVC 走 `__declspec` 分支即可。

- [ ] **Step 2: 重排结构体**

```c
typedef struct mpsc_queue_t {
    mpsc_slot_t    *buffer;      /**< Backing storage (capacity × sizeof(mpsc_slot_t)). */
    size_t          capacity;    /**< Maximum records (always rounded up to power-of-2). */
    size_t          mask;        /**< capacity - 1 (for fast modulo: pos & mask). */
    CLOG_CACHELINE_ALIGNED volatile size_t head; /**< Next write index (producer claim via fetch_add). */
    CLOG_CACHELINE_ALIGNED volatile size_t tail; /**< Next read index (consumer-side only). */
    CLOG_CACHELINE_ALIGNED volatile int closed;  /**< Non-zero once @ref mpsc_queue_close has been called. */
    CLOG_CACHELINE_ALIGNED volatile int consumer_parked; /**< 1 while consumer sleeps in wait_for_items. */
    clog_sem_t      items_sem;   /**< Semaphore: posted only on parked 1→0 claim (coalesced). */
    clog_sem_t      slots_sem;   /**< Semaphore: count of free slots (for blocking put). */
    clog_mutex_t    drain_mutex; /**< Mutex for wait_empty condvar (shutdown path only). */
    clog_cond_t     drain_cond;  /**< Signaled when head == tail after drain (for wait_empty). */
} mpsc_queue_t;
```

删除 `count` 字段;depth/stats 统一用 `head - tail` 快照(spec §1.3,单边停更会下溢回绕,故必须删不能留)。

- [ ] **Step 3: 同步文档注释**

  - line 21-23 "claims a slot via compare-exchange loop" → "claims a slot via `atomic_fetch_add` (wait-free, no retry)"。
  - line 49 thread-safety 条目 "lock-free via atomic CAS on `head`" → "lock-free via atomic fetch_add on `head`"。
  - line 92-101 struct `@brief` "claim slots atomically via compare-exchange" → "via fetch_add";"A semaphore (`items_sem`) wakes the consumer" → "(`items_sem`) wakes the consumer only when it is parked (coalesced)"。

- [ ] **Step 4: 编译验证**

```bash
make build/libclogx.a && echo QUEUE_H_OK
```

Expected: 通过(此时 `count` 引用还在 `.c` 里?不在——`count` 的 `.c` 引用 Task 4/5/6 才删,**本 Task 做完编译会失败**。顺序改为:本 Task 只做完不单独编译,合并到 Task 6 后统一验证)。调整:本 Task 无编译步骤,验证推迟到 Task 6 Step 4。

---

### Task 4: `queue.c` create 初始化 + rebase helper

**Files:**
- Modify: `core/queue.c:87-90` (create),文件顶部加 2 个 static helper

- [ ] **Step 1: create 初始化新字段、删 count**

```c
    q->head            = 0;
    q->tail            = 0;
    q->closed          = 0;
    q->consumer_parked = 0;
```

- [ ] **Step 2: 文件顶部(`next_pow2` 之后)加 inline 判定 + rebase helper**

```c
/* A record is inline iff its message points into its own inline_buf.
 * log_record_clone packs message+module+tag+KV strings all-or-nothing:
 * total <= CLOG_MAX_INLINE_SIZE goes fully inline, otherwise fully heap.
 * Empty records (message == NULL) and tombstones are never inline. */
static int log_record_is_inline(const log_record_t *r)
{
    return r->message != NULL && r->message >= r->inline_buf &&
           r->message < r->inline_buf + CLOG_MAX_INLINE_SIZE;
}

/* Repoint dst's string pointers into dst's own inline_buf at the same
 * offsets src uses. Pure pointer arithmetic, no memcpy (struct copy
 * already carried the bytes). Covers message/module/tag + KV keys/STR vals. */
static void log_record_rebase_inline(log_record_t *dst, const log_record_t *src)
{
    ptrdiff_t off;
    if (src->message) {
        off          = src->message - src->inline_buf;
        dst->message = dst->inline_buf + off;
    }
    if (src->module) {
        off         = src->module - src->inline_buf;
        dst->module = dst->inline_buf + off;
    }
    if (src->tag) {
        off      = src->tag - src->inline_buf;
        dst->tag = dst->inline_buf + off;
    }
    for (size_t i = 0; i < src->kv_count; i++) {
        if (src->kv[i].key) {
            off             = src->kv[i].key - src->inline_buf;
            dst->kv[i].key  = dst->inline_buf + off;
        }
        if (src->kv[i].type == CLOG_KV_TYPE_STR && src->kv[i].val.str) {
            off                 = src->kv[i].val.str - src->inline_buf;
            dst->kv[i].val.str = dst->inline_buf + off;
        }
    }
}
```

`queue.c` 已含 `<string.h>`;`ptrdiff_t` 来自 `queue.h` → `<stddef.h>`,无需新 include(满足 `clogx-unused-includes` 检查,不加多余头)。

---

### Task 5: `try_put` fetch_add + tombstone + 条件 post

**Files:**
- Modify: `core/queue.c:115-154` (整个 `mpsc_queue_try_put` 重写),加 `queue_signal_consumer` static

- [ ] **Step 1: 加条件唤醒 helper(放 `try_put` 之前)**

```c
/* Wake the consumer only if it is parked (CAS 1→0 claims the post).
 * When the consumer is awake and batch-draining, producers pay zero syscall. */
static void queue_signal_consumer(mpsc_queue_t *q)
{
    int expected = 1;
    if (clog_atomic_cas_int(&q->consumer_parked, &expected, 0)) {
        clog_sem_post(&q->items_sem);
    }
}
```

- [ ] **Step 2: 重写 `mpsc_queue_try_put`**

```c
int mpsc_queue_try_put(mpsc_queue_t *restrict q, log_record_t *restrict record)
{
    if (!q || !record) {
        return -1;
    }

    /* Fast check: if the queue has been closed, give up immediately. */
    if (clog_atomic_load_int(&q->closed)) {
        return -1;
    }

    /* Optimistic pre-check (same drop semantics as before). */
    size_t tail_snap = clog_atomic_load_sz(&q->tail);
    size_t head_snap = clog_atomic_load_sz(&q->head);
    if (head_snap - tail_snap >= q->capacity) {
        return -1; /* Queue is full. */
    }

    /*
     * Wait-free claim: one atomic op, no CAS retry storm under N producers.
     * head is monotonic — never rolled back (would race concurrent claims).
     */
    size_t         pos  = clog_atomic_fetch_add_sz_ar(&q->head, 1);
    mpsc_slot_t   *slot = &q->buffer[pos & q->mask];

    /* Post-claim re-check: losers of the full-queue race publish a tombstone
     * (valid seq + is_tombstone, no owned memory). Silent abandon is forbidden:
     * the consumer breaks at the first unpublished slot (get_batch_try) and
     * would spin forever on a hole. */
    tail_snap = clog_atomic_load_sz(&q->tail);
    if (pos - tail_snap >= q->capacity) {
        log_record_t tomb;
        memset(&tomb, 0, sizeof(tomb));
        tomb.is_tombstone = true;
        slot->rec         = tomb;
        clog_atomic_store_u64(&slot->seq, (uint64_t)pos + 1);
        queue_signal_consumer(q);
        return -1;
    }

    slot->rec = *record;
    if (log_record_is_inline(record)) {
        /* Pointers still reference the producer's stack copy — rebase into the slot. */
        log_record_rebase_inline(&slot->rec, record);
    }
    clog_atomic_store_u64(&slot->seq, (uint64_t)pos + 1);
    queue_signal_consumer(q);
    return 0;
}
```

`(uint64_t)pos + 1` 照抄旧 line 147 的 cast 风格(`-Wconversion` 安全)。

- [ ] **Step 3: 更新文件头 Design/Memory Ordering 文档(§1 配套)**

  - line 8-10 "uses an atomic compare-exchange loop on `head`" → "uses a single `atomic_fetch_add` on `head` (wait-free claim, no retry)".
  - line 31-34 producers 条目: CAS-loop 描述 → fetch_add claim + post-claim full check publishes tombstone;`sem_post(items_sem)` → conditional post on parked 1→0。
  - line 36 consumer 条目 `sem_wait(items_sem)` 前加 parked 置位 + double-check 说明。
  - line 18-20 semaphores 条目 "`items_sem` signals the consumer when new items arrive" → "posted only when the consumer is parked (coalesced)"。

---

### Task 6: `wait_for_items` parked 双重检查 + `get_batch_try` rebase/删 count

**Files:**
- Modify: `core/queue.c:185-212` (`wait_for_items`),`core/queue.c:214-275` (`get_batch_try`)

- [ ] **Step 1: 重写 `wait_for_items`**

```c
int mpsc_queue_wait_for_items(mpsc_queue_t *q)
{
    if (!q) {
        return -1;
    }

    for (;;) {
        /* Fast path: items already visible — never touch parked/sem. */
        size_t head = clog_atomic_load_sz(&q->head);
        size_t tail = clog_atomic_load_sz(&q->tail);
        if (head - tail > 0) {
            return 0;
        }
        if (clog_atomic_load_int(&q->closed)) {
            return -1;
        }
        /* Park, then re-check (check-then-park): a producer that posted
         * between our first check and park would otherwise be lost. */
        clog_atomic_store_int(&q->consumer_parked, 1);
        head = clog_atomic_load_sz(&q->head);
        tail = clog_atomic_load_sz(&q->tail);
        if (head - tail > 0) {
            clog_atomic_store_int(&q->consumer_parked, 0);
            return 0;
        }
        if (clog_atomic_load_int(&q->closed)) {
            clog_atomic_store_int(&q->consumer_parked, 0);
            return -1;
        }
        clog_sem_wait(&q->items_sem);
        clog_atomic_store_int(&q->consumer_parked, 0);
        /* Loop: spurious wake-ups (incl. close's empty post) re-block here. */
    }
}
```

No-lost-wakeup 论证(写进注释已覆盖):producer 的 CAS(1→0)若发生在 park 置位前,则 double-check 必见 head 推进;若发生在置位后,则 CAS 成功并 post。`close()` 的无条件 post 行为不变。

- [ ] **Step 2: `get_batch_try` 加 consumer 侧 rebase、删 count**

```c
        records[n] = slot->rec;
        if (log_record_is_inline(&slot->rec)) {
            /* Pointers reference the ring slot — rebase into our own batch copy. */
            log_record_rebase_inline(&records[n], &slot->rec);
        }
        n++;
```

并删 line 253 `clog_atomic_fetch_sub_sz(&q->count, n);`。line 260-266 drain 注释 "`count` reaches 0" → "`head == tail`"。

- [ ] **Step 3: 同步 `queue.h` line 33-35 semaphores 文档**(Task 3 已覆盖,此处确认无遗漏)。

- [ ] **Step 4: 编译验证(Task 3+4+5+6 一起验)**

```bash
make build/libclogx.a && echo QUEUE_OK
```

Expected: `QUEUE_OK`,零警告。

---

### Task 7: `log_limits.h` 加 `CLOG_MAX_INLINE_SIZE`

**Files:**
- Modify: `include/log_limits.h` (表格 + 宏定义,`CLOG_MAX_KV` 附近)

- [ ] **Step 1: 表格加行(放 `CLOG_MAX_KV` 行之后,表格在 line 12-24)**

```
 * | @ref CLOG_MAX_INLINE_SIZE   | 256      | Per-record inline string bytes (async fast-path) |
```

- [ ] **Step 2: 宏定义(放 `CLOG_MAX_KV` 块之后)**

```c
/**
 * @def CLOG_MAX_INLINE_SIZE
 * @brief Byte budget for inline string storage inside @ref log_record_t.
 *
 * When the async packer's total string bytes fit this budget, strings are
 * copied into the record's embedded `inline_buf` instead of heap — zero
 * malloc/free on the producer hot path. Overridable via `-D` like all limits.
 * Final value to be calibrated by benchmark (spec §3.3).
 */
#ifndef CLOG_MAX_INLINE_SIZE
#define CLOG_MAX_INLINE_SIZE 256
#endif
```

- [ ] **Step 3: 编译验证**

```bash
make build/libclogx.a && echo LIMITS_OK
```

---

### Task 8: `log_record.h` 加 inline 区 + tombstone 位

**Files:**
- Modify: `include/log_record.h:138-156` (struct),line 127 Size 文档

- [ ] **Step 1: struct 加字段(`kv_count` 之后)**

```c
    clog_kv_t kv[CLOG_MAX_KV]; /**< Structured key-value attributes array. */
    size_t    kv_count;        /**< Number of valid entries in kv array (0..CLOG_MAX_KV). */
    bool      is_tombstone;    /**< Queue-full marker: skip dispatch, no owned memory. */
    char      inline_buf[CLOG_MAX_INLINE_SIZE]; /**< Embedded string storage (async inline fast-path). */
```

ABI 可动已批准(spec 成功标准)。`bool` 可用:`log_record.h` 已含 `<stdbool.h>`。

- [ ] **Step 2: 更新 Size 文档(line 127)**

旧文 "Size: 64 bytes on typical 64-bit platforms…" → 改为说明 struct 已 intentionally 增大(含 256B inline 区 + KV 数组),并用实测值填写。先跑命令取数:

```bash
cat > /tmp/sizeof_rec.c <<'EOF'
#include "log_record.h"
#include <stdio.h>
int main(void) { printf("%zu\n", sizeof(log_record_t)); return 0; }
EOF
cc -std=c99 -I include /tmp/sizeof_rec.c -o /tmp/sizeof_rec && /tmp/sizeof_rec
```

把输出数字填入文档:"Size: <N> bytes on typical 64-bit platforms (dominated by kv[16] + 256B inline_buf)…"。同步改 line 17-18 "fits on the stack with minimal overhead (64 bytes…)" 同一数字。

---

### Task 9: `async.c` clone 走 inline 路径

**Files:**
- Modify: `core/async.c:89-165` (`log_record_clone`,只加分支不改 heap 路径)

- [ ] **Step 1: `total_bytes` 算出后加 inline 分支(插在 `if (total_bytes == 0)` 块之后、`malloc` 之前)**

```c
    if (total_bytes > 0 && total_bytes <= CLOG_MAX_INLINE_SIZE) {
        /* Inline fast-path: pack everything into dst's embedded buffer.
         * No malloc; pointers aim at dst->inline_buf and are rebased into
         * the ring slot by try_put (never into our stack copy). */
        char *p = dst->inline_buf;
        if (src->message) {
            memcpy(p, src->message, msg_len + 1);
            dst->message = p;
            p += msg_len + 1;
        } else {
            dst->message = NULL;
        }
        if (src->module) {
            memcpy(p, src->module, mod_len + 1);
            dst->module = p;
            p += mod_len + 1;
        } else {
            dst->module = NULL;
        }
        if (src->tag) {
            memcpy(p, src->tag, tag_len + 1);
            dst->tag = p;
            p += tag_len + 1;
        } else {
            dst->tag = NULL;
        }

        for (size_t i = 0; i < src->kv_count; i++) {
            if (src->kv[i].key) {
                size_t klen = strlen(src->kv[i].key);
                memcpy(p, src->kv[i].key, klen + 1);
                dst->kv[i].key = p;
                p += klen + 1;
            }
            if (src->kv[i].type == CLOG_KV_TYPE_STR && src->kv[i].val.str) {
                size_t vlen = strlen(src->kv[i].val.str);
                memcpy(p, src->kv[i].val.str, vlen + 1);
                dst->kv[i].val.str = p;
                p += vlen + 1;
            }
        }
        return 0;
    }
```

拷贝顺序与 heap 路径完全一致(message/module/tag/KV),总长度已预检 `<= 256`,无溢出。`p += …` 中 `size_t` 与指针运算无 narrowing(`-Wconversion` 安全);`msg_len` 等已有 `size_t` 类型。

- [ ] **Step 2: 编译验证**

```bash
make build/libclogx.a && echo CLONE_OK
```

---

### Task 10: `async.c` free 跳过 + worker 跳 tombstone

**Files:**
- Modify: `core/async.c:52-74` (`log_record_free_owned`),`core/async.c:197-201` (worker loop)

- [ ] **Step 1: `free_owned` 顶部加 inline 守卫**

```c
static void log_record_free_owned(log_record_t *record)
{
    if (!record) {
        return;
    }
    if (record->message != NULL && record->message >= record->inline_buf &&
        record->message < record->inline_buf + CLOG_MAX_INLINE_SIZE) {
        /* Inline record: strings live in our own inline_buf, nothing to free. */
        record->message  = NULL;
        record->file     = NULL;
        record->func     = NULL;
        record->module   = NULL;
        record->tag      = NULL;
        record->kv_count = 0;
        return;
    }
    const char *block = ... (以下 heap 启发式不变)
```

Tombstone 无需特判:`message == NULL` 落入旧路径,`block == NULL`,零行为。

- [ ] **Step 2: worker 跳过 tombstone(不计数——写侧 `log.c:431` 已计,见本 plan 头部 deviation)**

```c
        int count = mpsc_queue_get_batch_try(logger->queue, batch, ASYNC_BATCH_SIZE);
        for (int i = 0; i < count; i++) {
            if (batch[i].is_tombstone) {
                continue; /* Full-queue marker: already counted write-side; skip dispatch. */
            }
            log_dispatcher_dispatch_for(logger, &batch[i]);
            log_record_free_owned(&batch[i]);
        }
```

- [ ] **Step 3: 编译验证**

```bash
make build/libclogx.a && echo FREE_OK
```

---

### Task 11: `async.c` depth 快照 + atfork 重置

**Files:**
- Modify: `core/async.c:296-302` (depth),`core/async.c:316-319` (atfork)

- [ ] **Step 1: depth 改 head-tail 快照**

```c
size_t log_async_get_queue_depth_for(logger_t *logger)
{
    if (!logger || !logger->queue) {
        return 0;
    }
    mpsc_queue_t *q = logger->queue;
    return clog_atomic_load_sz(&q->head) - clog_atomic_load_sz(&q->tail);
}
```

与 drain 路径 `queue.c:267` 同口径(近似值语义不变,调用者只做展示/判断)。

- [ ] **Step 2: atfork 更新重置字段**

```c
    q->head            = 0;
    q->tail            = 0;
    q->closed          = 0;
    q->consumer_parked = 0;
```

删 `q->count = 0;`。fork 后 items_sem/slots_sem 重建逻辑不变。

- [ ] **Step 3: 全量编译验证**

```bash
make build/libclogx.a && make build/example && echo ASYNC_OK
```

---

### Task 12: TDD 压测 — 先写失败测试,再实现中变绿

> 执行顺序说明:本 Task 的 Step 1–2 应在 Task 2 之前先做(测试引用 `is_tombstone`/`CLOG_MAX_INLINE_SIZE`,旧代码下编译失败,红);Task 2–11 完成后 Step 3 变绿。若按顺序读到此处才补测试亦可,但必须先确认红再确认绿。

**Files:**
- Create: `tests/test_async_producer_pressure.c`
- Modify: `Makefile` `TESTS` 列表末尾加 `test_async_producer_pressure`
- Modify: `CMakeLists.txt` `CLOG_TEST_SOURCES` 末尾 + `CLOG_INTERNAL_TESTS` 加同名(用内部 `mpsc_queue_*`,仿 `test_queue_try_put`)

- [ ] **Step 1: 写测试文件**(仿 `tests/test_queue_try_put.c` 风格:无框架,`main` 返回非零即失败,8 线程 × 100k,小容量逼出满队列竞争)

```c
/**
 * @file test_async_producer_pressure.c
 * @brief Pressure test: 8 producers x 100k records through a small queue.
 *        Verifies producer fast-path accounting: real + tombstones + fails == attempts,
 *        final depth 0, and inline short messages survive the round trip.
 */
#include "clog_port.h"
#include "log_record.h"
#include "queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUM_PRODUCERS 8
#define LOGS_PER_PRODUCER 100000
#define QUEUE_CAP 1024

typedef struct {
    mpsc_queue_t *q;
    int           id;
    long          ok;
    long          failed;
} producer_ctx_t;

static void *producer_thread(void *arg)
{
    producer_ctx_t *ctx = (producer_ctx_t *)arg;
    for (int i = 0; i < LOGS_PER_PRODUCER; i++) {
        log_record_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.message = "pressure-msg";
        rec.module  = "mod";
        rec.tid     = (uint32_t)ctx->id;
        rec.level   = LOG_LEVEL_INFO;
        if (mpsc_queue_try_put(ctx->q, &rec) == 0) {
            ctx->ok++;
        } else {
            ctx->failed++;
        }
    }
    return NULL;
}

typedef struct {
    mpsc_queue_t *q;
    long          total;
    long          real;
    long          tombstones;
    int           inline_verified;
} consumer_ctx_t;

static void *consumer_thread(void *arg)
{
    consumer_ctx_t *ctx = (consumer_ctx_t *)arg;
    long            done = 0;
    while (done < ctx->total) {
        log_record_t batch[64];
        int          n = mpsc_queue_get_batch(ctx->q, batch, 64);
        if (n < 0) {
            break;
        }
        for (int i = 0; i < n; i++) {
            if (batch[i].is_tombstone) {
                ctx->tombstones++;
            } else {
                if (strcmp(batch[i].message, "pressure-msg") != 0 ||
                    strcmp(batch[i].module, "mod") != 0) {
                    fprintf(stderr, "content mismatch\n");
                    exit(1);
                }
                /* Inline pointers must aim inside the batch copy itself. */
                if (batch[i].message < batch[i].inline_buf ||
                    batch[i].message >= batch[i].inline_buf + CLOG_MAX_INLINE_SIZE) {
                    fprintf(stderr, "message not rebased into batch inline_buf\n");
                    exit(1);
                }
                ctx->inline_verified = 1;
                ctx->real++;
            }
            done++;
        }
    }
    return NULL;
}
```

`main`:建队 `mpsc_queue_create(QUEUE_CAP)` → 先起 consumer → 8 producers → join producers → `mpsc_queue_close` → join consumer → 断言:

```c
    long attempts = (long)NUM_PRODUCERS * LOGS_PER_PRODUCER;
    long ok = 0, failed = 0;
    for (int i = 0; i < NUM_PRODUCERS; i++) { ok += ctxs[i].ok; failed += ctxs[i].failed; }
    if (ok + failed != attempts) { fprintf(stderr, "attempt accounting\n"); return 1; }
    if (cctx.real + cctx.tombstones != ok) { fprintf(stderr, "drain accounting\n"); return 1; }
    if (cctx.real == 0 || !cctx.inline_verified) { fprintf(stderr, "no real records\n"); return 1; }
    /* depth must be 0: head == tail snapshot */
    size_t depth = clog_atomic_load_sz(&q->head) - clog_atomic_load_sz(&q->tail);
    if (depth != 0) { fprintf(stderr, "depth not zero\n"); return 1; }
    printf("pressure: %ld ok (%ld real + %ld tombstones), %ld failed, depth 0\n", ...);
```

注意:本测试走 `mpsc_queue_*` 裸队列(短消息栈上构造、`slot->rec = *record` 值拷贝,指针悬空风险由 try_put 内的 slot 拷贝消除——等等:裸 try_put 不走 clone!`rec.message` 指向字符串字面量 `"pressure-msg"`(静态存储,非栈),consumer 侧 `strcmp` 安全。这是刻意设计:字面量常驻,验证的是 queue 层拷贝/rebase,不验证 clone。clone 的 inline 覆盖由 `test_async_*` 现有测试 + 本测试的 `inline_verified` 间接覆盖?不——裸路径下 `record_is_inline(record)` 为 false(字面量不在 inline_buf),rebase 不触发,`inline_verified` 的断言会失败!

修正:要验证 queue 层 rebase,必须构造 inline record。加一个单线程确定性单测 `inline_rebase_test()`:构造 `rec`,手动 `memcpy(rec.inline_buf, "abc", 4); rec.message = rec.inline_buf;`,try_put,get,断言取出 `out.message` 内容为 "abc" 且指针落在 `out.inline_buf` 内。这是 queue 层 rebase 的直接验证,确定性、无并发。

- [ ] **Step 2: 注册并确认红**

```bash
# Makefile TESTS 末尾加 test_async_producer_pressure; CMakeLists 两处列表同步加名
make build/test_async_producer_pressure
```

Expected: **编译失败** (`is_tombstone` / `inline_buf` 不存在)——红成立后再继续 Task 2–11。

- [ ] **Step 3: Task 2–11 完成后确认绿**

```bash
make build/test_async_producer_pressure && ./build/test_async_producer_pressure
```

Expected: `pressure: ... depth 0`,exit 0。

---

### Task 13: 全门禁 + benchmark 对比

- [ ] **Step 1: 全量测试**

```bash
make test
```

Expected: 全部 pass(含新压测)。tests 假定 cwd=repo root,已有 `logs/` 由 make 创建。

- [ ] **Step 2: 完整门禁**

```bash
make check
```

Expected: format → clang-tidy(含 `clogx-unused-includes`)→ clean 重建 → 全量 test 全绿。新增 helper/宏的 include 都是已用项,不加多余头。

- [ ] **Step 3: sanitizer 抽查(queue 并发必查 TSan)**

```bash
make test-tsan 2>/dev/null || (cmake -S . -B build-tsan -DCLOG_ENABLE_TSAN=ON && cmake --build build-tsan -j && ./build-tsan/tests/test_async_producer_pressure)
```

以 repo 实际 TSan 目标名为准(`make help | grep -i tsan` 先确认;Makefile 已有 `test-asan`/`test-ubsan`,TSan 若无则用 CMake 变体)。Expected: 零 data race(parked/CAS/seq 链条)。

- [ ] **Step 4: benchmark 前后对比**

```bash
make benchmark > /tmp/bench_after.txt 2>&1; diff /tmp/bench_before.txt /tmp/bench_after.txt
```

Expected: `benchmark_async_vs_sync` + `benchmark_throughput` 多生产者吞吐提升;不回退(committed `benchmarks/.baseline` 30% 裁判线)。**基线刷新條件**:提升确认后才跑 `make benchmark-baseline`(改动 `.baseline` 文件需单独 commit,本 plan 不自动刷新)。

- [ ] **Step 5: Commit**

```bash
git add core/queue.c include/queue.h include/clog_port.h include/log_record.h include/log_limits.h core/async.c tests/test_async_producer_pressure.c Makefile CMakeLists.txt
git commit -m "feat(async): ⚡ producer fast-path (fetch_add + coalesced wakeup + inline)"
```

按 repo 惯例 conventional-commit + gitmoji(用 `gitmoji-commit` skill 组织语言),CHANGELOG `Unreleased` 下加条目。

---

## Self-Review

1. **Spec coverage:** §1 fetch_add+tombstone+删count+cacheline(Task 2/3/4/5/6/11)✔;§2 parked 合并+双重检查(Task 5/6)✔;§3 inline+rebase+free+阈值宏(Task 4/7/8/9/10)✔;§4 基线/门禁/压测(Task 1/12/13)✔;范围锁死 5 文件+测试/构建注册(Task 12/13)✔,dispatcher/sinks/formatter 无任务✔。
2. **Placeholder scan:** 无 TBD/TODO;`sizeof` 数值由 Task 8 实测填入,非占位。
3. **Type consistency:** `is_tombstone(bool)`/`inline_buf[256]`/`CLOG_MAX_INLINE_SIZE` 三处同名;`record_is_inline`/`record_rebase_inline` 两处调用签名一致;`clog_atomic_cas_int` 新 helper 仅 Task 5 使用;`fetch_add_sz_ar` 仅 Task 5 使用。
