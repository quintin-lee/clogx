# Project Maturity Infrastructure Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Harden the clogx project with production-grade CI, release automation, benchmarking, packaging, and developer tooling to v0.1.x release readiness.

**Architecture:** Each maturity item is an independent, self-contained change set — CI config files, CI workflow YAMLs, benchmark C source files, packaging configs, and pre-commit hooks. No source code changes to the library itself.

**Tech Stack:** GitHub Actions, CPack (CMake), Doxygen, clang-format, pre-commit framework, codecov, dependabot.

---

## Chunk 1: Cross-Platform CI

**Files:**
- Modify: `.github/workflows/ci.yml`

- [ ] **Step 1: Expand CI matrix to include macOS and Windows**
  Replace the single `runs-on: ubuntu-latest` in `build-and-test` job with a matrix that adds `macos-latest` and `windows-latest`, keeping the existing `ubuntu-latest` entries. The makefile target must be skipped on Windows (no GNU make by default). Add `shell: bash` and platform guards.

```yaml
# In build-and-test job, change strategy:
strategy:
  matrix:
    include:
      - os: ubuntu-latest
        build_type: makefile
      - os: ubuntu-latest
        build_type: cmake
      - os: macos-latest
        build_type: cmake
      - os: windows-latest
        build_type: cmake
      - os: macos-latest
        build_type: makefile  # will be skipped via if

# Each job step for makefile gets:
if: matrix.build_type == 'makefile' && matrix.os != 'windows-latest'
```

- [ ] **Step 2: Add macOS-specific setup steps**
  Add a step before build on macos to install libyaml via Homebrew:
  ```yaml
  - name: Install libyaml (macOS)
    if: matrix.os == 'macos-latest'
    run: brew install libyaml
  ```

- [ ] **Step 3: Add Windows-specific setup steps**
  Add a step before cmake build on Windows to install vcpkg dependencies:
  ```yaml
  - name: Install dependencies (Windows)
    if: matrix.os == 'windows-latest' && matrix.build_type == 'cmake'
    run: |
      vcpkg install yaml-cpp
  ```
  And update CMake configure to pass vcpkg toolchain:
  ```yaml
  - name: Configure with CMake (Windows)
    if: matrix.os == 'windows-latest' && matrix.build_type == 'cmake'
    run: >
      cmake -S . -B build
      -DCMAKE_TOOLCHAIN_FILE=${{ env.VCPKG_ROOT }}/scripts/buildsystems/vcpkg.cmake
      -DCLOG_ENABLE_TLS=ON
  ```

- [ ] **Step 4: Run existing tests on all platforms**
  Ensure `make test` / `ctest` steps run on each platform. For Windows, convert makefile test step to use `ctest` only (skip makefile test).

- [ ] **Step 5: Commit cross-platform CI**
  ```bash
  git add .github/workflows/ci.yml
  git commit -m "ci: expand CI matrix to macOS and Windows"
  ```

---

## Chunk 2: Dependabot Configuration

**Files:**
- Create: `.github/dependabot.yml`

- [ ] **Step 1: Create dependabot.yml**
  ```yaml
  version: 2
  updates:
    - package-ecosystem: "github-actions"
      directory: "/"
      schedule:
        interval: "weekly"
      open-pull-requests-limit: 5

    - package-ecosystem: "docker"
      directory: "/"
      schedule:
        interval: "weekly"
      open-pull-requests-limit: 5
  ```

- [ ] **Step 2: Commit dependabot config**
  ```bash
  git add .github/dependabot.yml
  git commit -m "ci: add dependabot configuration for dependency updates"
  ```

---

## Chunk 3: Release Automation

**Files:**
- Create: `.github/workflows/release.yml`
- Minor: `CHANGELOG.md` (format check)

