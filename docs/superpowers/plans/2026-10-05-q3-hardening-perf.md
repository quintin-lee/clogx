# Q3 精修内功 (Hygiene + Perf Baseline) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land hygiene fixes H1–H3 and a measured perf verdict P1–P2 with zero ABI risk.

**Architecture:** Docs-only edits plus benchmark measurement; no C source changes unless the ≥10% gate in Task 5 identifies a qualifying optimization. Verification is `make check` plus CHANGELOG entries under `Unreleased`.

**Tech Stack:** C99, Makefile (`make check`, `make benchmark`), `scripts/benchmark_check.sh`, committed baseline `benchmarks/.baseline` (`throughput_throughput=1100626`, `sync_sync=2233298`, `async_async=1165383`).

---

## File map

- Delete: `include/log.h.bak` (stray gitignored backup, 14218 bytes — not tracked, plain `rm` is safe)
- Modify: `docs/PLATFORM_SUPPORT.md` (3 typo lines: 34, 65, 76)
- Modify: `docs/ROADMAP.md` (lines 56–57: absolute-path link + stale "(bug)" wording)
- Modify: `CHANGELOG.md` (append under `## [Unreleased]`)
- Read-only reference: `benchmarks/.baseline`, `Makefile` (`benchmark` target, line 263), `docs/superpowers/specs/2026-10-05-q3-hardening-perf-design.md`
- Do NOT touch: any `*.c`/`*.h` source, `VERSION`, `vcpkg.json`, `clogx.map`, `clogx.exports`

---

### Task 1: H1 — delete stray backup `include/log.h.bak`

**Files:**
- Delete: `include/log.h.bak`

- [ ] **Step 1: Confirm the file is untracked (safe to delete)**

Run: `git status --porcelain include/log.h.bak && git check-ignore -v include/log.h.bak`
Expected: first command prints nothing (untracked); second prints a line showing the `*.bak` ignore rule.

- [ ] **Step 2: Delete it**

Run: `rm include/log.h.bak && ls include/log.h.bak; echo "exit=$?"`
Expected: `ls: cannot access 'include/log.h.bak'` and `exit=2`.

- [ ] **Step 3: Commit**

Run: `git add -A && git status --porcelain`
Expected: empty output (nothing tracked changed — the file was ignored, so this commit is a no-op guard; if empty, skip the commit).
If output is non-empty (unexpected), inspect with `git diff --cached` and STOP — something else changed.

---

### Task 2: H2 — fix 3× `clog/port.h` typos in `docs/PLATFORM_SUPPORT.md`

**Files:**
- Modify: `docs/PLATFORM_SUPPORT.md` (lines 34, 65, 76)

- [ ] **Step 1: Fix line 34**

Old: `` - **socket** — built on the Winsock abstraction in `include/clog/port.h` ``
New: `` - **socket** — built on the Winsock abstraction in `include/clog_port.h` ``

- [ ] **Step 2: Fix line 65**

Old: `` is a no-op. Even though `include/clog/port.h` provides `clog_dlopen` ``
New: `` is a no-op. Even though `include/clog_port.h` provides `clog_dlopen` ``

- [ ] **Step 3: Fix line 76**

Old: `` `clog_dlsym` / `clog_dlclose` wrappers in `clog/port.h` to implement ``
New: `` `clog_dlsym` / `clog_dlclose` wrappers in `clog_port.h` to implement ``

- [ ] **Step 4: Verify zero remaining occurrences**

Run: `grep -rn "clog/port" -- . 2>/dev/null | grep -v build/ | grep -v deps/ | grep -v "^./.git/"`
Expected: no output.

- [ ] **Step 5: Commit**

```bash
git add docs/PLATFORM_SUPPORT.md
git commit -m "docs: 📝 fix clog_port.h include path typos in PLATFORM_SUPPORT"
```

---

### Task 3: H3 — fix ROADMAP vcpkg link + resolved-status wording

**Files:**
- Modify: `docs/ROADMAP.md` (lines 56–57)

- [ ] **Step 1: Replace the absolute-path link and stale "(bug)" wording**

Old (lines 56–57):

```
3. **vcpkg/构建依赖不一致(bug)**: [vcpkg.json](/data/home/quintin/workspace/source/c/clog/vcpkg.json) 声明 `yaml-cpp`,
   而 CMake/CPack 实际用的是 **`libyaml`**(`yaml_parser_*`, C 解析器),拉包会拉错依赖。
```

New:

```
3. **vcpkg/构建依赖不一致 ✅ 已修复**: [vcpkg.json](../../vcpkg.json) 声明 `yaml-cpp`,
   而 CMake/CPack 实际用的是 **`libyaml`**(`yaml_parser_*`, C 解析器),拉包会拉错依赖。(修复见 §3 A2)
```

- [ ] **Step 2: Verify the relative link target exists**

