# socket_async.c 拆分 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `core/socket_async.c`（823 行）按三簇拆成 `socket_ring.c` / `socket_transport.c` / `socket_writer.c` + `socket_async_internal.h`，零行为变化。

**Architecture:** 整块搬运 + 去 static：transport 六个 helper 去 `static`（跨单元给 writer 用），其余函数动都不动。writer struct 定义进内部头。构建双同步。

**Tech Stack:** C99，Makefile + CMake 双构建（只改源文件列表，不碰构建逻辑）。

---

## 文件结构说明

- `core/socket_ring.c`（新建）：环缓冲，原文 54–339 行（含 `/* ── Ring Buffer Implementation ── */` 分隔）。
- `core/socket_transport.c`（新建）：backoff + socket 建连/发送/TLS，原文 339–488 行（含 `/* ── Exponential Backoff Helpers ── */` 分隔，含末尾 `#endif`）。
- `core/socket_writer.c`（新建）：writer 线程 + 启停，原文 490–823 行（含 `/* ── Writer Thread ── */` 与 `/* ── Public API ── */` 两个分隔）。
- `core/socket_async_internal.h`（新建）：`struct socket_writer_t` 定义（原文 40–52 行，含 `/* ── Internal writer state ── */` 分隔）+ 6 个 transport helper 前向声明。
- `core/socket_async.c`（删除）：原文 1–38 行文件头 `@file` docblock（含 Lock-free Design）跟 ring 走，放到 `socket_ring.c` 开头。
- `include/socket_async.h`：不动。`tests/test_socket_async.c`：不动（ring 结构体本就在公共头）。

行号全部 1-indexed，来自 master 实测（`^}` 映射 + 三段原文抽读交叉验证）。

---

### Task 1: 建分支 + 基线（测试全绿 + ABI 快照）

**Files:**
- Modify: `core/socket_async.c`（后置任务）
- Test: `make build/test_socket_async`、`make test`

- [ ] **Step 1: 从 master 建功能分支**

```bash
git checkout -b refactor/socket-split
git status --short
```

Expected: 新分支，切自 master，工作树干净（除已知的 untracked plan 文件外）。

- [ ] **Step 2: socket 测试基线**

```bash
make build/test_socket_async && ./build/test_socket_async 2>&1 | tail -n 3
```

Expected: 全 PASS。记下结果给 Task 4 对照。

- [ ] **Step 3: 保存 ABI 基线快照**

```bash
ls build/*.so && nm -D build/libclogx.so | grep ' T ' | awk '{print $3}' | sort > /tmp/abi_base_sock.txt && wc -l /tmp/abi_base_sock.txt
```

Expected: 显示 `.so` 文件名，快照约 74 行（数字以实际输出为准，记下来给 Task 4 对照）。

---

### Task 2: 内部头 + 双构建同步（先断后立，红）

**Files:**
- Create: `core/socket_async_internal.h`
- Modify: `Makefile:116`（`CORE_SRCS` 行）、`CMakeLists.txt:126`（`core/socket_async.c` 行）

- [ ] **Step 1: 写内部头（exact content）**

```c
#ifndef CLOGX_SOCKET_ASYNC_INTERNAL_H
#define CLOGX_SOCKET_ASYNC_INTERNAL_H

#include "socket_async.h"
#include "clog_port.h"

#ifdef CLOG_USE_TLS
#include <openssl/ssl.h>
#endif

/* ── Internal writer state ── */

struct socket_writer_t {
    socket_writer_config_t config;     /**< Copy of configuration. */
    socket_ring_buffer_t  *ring;       /**< Ring buffer (owned). */
    clog_thread_t          thread;     /**< Writer thread handle. */
    clog_socket_t          sockfd;     /**< Current socket fd. */
    int                    connected;  /**< 1 if socket is connected. */
    uint32_t               backoff_ms; /**< Current backoff delay. */
#ifdef CLOG_USE_TLS
    SSL_CTX *ssl_ctx; /**< TLS context (if enabled). */
    SSL     *ssl;     /**< TLS session (if enabled). */
#endif
};

uint32_t backoff_next(uint32_t current_ms, uint32_t max_ms);
void     backoff_sleep(uint32_t ms);
int      socket_set_nonblocking(clog_socket_t sockfd);
int      socket_connect_nonblocking(clog_socket_t sockfd,
                                    const char   *host,
                                    int           port,
                                    uint32_t      timeout_ms);
int      socket_send_nonblocking(clog_socket_t sockfd, const char *buf, size_t len);
#ifdef CLOG_USE_TLS
int tls_send_nonblocking(SSL *ssl, const char *buf, size_t len);
#endif

#endif /* CLOGX_SOCKET_ASYNC_INTERNAL_H */
```

