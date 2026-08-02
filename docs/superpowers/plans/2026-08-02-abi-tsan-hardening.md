# ABI Stability & ThreadSanitizer Hardening — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Lock down the clogx shared-library ABI with symbol versioning / export whitelists on all platforms and add ThreadSanitizer coverage (Linux + macOS) to catch data races automatically in CI.

**Architecture:** A GNU ld version script (`clogx.map`) version-links the 67 public symbols as `@@CLOGX_0_2` on Linux/ELF; macOS gets the equivalent via `-Wl,-exported_symbols_list` (`clogx.exports`); MSVC gets a proper `__declspec(dllexport/dllimport)` `CLOGX_API` branch. A POSIX shell check (`scripts/check_abi_exports.sh`) enforces that exported symbols exactly match the whitelist in both directions and is wired into `make check` + CI. TSan is added as `make tsan` (Makefile), `CLOG_ENABLE_TSAN` (CMake), and a ubuntu+macos CI job.

**Tech Stack:** C99, GNU ld / Apple ld64 / MSVC linkers, POSIX shell + awk, CMake ≥ 3.14, Makefile, GitHub Actions, ThreadSanitizer.

**Spec:** `docs/superpowers/specs/2026-08-02-abi-tsan-hardening-design.md`

---

### Task 1: Generate `clogx.map` + `clogx.exports` from the current 67 exports

**Files:**
- Create: `clogx.map`
- Create: `clogx.exports`

- [ ] **Step 1: Build the shared library and dump the current export list**

Run (from repo root):
```bash
make build/libclogx.so
nm -D --defined-only build/libclogx.so | awk '$2 == "T" {print $3}' | sort > /tmp/clogx-symbols.txt
wc -l /tmp/clogx-symbols.txt
```
Expected: `67 /tmp/clogx-symbols.txt` (any other count means the export surface drifted; stop and reconcile before continuing).

- [ ] **Step 2: Generate the version script and macOS export list**

Run:
```bash
cd /home/quintin/Data/source/c_cpp/clogx
{
  echo 'CLOGX_0_2 {'
  echo '  global:'
  sed 's/^/    /' /tmp/clogx-symbols.txt | sed 's/$/;/'
  echo '  local:'
  echo '    *;'
  echo '};'
} > clogx.map
cp /tmp/clogx-symbols.txt clogx.exports
rm -f /tmp/clogx-symbols.txt
```

- [ ] **Step 3: Verify the generated files**

Run:
```bash
head -12 clogx.map
echo "---"
wc -l clogx.exports
```
Expected: map starts with `CLOGX_0_2 {`, `global:`, then `    clog_clear_trace_context;` … and `clogx.exports` has 67 lines, one symbol per line, sorted.

- [ ] **Step 4: Commit**

```bash
git add clogx.map clogx.exports
git commit -m "build(abi): 📦 add symbol version script and macOS export whitelist"
```

---

### Task 2: `scripts/check_abi_exports.sh` — dual-direction ABI export check

**Files:**
- Create: `scripts/check_abi_exports.sh`

- [ ] **Step 1: Write the check script**

Create `scripts/check_abi_exports.sh` with exactly this content:

