# Q3 精修内功：Hygiene + Perf Baseline — Design Spec

- Date: 2026-10-05
- Status: approved by user, awaiting implementation plan
- Direction: approach 2 (small deterministic hardening + perf), Windows landing / new sinks / i18n explicitly out of scope

## §1 Scope

Zero ABI risk. No public API changes. Excludes: A3 Windows landing, new sinks, i18n/API changes (YAGNI).

## §2 Hygiene (deterministic, doc-only + one deletion)

- **H1**: delete stray gitignored backup `include/log.h.bak`.
- **H2**: fix 3× wrong include path in `docs/PLATFORM_SUPPORT.md` (lines 34, 65: `include/clog/port.h`; line 76: `clog/port.h`) → `include/clog_port.h` / `clog_port.h` (real header is `include/clog_port.h`).
- **H3**: ROADMAP fixes — vcpkg path typo + resolve §2 vs §3 contradiction on vcpkg bug status (wording: mark resolved).

## §3 Perf (measure first, gate on numbers)

- **P1**: run `benchmarks/` suite, compare against committed `benchmarks/.baseline`
  (known figures: throughput ~1.10M/s, sync ~2.23M/s, async ~1.17M/s).
- **P2**: ≥10%-gain gate — only changes clearing the bar land; otherwise record a no-change conclusion.

## §4 Verification

- `make check` (format → clang-tidy → unused-includes → clean rebuild → all 44 tests).
- CHANGELOG `Unreleased` entries (conventional-commit semantics).