注意：struct 定义从原文 40–52 行逐字搬（含注释与 `#ifdef`）；函数签名与原文 342/361/375/390–391/435/470 行一致，只是去掉 `static`。`socket_connect_nonblocking` 原文就是两行式签名（`static int` 独占一行），头里压成常规多行声明，语义相同。

- [ ] **Step 2: 双构建同步**

Makefile 第 116 行 `CORE_SRCS` 中把 `socket_async.c` 替换为 `socket_ring.c socket_transport.c socket_writer.c`（同行内替换，其余不动）。

CMakeLists.txt 第 126 行 `core/socket_async.c` 替换为三行：

```
    core/socket_ring.c
    core/socket_transport.c
    core/socket_writer.c
```

- [ ] **Step 3: 确认构建变红（旧文件已不在列表）**

```bash
make build/libclogx.a 2>&1 | tail -n 3
```

Expected: 失败（找不到 `socket_async.c` 对应目标或链接缺符号）。此红是预期的，证明构建已切换到新文件列表。

---

### Task 3: 运行拆分脚本 + 去 static + 编译

**Files:**
- Create: `core/socket_ring.c`、`core/socket_transport.c`、`core/socket_writer.c`
- Delete: `core/socket_async.c`

- [ ] **Step 1: 写入并运行拆分脚本**

保存以下脚本为 `/tmp/split_socket.py`（不要提交到 repo），然后运行：

```python
SRC = "core/socket_async.c"
raw = open(SRC).read().split("\n")
if raw and raw[-1] == "":
    raw.pop()
assert len(raw) == 823, "expected 823 lines, got %d" % len(raw)

# (name, start, end, must_contain) —— 1-indexed 闭区间
SPECS = [
    ("head",      1,   38,  ["Lock-free async non-blocking socket writer"]),
    ("statedecl", 40,  52,  ["struct socket_writer_t", "backoff_ms"]),
    ("ring",      54,  337, ["socket_ring_get_batch", "Ring Buffer Implementation"]),
    ("transport", 339, 488, ["Exponential Backoff Helpers", "tls_send_nonblocking"]),
    ("writer",    490, 823, ["Writer Thread", "socket_writer_stop"]),
]

blocks = {}
covered = set()
for name, a, b, must in SPECS:
    blk = raw[a - 1:b]
    while blk and not blk[0].strip():
        blk.pop(0)
    while blk and not blk[-1].strip():
        blk.pop()
    text = "\n".join(blk)
    for m in must:
        assert m in text, "block %s missing %r" % (name, m)
    blocks[name] = blk
    covered.update(range(a, b + 1))

# 允许的未覆盖行：39（struct 分隔前的空行）、53（ring 分隔前的空行）、
# 338（transport 分隔前的空行）、489（writer 分隔前的空行）
assert covered == set(range(1, 824)) - {39, 53, 338, 489}, "coverage gap"

RING_INC = ["#include \"socket_async.h\"", "#include \"clog_port.h\"",
            "#include <stdlib.h>", "#include <string.h>", ""]
TRANSPORT_INC = ["#include \"socket_async.h\"", "#include \"clog_port.h\"",
                 "#include \"socket_async_internal.h\"", "#include <errno.h>",
                 "#include <fcntl.h>", "#include <string.h>", "",
                 "#if !defined(_WIN32)", "#include <arpa/inet.h>",
                 "#include <netinet/in.h>", "#include <sys/select.h>",
                 "#include <sys/socket.h>", "#include <unistd.h>", "#endif", "",
                 "#ifdef CLOG_USE_TLS", "#include <openssl/err.h>",
                 "#include <openssl/ssl.h>", "#endif", ""]
WRITER_INC = ["#include \"socket_async.h\"", "#include \"clog_port.h\"",
              "#include \"socket_async_internal.h\"", "#include <errno.h>",
              "#include <fcntl.h>", "#include <stdio.h>", "#include <stdlib.h>",
              "#include <string.h>", "",
              "#if !defined(_WIN32)", "#include <arpa/inet.h>",
              "#include <netinet/in.h>", "#include <sys/select.h>",
              "#include <sys/socket.h>", "#include <unistd.h>", "#endif", "",
              "#ifdef CLOG_USE_TLS", "#include <openssl/err.h>",
              "#include <openssl/ssl.h>", "#endif", ""]

open("core/socket_ring.c", "w").write(
    "\n".join(blocks["head"] + [""] + RING_INC + blocks["ring"] + [""]).rstrip("\n") + "\n")
open("core/socket_transport.c", "w").write(
    "\n".join(TRANSPORT_INC + blocks["transport"] + [""]).rstrip("\n") + "\n")
open("core/socket_writer.c", "w").write(
    "\n".join(WRITER_INC + blocks["writer"] + [""]).rstrip("\n") + "\n")
print("split OK")
```

