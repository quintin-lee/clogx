# journald 写路径零分配 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `journald_write` 小消息走 512B 栈缓冲，消除每条记录一次 malloc/free，零行为变化。

**Architecture:** 函数内局部缓冲二选一（`total <= 512` 用栈，否则原 malloc），三处 free 点加 `packet != stack_buf` 条件。单文件、单函数、无新符号。

**Tech Stack:** C99，Makefile（`make build/test_journald_sink`、`make test`、`make check`）。

---

## 文件结构说明

只改一个文件：`sinks/journald_sink.c` 的 `journald_write`（89–151 行）。不动头文件，不动测试，不动构建系统。
测试从 repo 根运行（cwd 必须是 repo 根）。
无 TDD 红/绿循环：方案 A 明确不新增测试（`journald_write` 为 static，现有 `tests/test_journald_sink.c` 在有 journal 环境跑 round-trip）；验证靠重编 + 现有测试 + 全量门禁。

---

### Task 1: 建分支 + 基线（journald 测试现状 + ABI 快照）

**Files:**
- Modify: `sinks/journald_sink.c`（后置任务）
- Test: `make build/test_journald_sink`、`make test`

- [ ] **Step 1: 从 master 建功能分支**

```bash
git checkout -b perf/journald-zero-alloc
git status --short
```

Expected: 新分支，切自 master，工作树干净（除已知的 untracked plan 文件外）。

- [ ] **Step 2: 记录 journald 测试基线行为**

```bash
make build/test_journald_sink && ./build/test_journald_sink 2>&1 | tail -n 3
```

Expected: PASS 或 SKIP（本容器无 journal 守护进程时为 SKIP；有则跑 round-trip）。记下结果给 Task 3 对照：基线是什么，改后必须一致。

- [ ] **Step 3: 保存 ABI 基线快照**

```bash
ls build/*.so && nm -D build/libclogx.so | grep ' T ' | awk '{print $3}' | sort > /tmp/abi_base_journald.txt && wc -l /tmp/abi_base_journald.txt
```

Expected: 显示 `.so` 文件名，快照约 74 行（数字以实际输出为准，记下来给 Task 3 对照）。

---

### Task 2: 加栈缓冲 + 编译 + 现有测试

**Files:**
- Modify: `sinks/journald_sink.c:89-151`

- [ ] **Step 1: 应用以下精确改动**

改动点 1 —— 声明区加栈缓冲（原 97 行 `char *packet;` 处）：

```c
    char                 *packet;
    char                  stack_buf[512];
    size_t                off = 0;
```

改动点 2 —— 分配点二选一（原 123–126 行）：

```c
    packet = (total <= sizeof(stack_buf)) ? stack_buf : (char *)malloc(total);
    if (!packet) {
        return -1;
    }
```

改动点 3 —— 三处 `free(packet);`（`off != total` 防御分支、send 失败分支、正常返回前）逐一改为：

```c
    if (packet != stack_buf) {
        free(packet);
    }
```

注意：防御分支原顺序是先 `free(packet);` 再 `return -1;`，保持该顺序，只给 free 加条件。

- [ ] **Step 2: 编译通过**

```bash
make build/libclogx.a 2>&1 | tail -n 3
```

Expected: 无 warning、无 error（`-Wall -Wextra -Wconversion` 全开）。

- [ ] **Step 3: journald 测试行为与基线一致**

```bash
make build/test_journald_sink && ./build/test_journald_sink 2>&1 | tail -n 3
```

Expected: 与 Task 1 Step 2 记录的基线结果一致（PASS 则仍 PASS，SKIP 则仍 SKIP）。

- [ ] **Step 4: 提交改动**

```bash
git add sinks/journald_sink.c && git commit -m "perf(journald): ⚡️ zero-alloc write path (512B stack fast path)"
```

---

### Task 3: 全门禁（make test + make check + ABI 对照）

**Files:**
- Test: 全套

- [ ] **Step 1: 全量测试**

```bash
make test 2>&1 | tail -n 5
```

Expected: 全部 PASS，exit 0（与基线一致；已知 `test_async_producer_pressure` 偶发 timing flake：若单测失败而独立重跑通过，记为 flake，不阻断）。

- [ ] **Step 2: 格式 + tidy**

```bash
make check 2>&1 | tail -n 8
```

Expected: format/tidy 干净后走完全部测试（已知 `mermaid-check` 因缺 jsdom 模块失败属环境问题：本分支零 mermaid 改动，若失败需确认 master 同样失败）。

- [ ] **Step 3: ABI 符号对照**

```bash
nm -D build/libclogx.so | grep ' T ' | awk '{print $3}' | sort > /tmp/abi_after_journald.txt && diff /tmp/abi_base_journald.txt /tmp/abi_after_journald.txt && echo ABI-UNCHANGED
```

Expected: `ABI-UNCHANGED`（无任何输出差异；无头文件改动，必过，走形式）。

---

### Task 4: CHANGELOG + 提交

**Files:**
- Modify: `CHANGELOG.md`（`Unreleased` 节）

- [ ] **Step 1: 加 Unreleased 条目**

在 `CHANGELOG.md` 的 `Unreleased` 节下追加（按仓库 conventional-commit 语义，本次属性能优化、无行为变化，不触发版本 bump；exact text）：

```markdown
- perf(journald): ⚡️ zero-alloc write path — 512B stack fast path for small records, malloc fallback for oversized; zero behavior change, no ABI change.
```

- [ ] **Step 2: 提交**

```bash
git add CHANGELOG.md && git commit -m "docs: 📝 changelog for journald zero-alloc write path"
git log --oneline -3 && git status --short
```

Expected: 两个 commit（Task 2 改动 + 本 CHANGELOG），工作树干净（除已知的 untracked plan 文件）。

---

## Self-Review（已执行）

1. **Spec 覆盖**：§1（单函数/零行为/硬约束）→ Task 1 基线 + Task 3 门禁；§2（512B 二选一 + 三处条件 free + 顺序不变）→ Task 2 三个改动点逐字对应；§3（验证矩阵 + 单 commit 回滚）→ Task 3 + 单改动 commit（Task 2 Step 4）。
2. **Placeholder 扫描**：代码 diff、命令、commit message、CHANGELOG 文本全部完整，无 TBD/TODO/"酌情"。flake 与 mermaid 两处已知问题写明判定标准，非开放式描述。
3. **一致性**：行号（89–151 函数体、97 声明、123–126 分配、三处 free）与 2026-10-11 实测源码一致；分支名/快照文件名（`/tmp/abi_base_journald.txt`）全计划统一；阈值 512B 与 spec 一致。