- [ ] **Step 1: Create release workflow**
  ```yaml
  name: Release

  on:
    push:
      tags:
        - 'v*.*.*'

  permissions:
    contents: write

  jobs:
    release:
      runs-on: ubuntu-latest
      steps:
        - name: Checkout
          uses: actions/checkout@v7
          with:
            fetch-depth: 0

        - name: Generate changelog
          run: |
            echo "## Changes since last release" >> $GITHUB_STEP_SUMMARY
            git log ${{ github.event.before }}..${{ github.ref_name }} --oneline >> $GITHUB_STEP_SUMMARY

        - name: Create GitHub Release
          uses: softprops/action-gh-release@v1
          with:
            body_path: CHANGELOG.md
            files: |
              build-cmake/clogx.pc

        - name: Build artifacts
          run: |
            mkdir -p dist
            cmake -S . -B dist/build -DCMAKE_BUILD_TYPE=Release
            cmake --build dist/build --target clogx -j$(nproc)

        - name: Upload release assets
          uses: actions/upload-artifact@v4
          with:
            name: clogx-${{ github.ref_name }}
            path: |
              dist/build/libclogx.a
              dist/build/libclogx.so
              dist/build/clogx.pc
  ```

- [ ] **Step 2: Commit release workflow**
  ```bash
  git add .github/workflows/release.yml
  git commit -m "ci: add automated release workflow for version tags"
  ```

---

## Chunk 4: Benchmark Infrastructure

**Files:**
- Create: `benchmarks/CMakeLists.txt`
- Create: `benchmarks/benchmark_throughput.c`
- Create: `benchmarks/benchmark_async_vs_sync.c`
- Modify: `CMakeLists.txt` (add benchmark option)
- Modify: `Makefile` (add benchmark target)

- [ ] **Step 1: Create benchmarks/CMakeLists.txt**
  ```cmake
  add_executable(benchmark_throughput benchmark_throughput.c)
  target_link_libraries(benchmark_throughput clogx_objects Threads::Threads ${CLOG_YAML_TARGET})
  target_include_directories(benchmark_throughput PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

  add_executable(benchmark_async_vs_sync benchmark_async_vs_sync.c)
  target_link_libraries(benchmark_async_vs_sync clogx_objects Threads::Threads ${CLOG_YAML_TARGET})
  target_include_directories(benchmark_async_vs_sync PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  ```

- [ ] **Step 2: Create benchmarks/benchmark_throughput.c**
  ```c
  #include "log.h"
  #include <stdio.h>
  #include <time.h>

  int main(void) {
      log_config_t cfg = {0};
      cfg.format = "text";
      cfg.queue_size = 1024;
      log_config_set(&cfg);
      log_init(NULL);

      const int N = 100000;
      struct timespec ts_start, ts_end;
      clock_gettime(CLOCK_MONOTONIC, &ts_start);

      for (int i = 0; i < N; i++) {
          LOG_INFO("benchmark throughput message");
      }

      clock_gettime(CLOCK_MONOTONIC, &ts_end);
      double elapsed = (ts_end.tv_sec - ts_start.tv_sec) +
                       (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
      printf("throughput: %.0f msgs/sec\n", N / elapsed);

      log_destroy();
      return 0;
  }
  ```

- [ ] **Step 3: Create benchmarks/benchmark_async_vs_sync.c**
  ```c
  #include "log.h"
  #include <stdio.h>
  #include <time.h>

  static void bench_mode(const char *label, log_config_t *cfg) {
      log_config_set(cfg);
      log_init(NULL);

      const int N = 50000;
      struct timespec ts_start, ts_end;
      clock_gettime(CLOCK_MONOTONIC, &ts_start);

      for (int i = 0; i < N; i++) {
          LOG_INFO("async benchmark message");
      }

      clock_gettime(CLOCK_MONOTONIC, &ts_end);
      double elapsed = (ts_end.tv_sec - ts_start.tv_sec) +
                       (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
      printf("mode=%s elapsed=%.4fs msgs/sec=%.0f\n",
             label, elapsed, N / elapsed);

      log_destroy();
  }

  int main(void) {
      printf("=== clogx benchmark: async vs sync ===\n");

      log_config_t sync_cfg = {0};
      sync_cfg.format = "text";
      sync_cfg.queue_size = 0;
      sync_cfg.async = false;
      bench_mode("sync", &sync_cfg);

      log_config_t async_cfg = {0};
      async_cfg.format = "text";
      async_cfg.queue_size = 1024;
      async_cfg.async = true;
      bench_mode("async", &async_cfg);

      return 0;
  }
  ```