```bash
python3 /tmp/split_socket.py && rm core/socket_async.c && git status --short
```

Expected: `split OK`；`socket_async.c` 已删除，三个新文件 untracked。

注意：
- transport include 块是原文 20–33 行的子集：去掉经 grep 验证无使用的 `<stdio.h>` / `<stdlib.h>`（transport 只用 memset/errno/socket/TLS）。
- ring include 块经 grep 验证：只用 malloc/free/memcpy，故只要 stdlib + string。
- writer 保留原文完整 include 块 + 内部头（writer 用 select/shutdown/fprintf/malloc/strlen/SSL_*，原文块全覆盖）。

- [ ] **Step 2: transport 六个函数去 static**

```bash
cd /home/quintin/Data/source/c_cpp/clogx && sed -i 's/^static uint32_t backoff_next/uint32_t backoff_next/; s/^static void backoff_sleep/void backoff_sleep/; s/^static int socket_set_nonblocking/int socket_set_nonblocking/; s/^static int socket_send_nonblocking/int socket_send_nonblocking/; s/^static int tls_send_nonblocking/int tls_send_nonblocking/; s/^static int$/int/' core/socket_transport.c && grep -n "^static" core/socket_transport.c; grep -n "^int$\|^uint32_t backoff_next\|^void backoff_sleep\|^int socket_\|^int tls_" core/socket_transport.c
```

Expected: 第一个 grep 无输出（文件内零 `static`）；第二个 grep 命中 6 处（`int` 独占一行的是 `socket_connect_nonblocking` 的首行，其签名第二行 `socket_connect_nonblocking(...` 不变；另 5 处为 backoff_next/backoff_sleep/set_nonblocking/send_nonblocking/tls_send_nonblocking 的定义行）。

注意：`s/^static int$/int/` 只匹配独占一行的 `static int`（即 390 行那个），其余五个 `static int <name>(...` 单行定义各由一条专属规则处理，互不重叠。

- [ ] **Step 3: 编译通过**

```bash
make build/libclogx.a 2>&1 | tail -n 3
```

Expected: 无 warning、无 error（`-Wall -Wextra -Wconversion` 全开，纯搬运不应产生任何新警告）。

---

### Task 4: 测试 + ABI 对照 + 提交搬运

**Files:**
- Test: `tests/test_socket_async.c`（只跑不改）

- [ ] **Step 1: socket 测试全过**