```bash
#!/usr/bin/env bash
# Verify the shared library's exported symbols exactly match the ABI whitelist.
#
# Usage: scripts/check_abi_exports.sh [path-to-shared-lib]
#   Default library: build/libclogx.so (or build/libclogx.0.dylib on macOS).
#
# Checks in both directions:
#   1. every symbol in clogx.map's global block is exported by the library;
#   2. every symbol the library exports is listed in clogx.map;
#   3. clogx.exports (macOS) matches the clogx.map global block exactly.
set -euo pipefail

LIB="${1:-}"
if [ -z "$LIB" ]; then
    if [ -f build/libclogx.so ]; then
        LIB=build/libclogx.so
    elif [ -f build/libclogx.0.dylib ]; then
        LIB=build/libclogx.0.dylib
    else
        echo "FAIL: no shared library found (pass a path as arg 1)" >&2
        exit 1
    fi
fi
MAP="clogx.map"

# Symbol names listed in the version script's global block.
map_symbols() {
    sed -n '/^CLOGX_/,/^};/p' "$MAP" \
        | grep -E '^[[:space:]]*[a-zA-Z_][a-zA-Z0-9_]*;' \
        | sed 's/;.*//; s/^[[:space:]]*//'
}

# Text symbols actually exported by the library, sans version/@ and leading _.
exported_symbols() {
    if [ "$(uname -s)" = "Darwin" ]; then
        nm -gU "$LIB" | awk '$2 == "T" {sub(/^_/, "", $3); print $3}'
    else
        nm -D --defined-only "$LIB" | awk '$2 == "T" {sub(/@@.*/, "", $3); print $3}'
    fi
}

fail=0

# Direction 1: whitelisted but missing from the library.
while read -r sym; do
    [ -z "$sym" ] && continue
    if ! exported_symbols | grep -qx "$sym"; then
        echo "FAIL: '$sym' listed in $MAP but NOT exported by $LIB" >&2
        fail=1
    fi
done < <(map_symbols)

# Direction 2: exported but not whitelisted (accidental leak).
while read -r sym; do
    [ -z "$sym" ] && continue
    if ! map_symbols | grep -qx "$sym"; then
        echo "FAIL: '$sym' exported by $LIB but NOT listed in $MAP" >&2
        fail=1
    fi
done < <(exported_symbols)

# macOS export list must match the version script's global block.
if [ -f clogx.exports ]; then
    if ! diff -q <(map_symbols) clogx.exports >/dev/null 2>&1; then
        echo "FAIL: clogx.exports differs from the clogx.map global block" >&2
        fail=1
    fi
fi

if [ "$fail" -ne 0 ]; then
    echo "ABI export check FAILED" >&2
    exit 1
fi
echo "OK: ABI exports match ($(map_symbols | wc -l | tr -d ' ') symbols)"
```

- [ ] **Step 2: Make it executable and verify it fails against a non-matching library**

Run:
```bash
chmod +x scripts/check_abi_exports.sh
cat > /tmp/clogx-extra.c <<'EOF'
int clogx_extra_export(void) { return 42; }
EOF
cc -shared -fPIC /tmp/clogx-extra.c -o /tmp/libextra.so
./scripts/check_abi_exports.sh /tmp/libextra.so
echo "exit=$?"
```
Expected: output contains `FAIL: 'clogx_extra_export' exported by /tmp/libextra.so but NOT listed in clogx.map` (plus direction-1 failures for the whitelisted-but-missing symbols), and `exit=1`. Clean up:
```bash
rm -f /tmp/clogx-extra.c /tmp/libextra.so
```

- [ ] **Step 3: Verify the adversarial "removed symbol" direction**

Run:
```bash
cp clogx.map /tmp/clogx.map.bak
sed -i '/log_init;/d' clogx.map
./scripts/check_abi_exports.sh build/libclogx.so
echo "exit=$?"
cp /tmp/clogx.map.bak clogx.map
rm -f /tmp/clogx.map.bak
```
Expected: output contains `FAIL: 'log_init' listed in clogx.map but NOT exported by build/libclogx.so`, `exit=1`, and the map is restored afterward.

- [ ] **Step 4: Commit**

```bash
git add scripts/check_abi_exports.sh
git commit -m "ci(abi): 🧹 add dual-direction ABI export check script"
```

---

### Task 3: Wire the version script into the Makefile shared build (Linux + macOS branch)

**Files:**
- Modify: `Makefile` (near line 94 for `SO_VERSION`, line 136 for the `$(SO_TARGET)` rule)

- [ ] **Step 1: Add the platform flag + comment for SO_VERSION**

In `Makefile`, replace the `SO_VERSION = 0` line (line 94) with:

```make
# ABI version for the shared library. Bump ONLY on ABI-breaking changes
# (removing/changing a public function signature). Additive changes are
# carried by symbol versioning (clogx.map), not by this value.
SO_VERSION = 0
```

- [ ] **Step 2: Add the platform-dependent link flag and apply it to the SO rule**

In `Makefile`, replace the `$(SO_TARGET)` rule (line 135-136):