Run: `ls vcpkg.json && grep -n "libyaml" vcpkg.json | head -3`
Expected: `vcpkg.json` exists and mentions `libyaml` (confirming the "已修复" claim is true; if it still says `yaml-cpp`, STOP and report — the bug is NOT fixed).

- [ ] **Step 3: Commit**

```bash
git add docs/ROADMAP.md
git commit -m "docs: 📝 mark ROADMAP vcpkg issue resolved, fix absolute link"
```

---

### Task 4: P1 — run benchmarks, compare against committed baseline

**Files:** none modified (read-only measurement; cwd must be repo root).

- [ ] **Step 1: Build and run the benchmark suite**

Run: `make benchmark 2>&1 | tail -20`
Expected: three binaries run under `build/` (`benchmark_throughput`, plus async/sync), each printing its figures. `logs/` is created automatically.

- [ ] **Step 2: Compare current numbers against `benchmarks/.baseline`**

Run: `cat benchmarks/.baseline`
Expected reference values:

```
throughput_throughput=1100626
sync_sync=2233298
async_async=1165383
```

Record the fresh numbers next to these. Compute per-metric delta % = (new − baseline) / baseline × 100.

- [ ] **Step 3: Write down the verdict (no commit yet)**

If all deltas are within ±10% (noise): verdict = **no-change** — proceed to Task 6 with the no-change CHANGELOG variant.
If any metric regressed beyond −30%: STOP and report — that trips the CI `benchmark.yml` threshold (`CLOG_BENCH_THRESHOLD=30`) and is a real problem, out of this plan's scope.
If a metric improved ≥10% due to environment (not a code change): still **no-change** — do NOT update `benchmarks/.baseline` (it is a committed reference; refresh only via deliberate `make benchmark-baseline` + review, which this plan does not authorize).

---

### Task 5: P2 — ≥10%-gain gate (conditional code change)

**Files:** TBD by measurement — ONLY enter this task if Task 4 revealed a concrete, reproducible ≥10% optimization opportunity in our own code (not environment noise).

- [ ] **Step 1: State the opportunity in one sentence and STOP for user confirmation**

Report: which metric, current vs baseline numbers, hypothesized cause, proposed minimal diff. Do NOT edit code yet — the user must confirm the optimization is in-scope (zero ABI risk, no public header changes, `-Wconversion`-clean).

- [ ] **Step 2 (only after user confirms): profile → minimal diff → re-run `make benchmark` → confirm ≥10% gain persists across 3 runs**

- [ ] **Step 3 (only after user confirms): `make check` must be fully green, then commit with a `perf:` conventional-commit message**

If Task 4 verdict was **no-change**, skip this task entirely (mark complete as N/A).

---

### Task 6: CHANGELOG `Unreleased` entries + full gate

**Files:**
- Modify: `CHANGELOG.md` (under `## [Unreleased]`)

- [ ] **Step 1: Append the hygiene entry under the existing `### Fixed` section**

Insert after the `### Fixed` heading of `## [Unreleased]` (before the macOS dylib bullet):

```markdown
- Docs: fixed three wrong `clog/port.h` include paths in `docs/PLATFORM_SUPPORT.md` (real header is `include/clog_port.h`); marked the ROADMAP §2 vcpkg dependency issue as resolved (was already fixed per §3 A2) and replaced its stale absolute-path link; removed stray gitignored backup `include/log.h.bak`.
```

- [ ] **Step 2: Append the perf verdict entry under `### Changed` (no-change variant)**

Insert after the `### Changed` heading of `## [Unreleased]`:

```markdown
- Perf baseline check: re-ran `make benchmark` against the committed `benchmarks/.baseline`; all metrics within noise (±10%), no code change warranted. (If Task 5 landed a change, replace this bullet with: `perf(scope): <one-line description> — <metric> <before> → <after> (+<%>), verified across 3 runs.`)
```

- [ ] **Step 3: Run the full gate**

Run: `make check 2>&1 | tail -15`
Expected: format check, clang-tidy, unused-includes, clean rebuild, and all 44 tests pass. If anything fails: fix only what this plan caused; pre-existing failures get reported, not fixed.

- [ ] **Step 4: Commit**

```bash
git add CHANGELOG.md
git commit -m "docs: 📝 changelog entries for Q3 hygiene + perf baseline check"
```

---

## Self-review

1. **Spec coverage:** H1 → Task 1; H2 → Task 2 (exact lines 34/65/76 verified by read); H3 → Task 3 (exact lines 56–57 verified by read, link target `vcpkg.json` existence checked in-step); P1 → Task 4 (exact baseline numbers quoted); P2 ≥10% gate → Task 5 (conditional with user-confirmation stop); verification (`make check` + CHANGELOG) → Task 6. No gaps.
2. **Placeholder scan:** no TBD/TODO/"appropriate handling"; every edit shows exact old/new strings; every command shows expected output. Task 5's code content is inherently measurement-dependent — contained by requiring user confirmation before any edit.
3. **Type consistency:** N/A — no code or type definitions introduced; doc-only plan unless Task 5 activates.