```bash
make build/test_socket_async && ./build/test_socket_async 2>&1 | tail -n 3
```

Expected: 全 PASS（含 writer integration），与 Task 1 基线一致。

- [ ] **Step 2: 全量测试**

```bash
make test 2>&1 | tail -n 5
```

Expected: 全部 PASS，exit 0（已知 `test_async_producer_pressure` 偶发 timing flake：若单测失败而独立重跑通过，记为 flake，不阻断）。

- [ ] **Step 3: ABI 符号对照**

```bash
nm -D build/libclogx.so | grep ' T ' | awk '{print $3}' | sort > /tmp/abi_after_sock.txt && diff /tmp/abi_base_sock.txt /tmp/abi_after_sock.txt && echo ABI-UNCHANGED
```

Expected: `ABI-UNCHANGED`。原理：去 static 的六个函数本就是小写内部名，未标记 `CLOGX_API`，且构建用 `-fvisibility=hidden`，不会进入动态符号表。

- [ ] **Step 4: 提交搬运（单 commit，保证可 revert）**

```bash
git add core/socket_ring.c core/socket_transport.c core/socket_writer.c core/socket_async_internal.h Makefile CMakeLists.txt && git rm -q core/socket_async.c && git commit -m "refactor(socket): ♻️ split socket_async.c into ring/transport/writer TUs (zero behavior change)" && git log --oneline -1
```

---

### Task 5: 全门禁 + CHANGELOG + 提交

**Files:**
- Modify: `CHANGELOG.md`（`Unreleased` 节）

- [ ] **Step 1: 全门禁**

```bash
make check 2>&1 | tail -n 8
```

Expected: format/tidy 干净后走完全部测试（已知 `mermaid-check` 因缺 jsdom 模块失败属环境问题：本分支零 mermaid 改动，若失败需确认 master 同样失败）。

Fallback（仅当 `clogx-unused-includes` 点名新文件的某个 `#include` 时执行）：删除被点名的那一行，重跑 `make build/libclogx.a` 确认零 warning，再重跑 `make check`。writer 保留的原文完整 include 块中个别头（如 fcntl/errno）在 writer 单元可能无使用，届时按点名删除即可，不猜。

- [ ] **Step 2: 加 Unreleased 条目**

在 `CHANGELOG.md` 的 `Unreleased` 节下追加（exact text）：

```markdown
- refactor(socket): ♻️ split `core/socket_async.c` into 3 themed TUs (ring/transport/writer + `socket_async_internal.h`); zero behavior change, no ABI change.
```

- [ ] **Step 3: 提交**

```bash
git add CHANGELOG.md && git commit -m "docs: 📝 changelog for socket_async.c split"
git log --oneline -3 && git status --short
```

Expected: 两个 commit（Task 4 搬运 + 本 CHANGELOG），工作树干净（除已知的 untracked plan 文件）。

---

## Self-Review（已执行）

1. **Spec 覆盖**：§1（三簇 + 内部头 + writer/启停同文件）→ Task 3 脚本块划分一一对应；§2（struct 原样搬/6 helper 去 static/注释归属/`──` 风格/构建同步）→ Task 2 头内容 + Task 3 include 块 + Task 2 Step 2；§3（白名单/验证矩阵/单 commit 回滚）→ Task 4 + Task 5（stale 注释修正随搬运自然发生，不单列步骤）。
2. **Placeholder 扫描**：脚本、sed 命令、include 块、commit message、CHANGELOG 文本全部完整，无 TBD/TODO/"酌情"。Task 5 Fallback 是带确切触发条件与确切动作的条件分支，非开放式描述。
3. **一致性**：块行号（head 1–38、struct 40–52、ring 54–337、transport 339–488、writer 490–823、空行 39/53/338/489）与实测一致；`socket_connect_nonblocking` 两行签名在头与 sed 中处理一致；去 static 的六个函数与内部头六个声明一一对应；TLS `#ifdef` 在头与 transport 文件中成对出现。