- [ ] **Step 4: Add CMake option for benchmarks**
  In `CMakeLists.txt`, after the test section, add:
  ```cmake
  option(CLOG_BUILD_BENCHMARKS "Build benchmark programs" OFF)

  if(CLOG_BUILD_BENCHMARKS)
      add_subdirectory(benchmarks)
  endif()
  ```

- [ ] **Step 5: Add Makefile benchmark target**
  In `Makefile`, add:
  ```makefile
  BENCHMARK_SOURCES = $(wildcard benchmarks/*.c)
  BENCHMARK_BINS = $(patsubst benchmarks/%.c,$(BUILD_DIR)/benchmark_%,$(BENCHMARK_SOURCES))

  benchmark: $(BENCHMARK_BINS)

  $(BUILD_DIR)/benchmark_%: benchmarks/%.c $(LIB_TARGET)
  	$(CC) $(CFLAGS) -Iinclude -o $@ $< -L$(BUILD_DIR) -lclogx $(LDFLAGS) -lm

  .PHONY: benchmark
  ```

- [ ] **Step 6: Commit benchmark infrastructure**
  ```bash
  git add benchmarks/ CMakeLists.txt Makefile
  git commit -m "feat: add benchmark suite for throughput and async/sync comparison"
  ```

---

## Chunk 5: Doxygen GitHub Pages Deployment

**Files:**
- Modify: `.github/workflows/ci.yml` (add docs job)
- Create: `.github/workflows/deploy-docs.yml`
- Modify: `Doxyfile` (ensure output is correct)

- [ ] **Step 1: Verify Doxyfile settings**
  Check that `Doxyfile` has `OUTPUT_DIRECTORY = docs/html` and `GENERATE_HTML = YES`. If not, fix.

- [ ] **Step 2: Add GitHub Pages deploy workflow**
  Create `.github/workflows/deploy-docs.yml`:
  ```yaml
  name: Deploy Documentation

  on:
    push:
      branches: [main]
    workflow_dispatch:

  permissions:
    contents: write
    pages: write
    id-token: write

  jobs:
    build-docs:
      runs-on: ubuntu-latest
      steps:
        - uses: actions/checkout@v7

        - name: Install doxygen
          run: sudo apt-get update && sudo apt-get install -y doxygen

        - name: Generate docs
          run: doxygen Doxyfile

        - name: Upload artifact
          uses: actions/upload-pages-artifact@v3
          with:
            path: docs/html

    deploy:
      needs: build-docs
      runs-on: ubuntu-latest
      environment:
        name: github-pages
        url: ${{ steps.deployment.outputs.page_url }}
      steps:
        - name: Deploy to GitHub Pages
          uses: actions/deploy-pages@v4
  ```

- [ ] **Step 3: Update README with docs link**
  Add a badges or link to the GitHub Pages site in README.md:
  ```markdown
  [![Docs](https://img.shields.io/badge/docs-GitHub_Pages-blue.svg)](https://quintin-lee.github.io/clogx/)
  ```

- [ ] **Step 4: Commit docs deployment**
  ```bash
  git add .github/workflows/deploy-docs.yml README.md
  git commit -m "docs: add GitHub Pages deployment workflow for Doxygen API docs"
  ```

---

