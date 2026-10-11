# formatter.c 拆成 4 文件 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `core/formatter.c`（1129 行）按 spec 拆成 4 个文件，零行为变化。

**Architecture:** 先建内部头（18 个声明）+ 双构建同步，然后 Python 脚本按（起始行，结束行，必含断言）一次性搬运 8 个代码块并去 `static`，断言每块命中一次。新文件只含搬运来的函数 + 文件头 docblock。

**Tech Stack:** C99，Makefile + CMake 双构建（同步缺一即断）。

---

## 文件结构说明

- 新建 `core/formatter_internal.h`：跨单元声明（Task 2）。
- 新建 `core/formatter_fmt.c`：fmt 编译解释（Task 3：F1/F2/F3 块）。
- 新建 `core/formatter_json.c`：JSON 渲染（Task 3：J1/J2 块）。
- 新建 `core/formatter_otel.c`：TraceContext + OTel（Task 3：O1–O4 块）。
- 修改 `core/formatter.c`：只删搬走的块 + 共享符号去 `static` + 删 3 个孤儿 `----` banner；公开 API 与调度逻辑一行不动。
- 修改 `Makefile` 第 116 行 `CORE_SRCS`、`CMakeLists.txt` 第 106 行附近源文件列表：各加 3 个新文件。
- 测试从 repo 根运行。无 TDD 红/绿循环：纯搬运，验证靠重编 + 现有测试 + 全量门禁。

**与 spec 的一处修正**：spec §2 写内部头"约 10 个声明"，caller 实测证明共享面更广——`append_json_kv`（otel:730 调用）、`is_zero_id`/`trace_id_hex`/`span_id_hex`（json+otel+fmt 三处调用）都必须去 `static` 进头。实际 18 个声明（17 函数 + 1 extern 全局），以本计划为准。

---

### Task 1: 建分支 + 基线（测试全绿 + ABI 快照）

**Files:**
- Modify: 无（本任务只建分支 + 基线）
- Test: 全套 `make test`

- [ ] **Step 1: 从 master 建功能分支**

```bash
git checkout -b refactor/formatter-split
git status --short
```

Expected: 新分支，切自 master，工作树干净（除已知的 untracked plan 文件外）。

- [ ] **Step 2: 基线全量测试通过**

```bash
make test 2>&1 | tail -n 5
```

Expected: 全部测试 PASS（exit 0）。重点记下 `test_json_formatter` 的结果（PASS），给 Task 4 对照。若已有失败，先停下上报，不开工。

- [ ] **Step 3: 保存 ABI 基线快照**

```bash
ls build/*.so && nm -D build/libclogx.so | grep ' T ' | awk '{print $3}' | sort > /tmp/abi_base_fmt.txt && wc -l /tmp/abi_base_fmt.txt
```

Expected: 显示 `.so` 文件名，快照约 74 行（数字以实际输出为准，记下来给 Task 4 对照）。

---

### Task 2: 内部头 + 双构建同步

**Files:**
- Create: `core/formatter_internal.h`
- Modify: `Makefile:116`、`CMakeLists.txt`（第 106 行附近源文件列表）

- [ ] **Step 1: 新建 `core/formatter_internal.h`**（exact content，直接写入）

