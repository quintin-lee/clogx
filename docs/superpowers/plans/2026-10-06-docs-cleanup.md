# Docs Consistency + Anti-Rot (方案 A) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate doc rot (snapshot numbers), clarify the `include/` vs `include/clogx/` layout, and close the bilingual gap with pointer-style fixes in 3 md files — zero code changes.

**Architecture:** Docs-only edits in `docs/ROADMAP.md`, `README.md`, `docs/user_manual.md`, plus a CHANGELOG Unreleased entry. Verification is grep-zero checks plus `make check-format`. No C source, no build, no ABI impact.

**Tech Stack:** Markdown, git, `make check-format`, grep verification.

---

## File map

- Modify: `docs/ROADMAP.md` (version banner L3–5, status table L15–16, §2 item 4 L58)
- Modify: `README.md` (append install-layout note after L497)
- Modify: `docs/user_manual.md` (insert language note after L1; L302 verified consistent, no edit)
- Modify: `CHANGELOG.md` (append under `## [Unreleased]`)
- Read-only reference: `docs/superpowers/specs/2026-10-06-docs-cleanup-design.md`
- Do NOT touch: any `*.c`/`*.h` source, `VERSION`, `vcpkg.json`, `clogx.map`, `clogx.exports`, any mermaid diagram block, `README.md` L37 (`97.8%` — explicitly out of scope, see Task 6 notes)

---

### Task 1: ROADMAP version banner + English summary (L3–5)

**Files:**
- Modify: `docs/ROADMAP.md:3-5`

- [ ] **Step 1: Reword the snapshot version line**

Old:
```
> 版本: 基于当前 `master`(0.3.0)代码分析与现状整理。
```

New:
```
> 版本: 基于当前 `master` 代码分析与现状整理(快照数字不写入本文,取数位置见各节)。
```

- [ ] **Step 2: Append a 3-line English summary after the blockquote (after L5, before `## 1. 现状全景`)**

Insert:
```
> **Purpose**: record project status, weak spots, and follow-up directions for release planning.
> **Status**: feature-complete C99 logging lib; see §1 for the pointer-style (non-snapshot) status table.
> **Pointers**: per-feature Windows status lives in [PLATFORM_SUPPORT.md](PLATFORM_SUPPORT.md).
```

- [ ] **Step 3: Verify rendering of the edited region**

Run: `sed -n '1,12p' docs/ROADMAP.md`
Expected: blockquote shows the reworded 版本 line plus the 3 English pointer lines, then `## 1. 现状全景`.

---

### Task 2: ROADMAP status-table anti-rot (L15–16)

**Files:**
- Modify: `docs/ROADMAP.md:15-16`

- [ ] **Step 1: Replace the test-count snapshot**

Old:
```
| 测试 | 44 个测试全部通过(100%),覆盖多实例、fork 安全、信号、TLS、KV、OTLP、queue、rotate 等 |
```

New:
```
| 测试 | 测试全部通过(数量与覆盖范围见 CI / `make test`),覆盖多实例、fork 安全、信号、TLS、KV、OTLP、queue、rotate 等 |
```

- [ ] **Step 2: Replace the hardening snapshot numbers**

Old:
```
| 硬化 | 67 符号锁定 ABI(`clogx.map` / `clogx.exports`)、97.8%+ 分支覆盖、clang-tidy 零警告、ASan/TSan/UBSan 可构建 |
```

New:
```
| 硬化 | 符号锁定 ABI(见 `clogx.map` / `clogx.exports` 的符号数)、分支覆盖以 `make coverage-gcov` 门禁为准、clang-tidy 零警告、ASan/TSan/UBSan 可构建 |
```

- [ ] **Step 3: Verify no snapshot numbers remain in §1**

Run: `grep -n "44 个\|97.8%\|67 符号" docs/ROADMAP.md`
Expected: empty output (exit 1).

---

### Task 3: ROADMAP §2 item 4 ABI pointer (L58)

**Files:**
- Modify: `docs/ROADMAP.md:58`

