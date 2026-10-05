# Docs Consistency + Anti-Rot Design (方案 A)

> Date: 2026-10-06 | Status: draft (default-assumed, pending user sign-off: A / B / C / A+C)
> Scope: docs-only. Zero code, zero ABI, zero build impact.

## 1. 背景与证据

- `docs/user_manual.md` (1263 行,12 章)与 `README.md` (553 行)内容跟 v0.3.0 功能对得上,不陈旧。
- 真正的病三处(均已用 grep 取证):
  1. **数字腐烂**:`docs/ROADMAP.md` 硬编码 `44 个测试`、`97.8%`、`0.3.0` 现状、`ABI 0_2`(L3/L15-16/L58/L65/L72/L90/L95),每次发版即过期。
  2. **路径表述模糊**:`include/` 在源码树是平铺的,安装后才进 `include/clogx/`;README L162/L429/L497、CONTRIBUTING L82、manual L302 各说各话,未点明这一区别。
  3. **双语悬空**:README + manual 英文,ROADMAP 全中文;manual 标题 `(English Version)` 却无中文版。

## 2. 方案对照(已向用户呈现)

| 方案 | 内容 | 代价 | 结论 |
|---|---|---|---|
| A(推荐,默认假设) | 一致性修补 + 防腐,不写新文档 | 半天,小 diff | 先落地 |
| B | README + manual 全翻中文 | 量大 + 永久双语同步负担 | 除非明确要中文市场,否则不做 |
| C | API reference 补语义契约(前置条件/错误码/线程安全 ×67 符号) | 需逐个核对源码 | 视精力可选 |

## 3. 改动清单(仅 3 个 md 文件)

### 3.1 `docs/ROADMAP.md` — 去硬编码数字
- `44 个测试全部通过` → `测试全部通过(数量见 CI / make test)`。
- `97.8%+ 分支覆盖` → 以 `make coverage-gcov` 的 75% 门禁为准的表述。
- `ABI 0_2 仍锁定` → `ABI 版本见 clogx.map / clogx.exports`。
- 原则:**指向取数位置,不写快照数字**。

### 3.2 `docs/ROADMAP.md` — 顶部英文摘要
- 现有 blockquote 后加 Purpose / Status / Pointers 三行英文小结。

### 3.3 `README.md` — 路径澄清一句
- L497 段尾加:"> 注:仓库源码树中头文件平铺于 `include/`;`cmake --install` 后安装到 `<prefix>/include/clogx/`。"

### 3.4 `docs/user_manual.md` — 双语表态两句
- L1 标题下加:"> 中文版尚未提供,欢迎按 CONTRIBUTING 提交翻译。"
- L302 安装路径表述与 README 对齐。

### 明确不做
- 全文翻译(B)、API 语义补全(C)、任何 mermaid 图改动(mermaid-check 环境已坏,不碰图最安全)。

## 4. 验证与交付

- 验证:`grep -rn "44 个\|97.8%\|0_2" docs/ROADMAP.md` 归零;`make check-format` 通过;`git diff --stat` 确认仅 3 个 md 文件。
- CHANGELOG:Unreleased 加一条 docs 条目(与 Q3 惯例一致)。
- 交付:1 个 commit(`docs: 📝 ...`),纯文档,可直接推 master。
