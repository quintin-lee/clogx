# Engineering Hardening: ABI Stability & ThreadSanitizer

- **Date**: 2026-08-02
- **Status**: Approved (brainstorming complete)
- **Scope**: `clogx` 0.2.1 → 0.3.0

## 1. Goal

Harden clogx for production adoption by (1) locking down the shared-library ABI with
symbol versioning / export whitelists on every platform, and (2) adding
ThreadSanitizer coverage to catch data races automatically in CI.

The immediate motivation is concrete: between v0.2.0 and v0.2.1 we shipped two
concurrency bugs (a lost-wakeup deadlock in `socket_ring_get_batch` and an async
flush race) that survived unit tests and were only found under race-y CI timing.
Both are exactly the class of bug TSan detects deterministically. We also ship a
shared library with a plugin ABI and no mechanism preventing accidental symbol
surface changes — a silent break waiting for the first downstream consumer.

## 2. Success Criteria

1. `nm -D --defined-only build/libclogx.so` shows exactly the 67 public symbols,
   each versioned `@@CLOGX_0_2` on Linux; macOS build exports exactly those 67 via
   `-exported_symbols_list`; no internal symbols leak on any platform.
2. `scripts/check_abi_exports.sh` fails (non-zero exit) when (a) a symbol in the
   map is not exported, or (b) an exported symbol is missing from the map — verified
   by adversarial tests in step 3 of the verification loop.
3. `make tsan` (and CMake `CLOG_ENABLE_TSAN=ON`) pass the full test suite on
   Linux and macOS, with a documented suppression list containing only external
   (libyaml) or intentionally-untested entries.
4. `make check` is green and includes the ABI export check; master CI is green
   with the new TSan and ABI jobs.
5. `CLOGX_API` on MSVC exports correctly when `CLOG_BUILD_SHARED=ON` (dllexport),
   fixing the current empty-definition gap.

## 3. Current State (verified 2026-08-02)

- 67 symbols exported from `build/libclogx.so`, all via `-fvisibility=hidden` +
  `CLOGX_API` (`__attribute__((visibility("default")))` for GCC/Clang).
- `CLOGX_API` is **empty on MSVC** (`include/log.h:86-88`): static builds work,
  shared DLL builds silently export nothing.
- `SO_VERSION = 0` hardcoded in Makefile (line 94); `SOVERSION 0` in
  `CMakeLists.txt:153`. Not derived from VERSION, no bump policy.
- No version script, no `.symver`, no `clogx.map`, no TSan target in
  Makefile/CMake/CI. (TSan was used manually once, per CHANGELOG, to find the
  ring-buffer race.)
- `ci.yml` matrix: ubuntu (makefile + cmake), macos, windows.
- Public API is already cleanly delimited by `-fvisibility=hidden`; the 67-symbol
  list matches the documented public headers (verified by spot-check).

## 4. Design

### 4.1 ABI Symbol Versioning (Linux/ELF)

- New `clogx.map` version script with node `CLOGX_0_2 { global: <67 symbols>;
  local: *; };` — `local: *` hides everything not listed, adding a link-time
  hard gate on top of `-fvisibility=hidden`.
- Link `build/libclogx.so` with `-Wl,--version-script=clogx.map` (Makefile +
  CMake shared target).
- Exported symbols become `log_init@@CLOGX_0_2`. `.symver` backward-compat
  scaffolding is **deferred** to the first real ABI break — zero downstream
  consumers today, so a single default version node suffices.
- Version node name derives from `VERSION` (major.minor): `CLOGX_0_2`.

### 4.2 Export Whitelist Check (`scripts/check_abi_exports.sh`)

- Compares `nm -D --defined-only build/libclogx.so` against the symbol list in
  `clogx.map` **in both directions**:
  - map has symbol but library does not export it → error (missing export);
  - library exports a symbol absent from map → error (accidental leak).
- Also validates `clogx.exports` (macOS list) against the same check via
  `nm -gU build/libclogx.dylib`.
- Wired into `make check` and the CI lint job.

### 4.3 macOS: Export List (Mach-O has no GNU versioning)

- New `clogx.exports` (one symbol per line, same 67 symbols).
- Link `__APPLE__` shared library with `-Wl,-exported_symbols_list,clogx.exports`.
- Documented in CONTRIBUTING that macOS enforces the whitelist at link time but
  has no version nodes — ABI policy (4.6) governs break discipline instead.

### 4.4 Windows/MSVC Export Fix