```c
/**
 * @file formatter_internal.h
 * @brief Internal shared declarations for the split formatter modules.
 *
 * formatter.c keeps the shared append/format helpers and the public API;
 * formatter_fmt.c / formatter_json.c / formatter_otel.c implement the
 * pattern compiler, JSON renderer, and OTel renderer respectively.
 * All symbols here have hidden visibility (project builds with
 * -fvisibility=hidden); public API stays in include/log_formatter.h.
 */
#ifndef FORMATTER_INTERNAL_H
#define FORMATTER_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "log_limits.h"
#include "log_record.h"

/* ── Shared append helpers (defined in formatter.c) ── */
int append_buf_str(char **out, size_t *remaining, const char *str, size_t len);
int append_buf_u32(char **out, size_t *remaining, uint32_t val);
int append_buf_i32(char **out, size_t *remaining, int32_t val);
int append_buf_i64(char **out, size_t *remaining, int64_t val);
int append_buf_u64(char **out, size_t *remaining, uint64_t val);
int append_buf_u32_pad6(char **out, size_t *remaining, uint32_t val);
int append_json_escaped_string(char **out, size_t *remaining, const char *str);
int json_close_object(char *buf, char **out, size_t *remaining, const char *suffix);
int append_json_kv(char **out, size_t *remaining, const clog_kv_t *kv);

/* ── Shared scalar helpers (defined in formatter.c) ── */
void        format_sec_cached(time_t sec, const char *fmt, char *out_str, size_t out_str_size);
const char *level_to_string(log_level_t level);
int         is_zero_id(const uint8_t *id, int len);
void        trace_id_hex(const uint8_t trace_id[16], char *out);
void        span_id_hex(const uint8_t span_id[8], char *out);

/* Shared default time format (defined in formatter.c; OTel renderer uses it). */
extern char g_time_format_buf[64];

/* ── Renderer entry points (defined in their own TUs) ── */
int format_json_ex(log_record_t *restrict record,
                   char *restrict buf,
                   size_t      buf_size,
                   const char *time_format);
int format_otel_json(log_record_t *restrict record, char *restrict buf, size_t buf_size);
int format_impl(log_record_t *restrict record,
                char *restrict buf,
                size_t      buf_size,
                const char *fmt,
                const char *time_format);

#endif /* FORMATTER_INTERNAL_H */
```

类型来源确认：`log_record_t`/`clog_kv_t`/`log_level_t` 均在 `log_record.h`（`otel_severity_number` 是该头里的 `static inline`，新文件自动可用，无需声明）；`fmt_op_t`/`FMT_MAX_OPS` 在公开头 `log_formatter.h`，fmt 文件随原有 include 自动可用。

- [ ] **Step 2: 双构建同步**（两处都要改，缺一即断）

Makefile 第 116 行：

```bash
# 原：
CORE_SRCS = config.c formatter.c dispatcher.c queue.c async.c log.c rotate.c rate_limit.c signal_handler.c plugin_loader.c prometheus_exporter.c socket_async.c
# 改为（formatter.c 后加 3 个）：
CORE_SRCS = config.c formatter.c formatter_fmt.c formatter_json.c formatter_otel.c dispatcher.c queue.c async.c log.c rotate.c rate_limit.c signal_handler.c plugin_loader.c prometheus_exporter.c socket_async.c
```

CMakeLists.txt 第 106 行附近（`core/formatter.c` 之后加同样 3 行，保持原有缩进与排序风格，先读该区域确认 exact text 再改）：

```bash
grep -n "core/formatter.c" CMakeLists.txt
```

Expected: 找到行号，照 Makefile 同样顺序插入 `core/formatter_fmt.c`、`core/formatter_json.c`、`core/formatter_otel.c`。

- [ ] **Step 3: 此时编译仍通过**（新文件尚不存在，但构建系统用 glob 还是显式列表？若显式列表此时会断——先验证）

```bash
make build/libclogx.a 2>&1 | tail -n 3
```

Expected: 若构建系统是显式源文件列表，此步会报错找不到新文件——这是预期的，记下错误并继续 Task 3（新文件建好后即恢复）。若通过，同样继续。**不要**为此提交半成品。

---

### Task 3: 运行拆分脚本 + 编译

**Files:**
- Create: `core/formatter_fmt.c`、`core/formatter_json.c`、`core/formatter_otel.c`（脚本生成）
- Modify: `core/formatter.c`

- [ ] **Step 1: 写入并运行拆分脚本**

保存以下脚本为 `/tmp/split_formatter.py`（不要提交到 repo），然后运行。行号是 2026-10-11 实测（`grep -n "^}"` 交叉验证过），脚本单次运行、无移位问题：