## Chunk 6: CPack Packaging

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add CPack configuration to CMakeLists.txt**
  After the `install()` calls, add:
  ```cmake
  set(CPACK_PACKAGE_NAME "clogx")
  set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lightweight C99 logging library")
  set(CPACK_PACKAGE_VENDOR "quintin-lee")
  set(CPACK_PACKAGE_LICENSE "MIT")
  set(CPACK_GENERATOR "TGZ;ZIP;DEB;RPM")
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "quintin-lee")
  set(CPACK_RPM_PACKAGE_LICENSE "MIT")
  set(CPACK_DEBIAN_PACKAGE_DEPENDS "libyaml-0-2 (>= 0.2.5), libpthread")
  set(CPACK_RPM_PACKAGE_REQUIRES "libyaml >= 0.2.5")

  include(CPack)
  ```

- [ ] **Step 2: Commit CPack configuration**
  ```bash
  git add CMakeLists.txt
  git commit -m "ci: add CPack configuration for .deb/.rpm/.tar.gz packages"
  ```

---

## Chunk 7: Pre-commit Hooks

**Files:**
- Create: `.pre-commit-config.yaml`
- Create: `.pre-commit-hooks.yaml`

- [ ] **Step 1: Create .pre-commit-config.yaml**
  ```yaml
  repos:
    - repo: local
      hooks:
        - id: clang-format
          name: clang-format
          language: system
          entry: clang-format -i
          types: [c, h]
          pass_filenames: true

        - id: cmake-format
          name: cmake-format
          language: system
          entry: cmake-format -i
          types: [cmake]
          pass_filenames: true
  ```

- [ ] **Step 2: Create .pre-commit-hooks.yaml**
  ```yaml
  - id: clang-format
    name: clang-format
    entry: clang-format -i
    language: system
    types: [c, h]

    - id: cmake-format
      name: cmake-format
      entry: cmake-format -i
      language: system
      types: [cmake]
  ```

- [ ] **Step 3: Commit pre-commit config**
  ```bash
  git add .pre-commit-config.yaml .pre-commit-hooks.yaml
  git commit -m "ci: add pre-commit hooks forclang-format and cmake-format"
  ```

---

## Chunk 8: Public API Boundary Documentation

**Files:**
- Modify: `README.md` (add API section)
- Modify: `docs/CONTRIBUTING.md` (add public API note)

- [ ] **Step 1: Document public API in README**
  Add a section after Features listing exactly which headers are public:
  ```markdown
  ## Public API

  The public headers live in `include/`:
  - `log.h` — Primary macro API (`LOG_INFO`, `LOG_DEBUG`, etc.) and `log_init()`, `log_reload()`, `log_shutdown()`
  - `log_config.h` — Configuration struct and YAML config loading
  - `log_limits.h` — Buffer size configuration macros
  - `log_record.h` — Log record struct definition
  - `log_sink.h` — Sink creation and management functions
  - `log_sink.h` — Per-sink level control

  Headers in `include/` marked internal (e.g., `dispatcher.h`, `log_async.h`):
  are **not** part of the public API and may change without notice.
  ```

- [ ] **Step 2: Add public API note to CONTRIBUTING.md**
  Append to `docs/CONTRIBUTING.md`:
  ```markdown

  ## Public API Contract

  Only the following headers in `include/` define the stable public API:
  `log.h`, `log_config.h`, `log_limits.h`, `log_record.h`, `log_sink.h`.

  All other headers (`dispatcher.h`, `log_async.h`, `log_dispatcher.h`,
  `log_formatter.h`, `log_signal.h`, `log_rate_limit.h`, `queue.h`, `rotate.h`)
  are internal implementation details and subject to change between releases.

  Semantic versioning is followed: `MAJOR.MINOR.PATCH`.
  - `MAJOR` bump: breaking public API changes
  - `MINOR` bump: backward-compatible new features
  - `PATCH` bump: backward-compatible bug fixes
  ```

- [ ] **Step 3: Commit API boundary docs**
  ```bash
  git add README.md docs/CONTRIBUTING.md
  git commit -m("docs: document public API boundary and versioning policy")
  ```