- `CLOGX_API` gains a `_WIN32 && CLOGX_BUILD_SHARED` branch:
  - defining library: `__declspec(dllexport)`;
  - consuming side: `__declspec(dllimport)` (guard with a `CLOGX_BUILD` define on
    the library build);
  - fallback: empty (current behavior) for static builds.
- This is a compile-time-only change; no behavioral impact on existing static
  consumers.

### 4.5 SO_VERSION Discipline

- Keep `SO_VERSION`/`SOVERSION` but make the bump policy explicit and documented
  in `docs/CONTRIBUTING.md`: bump +1 only on ABI-breaking changes (removing or
  changing the signature of a public function); never on additive changes while
  symbol versioning carries compatibility.
- Single source: still the `VERSION` file for project version; SO_VERSION is a
  deliberately separate, rarely-changed value (document why it is not derived
  from VERSION: ABI and project versions diverge by design).

### 4.6 ABI Policy Documentation (CONTRIBUTING)

New "ABI Stability" section:
- New public function: MUST be added to `clogx.map` + `clogx.exports` + marked
  `CLOGX_API`; `check_abi_exports.sh` enforces it.
- Removing/changing a public signature: MAJOR bump + SO_VERSION bump + new
  version node (`CLOGX_x_y`) with `.symver` retention — documented procedure for
  the first real break.
- macOS/Windows equivalence notes from 4.3/4.4.

### 4.7 TSan Integration

- **Makefile**: `make tsan` — build library + all tests with
  `-fsanitize=thread -fno-omit-frame-pointer` (compile and link), run the full
  test suite. Independent from `asan`/`ubsan` (sanitizers are mutually exclusive).
- **CMake**: `CLOG_ENABLE_TSAN=ON` option injecting the same flags.
- **CI**: new TSan job in `ci.yml` matrix on ubuntu-latest and macos-latest.
- **Known-risk handling** (predicted, confirmed during implementation):
  - libyaml is not instrumented — suppress external races via `TSAN_OPTIONS` or a
    suppressions file; document each entry.
  - signal tests (`test_signal_handler`, `test_coverage_deep` signal segments) may
    misfire under TSan — use `catch_signals: false` configs or a documented skip.
  - `pthread_atfork` + TSan is a known-flaky combination — if hit, set
    `detect_deadlocks=0` or downgrade fork tests under TSan.
  - `TSAN_OPTIONS=halt_on_error=1` so the first race fails the run.

## 5. Out of Scope (explicitly rejected)

- **libabigail `abidiff` CI gate**: user chose symbol versioning over abidiff.
  `check_abi_exports.sh` is the maintenance companion for the version script, not
  a full type-level ABI diff tool.
- **GNU symbol versioning on macOS**: impossible (Mach-O); export list is the
  equivalent.
- **Symbol versioning nodes beyond the first**: deferred to first ABI break.
- **Windows TSan**: MSVC has no TSan; not in scope (user chose Linux + macOS).

## 6. Implementation Order

1. Generate `clogx.map` + `clogx.exports` from the current 67 exports (scripted,
   one-time).
2. Add `check_abi_exports.sh`; adversarial-test both failure directions.
3. Wire version script into Makefile + CMake shared builds; verify `nm -D`
   output (67 `@@CLOGX_0_2` symbols on Linux).
4. macOS export list in both build systems; verify via `nm -gU`.
5. MSVC `CLOGX_API` dllexport/dllimport branch; verify a Windows shared build.
6. `make tsan` + CMake option + suppressions; run Linux + macOS.
7. CI: TSan job (ubuntu + macos) + ABI check job; `make check` integration.
8. CONTRIBUTING ABI policy section; CHANGELOG entries.
9. Full verification loop (below).

## 7. Verification Loop (run before "done")

1. Shared build → `nm -D` shows exactly 67 symbols, all `@@CLOGX_0_2`.
2. `check_abi_exports.sh` passes in both directions.
3. Adversarial: remove a symbol from the map → script must fail;
   add an extra export → script must fail; restore → pass.
4. `make tsan` green on Linux and macOS (suppressions audited, external-only).
5. `make check` green; master CI green with new jobs.
6. `nm -gU` on macOS dylib matches `clogx.exports`.

## 8. Files Touched (expected)

- New: `clogx.map`, `clogx.exports`, `scripts/check_abi_exports.sh`
- Modified: `Makefile`, `CMakeLists.txt`, `include/log.h`, `.github/workflows/ci.yml`,
  `docs/CONTRIBUTING.md`, `CHANGELOG.md`