```python
SRC = "core/formatter.c"
raw = open(SRC).read().split("\n")
if raw and raw[-1] == "":
    raw.pop()
assert len(raw) == 1129, "expected 1129 lines, got %d" % len(raw)

# (name, start, end, must_contain) —— 1-indexed 闭区间，首尾空行自动剥离
MOVES = [
    # fmt file pieces
    ("F1_cache",  101,  122,  ["g_fmt_cache", "TIME_BUF_SIZE"]),
    ("F2_token",  148,  167,  ["append_token"]),
    ("F3_comp",   749,  1025, ["token_match", "fmt_compile", "format_impl"]),
    # json file pieces
    ("J1_kv",     352,  397,  ["append_json_kv"]),
    ("J2_jsonex", 410,  521,  ["format_json_ex"]),
    # otel file pieces
    ("O1_nib",    527,  541,  ["hex_nibble"]),
    ("O2_dec",    543,  562,  ["hex_decode"]),
    ("O3_tp",     564,  609,  ["parse_traceparent"]),
    ("O4_otel",   615,  744,  ["format_otel_json"]),
]

def get(a, b):
    blk = raw[a - 1:b]
    while blk and not blk[0].strip():
        blk.pop(0)
    while blk and not blk[-1].strip():
        blk.pop()
    assert blk, "empty block %d-%d" % (a, b)
    return blk

blocks = {}
for name, a, b, must in MOVES:
    text = "\n".join(get(a, b))
    for m in must:
        assert m in text, "block %s missing %r" % (name, m)
    assert name not in blocks
    blocks[name] = text

INCLUDES = '#include "clog_port.h"\n#include "fast_ascii.h"\n#include "formatter_internal.h"\n#include "log_formatter.h"\n#include "log_internal.h"\n#include "log_limits.h"\n#include "log_record.h"\n#include <ctype.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <time.h>'

def docblock(name, what):
    return ('/**\n * @file %s\n * @brief %s.\n *\n * Split from formatter.c (2026-10-11, zero behavior change).\n * Shared helpers and cross-unit declarations live in\n * formatter_internal.h.\n */\n\n%s\n' % (name, what, INCLUDES))

files = {
    "core/formatter_fmt.c": docblock("formatter_fmt.c",
        "Pattern compiler (fmt_compile/token_match) and opcode interpreter (format_impl)") +
        blocks["F1_cache"] + "\n\n" + blocks["F2_token"] + "\n\n" + blocks["F3_comp"] + "\n",
    "core/formatter_json.c": docblock("formatter_json.c",
        "Single-line JSON renderer (format_json_ex) and KV appender") +
        blocks["J1_kv"] + "\n\n" + blocks["J2_jsonex"] + "\n",
    "core/formatter_otel.c": docblock("formatter_otel.c",
        "W3C TraceContext parser and OTel JSON renderer") +
        blocks["O1_nib"] + "\n\n" + blocks["O2_dec"] + "\n\n" +
        blocks["O3_tp"] + "\n\n" + blocks["O4_otel"] + "\n",
}
for path, content in files.items():
    open(path, "w").write(content)

# 从 formatter.c 删除已搬走的行（倒序删，无移位），再删 3 个孤儿 ---- banner
cuts = [(745, 1025), (615, 744), (564, 609), (543, 562), (527, 541),
        (410, 521), (352, 397), (148, 167), (101, 122)]
lines = list(raw)
for a, b in cuts:
    del lines[a - 1:b]
text = "\n".join(lines)
for banner in ("/*  W3C TraceContext parser",
               "/*  OTel JSON renderer",
               "/*  Pattern compiler"):
    assert text.count(banner) == 1, banner
    text = text.replace("/* " + "-" * 66 + "\n" + banner, banner)  # 先拼出三行体再整段删
# 上面 replace 只是定位哨兵；真正删除三行 banner 体：
import re
text, n = re.subn(r"/\* -{66}\n/\*  (W3C TraceContext parser|OTel JSON renderer|Pattern compiler)[^\n]*\n \*{66} \*/\n", "", text)
assert n == 3, "expected 3 orphan banners removed, got %d" % n
open(SRC, "w").write(text.rstrip("\n") + "\n")
print("split OK")
```