- [ ] **Step 1: Replace the `0_2` snapshot**

Old:
```
4. **发布定制到 0.3.0**: ABI `0_2` 仍锁定;未来 bump 需走 CONTRIBUTING 的规则。
```

New:
```
4. **发布定制**: ABI 版本以 `clogx.map` / `clogx.exports` 为准;未来 bump 需走 CONTRIBUTING 的规则。
```

- [ ] **Step 2: Verify**

Run: `grep -n "0_2\|0\.3\.0" docs/ROADMAP.md`
Expected: only historical completion markers remain — L65 (`方向 A: 发布就绪 (0.3.0 打磨)`), L72 (A4 `v0.3.0 已发布`), L95 (`交付 0.3.0`). These are **intentionally retained**: they record done work, not live status. If any other line matches, reword it the same pointer-style way.

---

### Task 4: README install-layout clarification (after L497)

**Files:**
- Modify: `README.md` (insert after line 497)

- [ ] **Step 1: Append the layout note after the installed-headers line**

Anchor (unchanged):
```
Installed public headers: `log.h`, `log_config.h`, `log_limits.h`, `log_record.h`, `log_sink.h`, `log_prometheus.h`, `clog_port.h`, `clogx_plugin.h` under `include/clogx/`.
```

Insert directly after it:
```
> 注:仓库源码树中头文件平铺于 `include/`;`cmake --install` 后安装到 `<prefix>/include/clogx/`。
```

- [ ] **Step 2: Verify L162/L429 stay consistent (read-only check)**

Run: `grep -n "clogx/log.h" README.md`
Expected: L162 and L429 still say `#include <clogx/log.h>` for the post-install case — consistent with the new note. No edit needed there.

---

### Task 5: user_manual language note (after L1)

**Files:**
- Modify: `docs/user_manual.md:1`

- [ ] **Step 1: Insert the Chinese-edition note after the title line**

Anchor (unchanged):
```
# clogx User Manual (English Version)
```

Insert directly after it:
```
> 中文版尚未提供,欢迎按 CONTRIBUTING 提交翻译。
```

- [ ] **Step 2: Verify L302 needs no change (read-only check)**

Run: `sed -n '302p' docs/user_manual.md`
Expected: `- Headers: \`/usr/local/include/clogx/log.h\`, ...` — already the installed layout, consistent with the README note. No edit.

---

### Task 6: CHANGELOG + format check + commit

**Files:**
- Modify: `CHANGELOG.md` (under `## [Unreleased]`)

- [ ] **Step 1: Append the docs entry**

Run: `sed -n '1,20p' CHANGELOG.md` to find the `## [Unreleased]` section, then add:
```
- docs(roadmap): remove snapshot numbers (test count, coverage %, ABI tag) in favor of pointer-style references.
- docs(readme/manual): clarify `include/` vs installed `include/clogx/` layout; note Chinese manual not yet available.
```

- [ ] **Step 2: Run the format check**

Run: `make check-format`
Expected: exit 0.

- [ ] **Step 3: Confirm the diff touches only the 4 intended md files**

Run: `git status --porcelain && git diff --stat`
Expected: only `docs/ROADMAP.md`, `README.md`, `docs/user_manual.md`, `CHANGELOG.md`. If anything else appears, STOP and inspect.

- [ ] **Step 4: Commit**

Run: `git add docs/ROADMAP.md README.md docs/user_manual.md CHANGELOG.md && git commit -m "docs: 📝 de-snapshot roadmap numbers, clarify include layout"`
Expected: commit created on current branch.

---

## Out of scope (explicitly NOT in this plan)

- Full Chinese translation (方案 B) — needs user sign-off on audience + ongoing sync burden.
- Per-function semantic contracts for 67 symbols (方案 C) — separate follow-up.
- `README.md` L37 `97.8%+` — left as-is to keep this plan minimal; covered if/when C or a README pass happens.
- Any mermaid block edits (`mermaid-check` env is broken: missing global jsdom, fails on pristine HEAD too).