```make
# Linux/ELF: version script gives symbols @@CLOGX_0_2 and hides everything else.
# macOS: Mach-O has no GNU versioning; the exported-symbols list is the
# link-time whitelist equivalent.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
SO_ABI_FLAGS := -Wl,-exported_symbols_list,clogx.exports
else
SO_ABI_FLAGS := -Wl,-soname,libclogx.so.$(SO_VERSION) -Wl,--version-script,clogx.map
endif

$(SO_TARGET): $(ALL_OBJS)
	$(CC) $(EXTRA_LDFLAGS) -shared $(SO_ABI_FLAGS) -o $@ $^ $(LDFLAGS)
```

> Note: the `-Wl,-soname` flag moved into the non-Darwin branch only (ld64 does not accept it). Linux behavior is byte-for-byte identical plus the new version script.

- [ ] **Step 3: Rebuild and verify versioned symbols**

Run:
```bash
make clean && make build/libclogx.so
nm -D --defined-only build/libclogx.so | grep -c '@@CLOGX_0_2'
nm -D --defined-only build/libclogx.so | awk '$2 == "T" {print $3}' | grep -v '@@' | head
```
Expected: `67` for the first count, and the second command prints nothing (every exported text symbol is versioned).

- [ ] **Step 4: Run the ABI check against the newly linked library**

Run:
```bash
./scripts/check_abi_exports.sh build/libclogx.so
echo "exit=$?"
```
Expected: `OK: ABI exports match (67 symbols)` and `exit=0`.

- [ ] **Step 5: Run the full test suite to confirm nothing broke**

Run:
```bash
mkdir -p logs && rm -f logs/*.log
make test
```
Expected: every test binary prints and exits 0 (final `exit 0`).

- [ ] **Step 6: Commit**

```bash
git add Makefile
git commit -m "build(abi): 🔒 version-link shared library via clogx.map on Linux, export list on macOS"
```

---

### Task 4: Wire the version script into the CMake shared build

**Files:**
- Modify: `CMakeLists.txt` (lines 136-154, the `add_library(clogx SHARED ...)` block)

- [ ] **Step 1: Add the linker flags and SOVERSION comment**

In `CMakeLists.txt`, replace the `set_target_properties(clogx PROPERTIES ...)` block (lines 150-154):

```cmake
# SOVERSION bumps ONLY on ABI-breaking changes; additive changes are carried
# by symbol versioning (clogx.map). Do not derive from PROJECT_VERSION.
set_target_properties(clogx PROPERTIES
    OUTPUT_NAME clogx
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
)

if(CLOG_BUILD_SHARED)
    if(APPLE)
        target_link_options(clogx PRIVATE
            "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/clogx.exports")
    else()
        target_link_options(clogx PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/clogx.map")
    endif()
endif()
```

- [ ] **Step 2: Configure, build the shared library, and verify versioned symbols**

Run:
```bash
rm -rf /tmp/clogx-cmake-abi && cmake -S . -B /tmp/clogx-cmake-abi -DCLOG_BUILD_SHARED=ON -DCLOG_BUILD_TESTS=OFF -DCLOG_BUILD_EXAMPLES=OFF > /dev/null
cmake --build /tmp/clogx-cmake-abi -j --target clogx 2>&1 | tail -2
nm -D --defined-only /tmp/clogx-cmake-abi/libclogx.so | grep -c '@@CLOGX_0_2'
```
Expected: build succeeds, and the count is `67`.

- [ ] **Step 3: Run the ABI check against the CMake-built library**

Run:
```bash
./scripts/check_abi_exports.sh /tmp/clogx-cmake-abi/libclogx.so
echo "exit=$?"
```
Expected: `OK: ABI exports match (67 symbols)` and `exit=0`.

- [ ] **Step 4: Confirm the static (default) build is unaffected**