**注意**：脚本里 banner 删除用了"哨兵 replace + 正则整段删"两步——第一步 `text.replace` 若拼不出三行体则原样返回（无 harm），真正干活的是第二步正则；`assert n == 3` 是硬门：三个孤儿 banner（原 523–525 W3C、611–613 OTel、745–748 Pattern compiler）必须恰好各删一次。`Helpers` banner（原 123–125）保留（下面还有 helpers）。若 `assert` 失败，停下手工检查 banner exact text（`sed -n` 看三行），不要猜。

```bash
python3 /tmp/split_formatter.py && git status --short && git diff --stat
```

Expected: `split OK`；新文件 3 个 untracked；`core/formatter.c` 大幅缩小。

- [ ] **Step 2: 共享符号去 `static`**（在 `core/formatter.c` 内执行，用 sed 精确匹配，逐条确认计数）

以下符号去 `static`（`append_buf_*` 同时去 `inline`，变普通函数——C99 `inline` 无 extern 实例化会断链，变普通函数是唯一零风险做法；码量影响可忽略）：

```bash
cd /home/quintin/Data/source/c_cpp/clogx
sed -i 's/^static inline int append_buf_/int append_buf_/' core/formatter.c
sed -i 's/^static void format_sec_cached(/void format_sec_cached(/' core/formatter.c
sed -i 's/^static const char \*level_to_string(/const char *level_to_string(/' core/formatter.c
sed -i 's/^static int append_json_escaped_string(/int append_json_escaped_string(/' core/formatter.c
sed -i 's/^static int json_close_object(/int json_close_object(/' core/formatter.c
sed -i 's/^static int is_zero_id(/int is_zero_id(/' core/formatter.c
sed -i 's/^static void trace_id_hex(/void trace_id_hex(/' core/formatter.c
sed -i 's/^static void span_id_hex(/void span_id_hex(/' core/formatter.c
sed -i 's/^static char g_time_format_buf\[64\]/char g_time_format_buf[64]/' core/formatter.c
grep -c "^int append_buf_" core/formatter.c
grep -n "^static.*append_buf_\|^static.*format_sec_cached\|^static.*level_to_string\|^static.*append_json_escaped\|^static.*json_close_object\|^static.*is_zero_id\|^static.*trace_id_hex\|^static.*span_id_hex\|^static char g_time_format" core/formatter.c
```

Expected: 第一行输出 `6`；第二行输出为空（无残留）。**不要**动 `g_default_format`（仍只被本文件用，保持 static）、`hex_nibble`/`hex_decode`/`parse_traceparent`/`token_match`/`fmt_compile`（已随新文件搬走，在新文件内保持 static——脚本搬的是原文，含 `static`，正确）。

新文件内去 `static`（被跨单元调用的 4 个）：

```bash
sed -i 's/^static int append_json_kv(/int append_json_kv(/; s/^static int format_json_ex(/int format_json_ex(/' core/formatter_json.c
sed -i 's/^static int format_otel_json(/int format_otel_json(/' core/formatter_otel.c
sed -i 's/^static int format_impl(/int format_impl(/' core/formatter_fmt.c
grep -rn "^static int \(append_json_kv\|format_json_ex\|format_otel_json\|format_impl\)(" core/formatter_*.c
```

Expected: 无输出（4 个全去 static 成功）。`append_token` 保持 static（只被 format_impl 用，同文件）。

- [ ] **Step 3: 编译通过**

```bash
make build/libclogx.a 2>&1 | tail -n 3
```

Expected: 无 warning、无 error（`-Wall -Wextra -Wconversion` 全开；跨文件后最易爆 `size_t`↔`int`，有 warning 即修，不要 suppress）。

---

### Task 4: 测试 + ABI 对照 + 提交搬运

**Files:**
- Test: `test_json_formatter` + 全套

- [ ] **Step 1: formatter 专项测试**

```bash
make build/test_json_formatter && ./build/test_json_formatter 2>&1 | tail -n 3
```

Expected: 全 PASS（与 Task 1 基线一致）。

- [ ] **Step 2: 全量测试**

```bash
make test 2>&1 | tail -n 5
```

Expected: 全部 PASS，exit 0（已知 `test_async_producer_pressure` 偶发 timing flake：单测失败而独立重跑通过则记 flake，不阻断）。

- [ ] **Step 3: ABI 符号对照**

```bash
nm -D build/libclogx.so | grep ' T ' | awk '{print $3}' | sort > /tmp/abi_after_fmt.txt && diff /tmp/abi_base_fmt.txt /tmp/abi_after_fmt.txt && echo ABI-UNCHANGED
```

Expected: `ABI-UNCHANGED`（去 static 的函数全是 hidden 可见性，不得新增导出符号）。

- [ ] **Step 4: 对称性复查 + 提交**

```bash
git diff --stat && git status --short
```

Expected: `core/formatter.c` 缩小约 600 行；3 新 `.c` + 1 新 `.h`；`Makefile` + `CMakeLists.txt` 各改 1 处。删除行与新增行的差集应只有：内部头声明、去 static 关键字行、include 行、3 个删掉的孤儿 banner、新文件 docblock。

```bash
git add core/formatter.c core/formatter_fmt.c core/formatter_json.c core/formatter_otel.c core/formatter_internal.h Makefile CMakeLists.txt && git commit -m "refactor(formatter): ♻️ split formatter.c into 4 themed TUs (zero behavior change)"
```

---

### Task 5: 全门禁 + CHANGELOG + 提交

**Files:**
- Modify: `CHANGELOG.md`（`Unreleased` 节）
- Test: 全套

- [ ] **Step 1: 格式 + tidy**

```bash
make check 2>&1 | tail -n 8
```

Expected: format/tidy 干净后走完全部测试（已知 `mermaid-check` 因缺 jsdom 模块失败属环境问题：本分支零 mermaid 改动，若失败需确认 master 同样失败）。

- [ ] **Step 2: 加 Unreleased 条目**（exact text）

```markdown
- refactor(formatter): ♻️ split `core/formatter.c` into 4 themed TUs (formatter/formatter_fmt/formatter_json/formatter_otel + `formatter_internal.h`); zero behavior change, no ABI change.
```

- [ ] **Step 3: 提交**

```bash
git add CHANGELOG.md && git commit -m "docs: 📝 changelog for formatter.c split"
git log --oneline -3 && git status --short
```

Expected: 两个 commit（Task 4 搬运 + 本 CHANGELOG），工作树干净（除已知的 untracked plan 文件）。

---

## Self-Review（已执行）

1. **Spec 覆盖**：§1（4 单元划分）→ Task 3 脚本 MOVES 表一一对应（F1/F2/F3→fmt，J1/J2→json，O1–O4→otel）；§2（符号归属/内部头/构建同步）→ Task 2 头文件 + Task 3 Step 2 去 static + 双构建修改；§3（小修 allowlist/验证/回滚）→ 孤儿 banner 删除（allowlist 第 1 条的实例）+ Task 4/5 验证矩阵 + 单搬运 commit 回滚。
2. **Placeholder 扫描**：脚本、sed 命令、头文件内容、commit message、CHANGELOG 文本全部完整，无 TBD/TODO/"酌情"。两处已知问题（flake、mermaid）写明判定标准。
3. **一致性**：块行号经 `^}` 列表 + 分段原文抽读交叉验证；`parse_traceparent` 只被 otel 用（:666）→ 新文件内保持 static；`append_token` 只被 format_impl 用 → 保持 static；`g_default_format` 只被本文件用（:1074/1096/1119）→ 保持 static；`g_time_format_buf` 被 otel 用（:646）→ 去 static + header extern。`format_impl` 完整签名（:838–860）已实测，头文件声明与之一致。