Run:
```bash
rm -rf /tmp/clogx-cmake-static && cmake -S . -B /tmp/clogx-cmake-static > /dev/null
cmake --build /tmp/clogx-cmake-static -j --target clogx 2>&1 | tail -1
```
Expected: static library builds without error (version script flags only apply to `CLOG_BUILD_SHARED`).

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt
git commit -m "build(abi): 🔒 apply version script / export list to CMake shared target"
```

---

### Task 5: MSVC `CLOGX_API` dllexport/dllimport branch

**Files:**
- Modify: `include/log.h` (lines 82-89)
- Modify: `include/log_config.h` (lines 23-30)
- Modify: `CMakeLists.txt` (after the `target_link_libraries(clogx ...)` block, ~line 162)

- [ ] **Step 1: Update `CLOGX_API` in `include/log.h`**

Replace lines 82-89:

```c
/* Compiler portability macros. */
#if defined(_MSC_VER) && defined(CLOGX_BUILD_SHARED)
#define CLOGX_PRINTF_FMT(n, m)
#if defined(CLOGX_BUILD)
#define CLOGX_API __declspec(dllexport)
#else
#define CLOGX_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define CLOGX_PRINTF_FMT(n, m) __attribute__((format(printf, n, m)))
#define CLOGX_API __attribute__((visibility("default")))
#else
#define CLOGX_PRINTF_FMT(n, m)
#define CLOGX_API
#endif
```

- [ ] **Step 2: Update the `CLOGX_API` fallback in `include/log_config.h`**

Replace lines 23-30:

```c
/* Reuse CLOGX_API from log.h if already included, otherwise define it. */
#ifndef CLOGX_API
#if defined(_MSC_VER) && defined(CLOGX_BUILD_SHARED)
#if defined(CLOGX_BUILD)
#define CLOGX_API __declspec(dllexport)
#else
#define CLOGX_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define CLOGX_API __attribute__((visibility("default")))
#else
#define CLOGX_API
#endif
#endif
```

- [ ] **Step 3: Add the MSVC shared-build defines to CMake**

In `CMakeLists.txt`, directly after the `if(CLOG_ENABLE_TLS) ... endif()` block (around line 162), add:

```cmake
# Windows shared library: dllexport when building clogx, dllimport for consumers.
if(WIN32 AND CLOG_BUILD_SHARED)
    target_compile_definitions(clogx PRIVATE CLOGX_BUILD CLOGX_BUILD_SHARED)
    target_compile_definitions(clogx INTERFACE CLOGX_BUILD_SHARED)
endif()
```

- [ ] **Step 4: Verify the macro expands correctly (preprocessor test, runnable on Linux)**

Run:
```bash
printf '#define _MSC_VER 1937\n#define CLOGX_BUILD_SHARED 1\n#include "log.h"\nCLOGX_API void probe(void);\n' \
  | gcc -E -Iinclude -DCLOGX_BUILD - 2>/dev/null | grep -o '__declspec(dllexport)' | head -1
printf '#define _MSC_VER 1937\n#define CLOGX_BUILD_SHARED 1\n#include "log.h"\nCLOGX_API void probe(void);\n' \
  | gcc -E -Iinclude - 2>/dev/null | grep -o '__declspec(dllimport)' | head -1
printf '#include "log.h"\nCLOGX_API void probe(void);\n' \
  | gcc -E -Iinclude - 2>/dev/null | grep -c '__declspec'
```
Expected: first prints `__declspec(dllexport)`, second prints `__declspec(dllimport)`, third prints `0` (no declspec when not Windows-shared — static/GCC path unchanged). Repeat the three commands with `log_config.h` in place of `log.h` for the fallback definition.

- [ ] **Step 5: Confirm the normal Linux build is unaffected**

Run:
```bash
make clean && make > /dev/null 2>&1 && echo "make OK"
```
Expected: `make OK` (no warnings, since `_MSC_VER` is undefined on GCC the branch is skipped).

- [ ] **Step 6: Commit**

```bash
git add include/log.h include/log_config.h CMakeLists.txt
git commit -m "fix(port): 🐛 add MSVC dllexport/dllimport branch to CLOGX_API for shared builds"
```

---

### Task 6: `make tsan` (Makefile) + `CLOG_ENABLE_TSAN` (CMake) + suppressions file

**Files:**
- Modify: `Makefile` (near line 86 for sanitizer configs, near line 202 for targets, line 117 for `.PHONY`)
- Modify: `CMakeLists.txt` (near the top options block, ~line 29)
- Create: `tsan.supp`

- [ ] **Step 1: Add the TSan config and target to the Makefile**

In `Makefile`, after the `UBSAN_CFLAGS` line (line 88), add:

```make
TSAN_CFLAGS = -std=c99 -Wall -Wextra -Wconversion -Iinclude -Icore -O1 -g -D_GNU_SOURCE -fPIC -fvisibility=hidden -fsanitize=thread -fno-omit-frame-pointer
```

Add `tsan` to the `.PHONY` list (line 117). Then, after the `ubsan:` target (line 206-208), add:

```make
tsan:
	$(MAKE) clean
	TSAN_OPTIONS=halt_on_error=1:suppressions=tsan.supp $(MAKE) test CC=$(CC) CFLAGS="$(TSAN_CFLAGS)" EXTRA_LDFLAGS="$(TSAN_CFLAGS)"
```

- [ ] **Step 2: Add `CLOG_ENABLE_TSAN` to CMake**

In `CMakeLists.txt`, after the existing `option(...)` block for TLS (find it near line 29 area), add:

```cmake
option(CLOG_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
if(CLOG_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()
```

- [ ] **Step 3: Create the suppressions file**

Create `tsan.supp` with exactly this content (empty rule set; the `#` header documents intent):

```
# ThreadSanitizer suppressions for clogx.
#
# Entries are added ONLY for races involving external uninstrumented code
# (e.g. system libyaml) that cannot be instrumented in CI. Do NOT add entries
# for races inside clogx itself — those are real bugs to fix.
```

- [ ] **Step 4: Run the TSan suite locally (Linux)**

Run:
```bash
make tsan 2>&1 | tail -25
echo "exit=${pipestatus[1]}"
```
Expected: build succeeds with TSan flags and every test prints `=== build/test_* ===` + exits 0 (final `exit 0`). If a test reports a `WARNING: ThreadSanitizer:` race:
- If the race is inside clogx code → STOP, note the test name, and report it (it is a real bug the plan must extend to fix).
- If the race names external `libyaml`/`yaml_*` frames only → add a suppression rule to `tsan.supp`:
  ```
  race:yaml_
  ```
  then re-run `make tsan` until green.
- If `test_signal_handler` / fork tests hang or misfire under TSan → set `detect_deadlocks=0` in the `TSAN_OPTIONS` line of the `tsan:` target and re-run.

- [ ] **Step 5: Verify the CMake option configures**

Run:
```bash
rm -rf /tmp/clogx-cmake-tsan && cmake -S . -B /tmp/clogx-cmake-tsan -DCLOG_ENABLE_TSAN=ON > /dev/null
grep -q 'fsanitize=thread' /tmp/clogx-cmake-tsan/compile_commands.json && echo "TSAN in compile flags"
```
Expected: `TSAN in compile flags`.

- [ ] **Step 6: Commit**

```bash
git add Makefile CMakeLists.txt tsan.supp
git commit -m "test(tsan): 🔬 add make tsan + CLOG_ENABLE_TSAN with suppression file"
```

---

### Task 7: CI jobs (TSan on ubuntu+macos, ABI export check) + `make check` integration

**Files:**
- Modify: `.github/workflows/ci.yml`
- Modify: `Makefile` (the `check:` target, lines 245-252)

- [ ] **Step 1: Add the TSan job to ci.yml**

In `.github/workflows/ci.yml`, after the `asan:` job (lines 72-81), add:

```yaml
  tsan:
    runs-on: ${{ matrix.os }}
    strategy:
      fail-fast: false
      matrix:
        os: [ubuntu-latest, macos-latest]
    steps:
      - name: Checkout
        uses: actions/checkout@v7

      - name: Install libyaml (macOS)
        if: matrix.os == 'macos-latest'
        run: brew install libyaml

      - name: Build and test with ThreadSanitizer
        run: make tsan
```

- [ ] **Step 2: Add the ABI export check job to ci.yml**

In `.github/workflows/ci.yml`, after the `version-consistency:` job, add:

```yaml
  abi-exports:
    runs-on: ubuntu-latest
    steps:
      - name: Checkout
        uses: actions/checkout@v7

      - name: Install dependencies
        run: sudo apt-get update && sudo apt-get install -y libyaml-dev

      - name: Build shared library
        run: make build/libclogx.so

      - name: Verify ABI exports match the whitelist
        run: ./scripts/check_abi_exports.sh
```

- [ ] **Step 3: Integrate the ABI check into `make check`**

In `Makefile`, replace the `check:` target (lines 245-252):

```make
check:
	$(MAKE) check-format
	@if command -v clang-tidy >/dev/null 2>&1; then $(MAKE) check-tidy; fi
	@if command -v clang-tidy >/dev/null 2>&1; then $(MAKE) tidy-check; fi
	$(MAKE) clean
	$(MAKE) all
	./scripts/check_abi_exports.sh
	$(MAKE) test
	@echo "=== check passed ==="
```

- [ ] **Step 4: Validate the YAML and run `make check` locally**

Run:
```bash
python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/ci.yml')); print('ci.yml OK')"
make check 2>&1 | tail -5
```
Expected: `ci.yml OK`, then `=== check passed ===` (the full gate: format → tidy → clean build → ABI check → all tests).

- [ ] **Step 5: Commit**

```bash
git add .github/workflows/ci.yml Makefile
git commit -m "ci(tsan): 🔬 add TSan (ubuntu+macos) and ABI export check CI jobs"
```

---

### Task 8: CONTRIBUTING ABI policy + CHANGELOG entries

**Files:**
- Modify: `docs/CONTRIBUTING.md`
- Modify: `CHANGELOG.md`

- [ ] **Step 1: Add the ABI Stability section to CONTRIBUTING**

Append to `docs/CONTRIBUTING.md` a new section (place it before the release-workflow section if present):

```markdown
## ABI Stability

clogx ships a versioned shared library (`@@CLOGX_0_2` on Linux via `clogx.map`,
an `-exported_symbols_list` whitelist in `clogx.exports` on macOS) and a plugin
ABI (`CLOGX_PLUGIN_ABI_VERSION`). The rule: **the export surface is a reviewed,
explicit list, never an accident of what compiles.**

- **Adding a public function**: mark it `CLOGX_API`, add it to the `global:`
  block of `clogx.map` AND to `clogx.exports`. `scripts/check_abi_exports.sh`
  (part of `make check` and CI) fails if the two drift from the library.
- **Removing or changing a public signature** (ABI break): bump `VERSION` major,
  bump `SO_VERSION` in the Makefile and `SOVERSION` in CMake, and add a new
  version node `CLOGX_<major>_<minor>` to `clogx.map`; retain the previous
  node's symbols via `.symver` aliases so existing binaries keep resolving.
  The first real ABI break is when that `.symver` machinery gets built.
- **macOS**: Mach-O has no GNU symbol versioning; `clogx.exports` is the
  link-time whitelist. ABI break discipline is enforced by the SO_VERSION /
  version-node policy above, not by the linker.
- **Windows**: `CLOGX_API` expands to `__declspec(dllexport)` when building the
  shared library (`CLOGX_BUILD` + `CLOGX_BUILD_SHARED`) and `dllimport` for
  consumers. Static builds are unaffected (`CLOGX_API` stays empty).
- **Internal symbols**: `-fvisibility=hidden` plus the version script's
  `local: *` keep internals out of the ABI. Never mark an internal function
  `CLOGX_API`.
```

- [ ] **Step 2: Add CHANGELOG entries under `## [Unreleased]`**

In `CHANGELOG.md`, under `## [Unreleased]`, add to `### Added`:

```markdown
- Shared-library ABI locking: GNU ld version script (`clogx.map`, symbols exported as `@@CLOGX_0_2`) on Linux, `-exported_symbols_list` whitelist (`clogx.exports`) on macOS, and a `scripts/check_abi_exports.sh` dual-direction export checker wired into `make check` and CI so the public symbol surface can only change via explicit review
- ThreadSanitizer coverage: `make tsan` target (Makefile), `CLOG_ENABLE_TSAN` CMake option, and a CI job running the full suite under TSan on ubuntu-latest and macos-latest (with `tsan.supp` for external-code suppressions)
```

and to `### Fixed`:

```markdown
- MSVC `CLOGX_API` was empty, so a Windows shared-library build exported no symbols; it now uses `__declspec(dllexport)` / `dllimport` when `CLOGX_BUILD_SHARED` is defined (static builds unchanged)
```

- [ ] **Step 3: Commit**

```bash
git add docs/CONTRIBUTING.md CHANGELOG.md
git commit -m "docs: 📝 document ABI stability policy and TSan tooling"
```

---

### Task 9: Full verification loop

**Files:** none (verification only)

- [ ] **Step 1: Versioned symbols on Linux**

Run:
```bash
make clean && make build/libclogx.so
nm -D --defined-only build/libclogx.so | awk '$2 == "T" {print $3}' | grep -cv '@@'
nm -D --defined-only build/libclogx.so | awk '$2 == "T" {print $3}' | wc -l
```
Expected: `0` (no unversioned exports) and `67` (total).

- [ ] **Step 2: ABI check passes in both directions**

Run:
```bash
./scripts/check_abi_exports.sh build/libclogx.so
```
Expected: `OK: ABI exports match (67 symbols)`.

- [ ] **Step 3: Adversarial checks**

Run:
```bash
cp clogx.map /tmp/map.bak && sed -i '/log_init;/d' clogx.map && ./scripts/check_abi_exports.sh build/libclogx.so; e1=$?; cp /tmp/map.bak clogx.map; rm -f /tmp/map.bak
cat > /tmp/x.c <<'EOF'
int leaked_export(void) { return 1; }
EOF
cc -shared -fPIC /tmp/x.c -o /tmp/x.so && ./scripts/check_abi_exports.sh /tmp/x.so; e2=$?; rm -f /tmp/x.c /tmp/x.so
echo "removed-symbol exit=$e1  leaked-export exit=$e2"
```
Expected: `removed-symbol exit=1  leaked-export exit=1`.

- [ ] **Step 4: TSan green (Linux)**

Run:
```bash
make tsan 2>&1 | tail -5
echo "exit=${pipestatus[1]}"
```
Expected: final `exit 0` with all tests run. (macOS TSan is verified by the CI job; if you have a macOS box, `make tsan` there must also be green.)

- [ ] **Step 5: Full gate**

Run:
```bash
make check
```
Expected: `=== check passed ===`.

- [ ] **Step 6: macOS export list matches**

If a macOS machine is available: build the shared lib (Makefile or CMake `-DCLOG_BUILD_SHARED=ON`) and run:
```bash
./scripts/check_abi_exports.sh build/libclogx.0.dylib
```
Expected: `OK: ABI exports match (67 symbols)`. Otherwise note in the final report that macOS verification is pending the CI run.

- [ ] **Step 7: Push and confirm master CI is green**

Run:
```bash
git push origin master
```
Expected: push succeeds; the CI workflow (including the new `tsan` and `abi-exports` jobs) reports all green for the pushed commit. Report the run URL.

---

## Self-Review Notes

- **Spec coverage**: 4.1 (version script) → Tasks 1,3,4; 4.2 (check script, both directions) → Task 2 + adversarial steps; 4.3 (macOS export list) → Tasks 1,3,4 + Task 9 step 6; 4.4 (MSVC dllexport) → Task 5; 4.5 (SO_VERSION policy) → Tasks 3,4 + CONTRIBUTING; 4.6 (ABI policy docs) → Task 8; 4.7 (TSan make/CMake/CI + suppressions) → Task 6,7; §5 exclusions honored (no libabigail, no macOS GNU versioning, no Windows TSan); §6 order matches Tasks 1→9; §7 verification loop → Task 9.
- **Placeholder scan**: no TBD/TODO; every code step carries complete content; the TSan contingency (Step 4 of Task 6) is a documented decision procedure, not a placeholder.
- **Type/symbol consistency**: `CLOGX_0_2` node name, `clogx.map` / `clogx.exports` / `scripts/check_abi_exports.sh` / `make tsan` / `CLOG_ENABLE_TSAN` / `tsan.supp` / `CLOGX_BUILD_SHARED` / `CLOGX_BUILD` are used identically across all tasks.
