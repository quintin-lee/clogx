# clogx Contributing Guide

> This project follows the [Code of Conduct](CODE_OF_CONDUCT.md).

## Getting Started

Prerequisites required for development:

- `make` — GNU make
- `gcc` or compatible C compiler (C99 or later)
- `cmake` ≥ 3.14
- `libyaml-dev` — YAML configuration parsing
- `valgrind` — memory checking (optional but recommended)
- `clang-format` — code formatting (optional, used by `make format`)
- OpenSSL dev libraries (if building with TLS support: `TLS=1`)

Building and testing clogx requires no external installation beyond these tools.

## Building & Testing

```bash
make              # Build library and examples
make test         # Run all unit tests
make check        # Full quality gate: format check → clang-tidy → unused-includes → clean rebuild → tests
make asan         # Build + test with AddressSanitizer
make ubsan        # Build + test with UndefinedBehaviorSanitizer
make test-valgrind # Run all tests under Valgrind leak check
make fuzz-build   # Build AFL/libFuzzer targets
make format       # Apply clang-format to all source files
make docs         # Generate Doxygen API documentation
```

With CMake:

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Code Style

- Source code must be valid **C99**.
- All `.c` and `.h` files are formatted with **clang-format** using the project's `.clang-format` configuration (LLVM base, 4-space indent, 100 col, sorted includes).
- Run `make format` to automatically apply formatting.
- Compiler warnings treated as errors: `-Wall -Wextra -Wconversion` are enforced in the build.
- Follow existing code patterns and style observed in the repository.

## Commit Messages

Follow **[conventional commits](https://www.conventionalcommits.org/)** with a **gitmoji emoji** in the subject (see `git log` for examples):

- `feat(...): ✨` — new feature
- `fix(...): 🐛` — bug fix
- `docs(...): 📝` — documentation only
- `chore(...): 🔧` — build/tooling changes
- `perf(...): ⚡` — performance improvement
- `test(...): ✅` — adding or modifying tests
- `refactor(...): ♻️` — code refactoring
- `style(...): 🎨` — formatting / style fixes

Example: `feat(sink): ✨ add TLS socket sink support`

## Updating CHANGELOG

All non-trivial changes must update `CHANGELOG.md` using conventional commit semantics. Add an entry under `Unreleased` for new features and fixes; versioned entries remain unchanged.

## Pull Request Process

1. Branch from `master`: `git checkout -b feature/my-feature master`
2. Implement feature + related tests
3. Ensure all tests pass (`make check` and relevant sanitizer builds)
4. Update documentation if needed (API docs, examples, CHANGELOG)
5. Run `make format` to ensure code is properly formatted
6. Push branch and open a Pull Request against `master`
7. PR description should reference any related issues and summarize changes

The CI suite will run automatically on your PR. All checks must pass before merge.

## Public API Contract

Only the following headers in `include/` define the stable public API: `log.h`, `log_config.h`, `log_limits.h`, `log_record.h`, `log_sink.h`, `log_prometheus.h`, `clog_port.h`, `clogx_plugin.h` (installed to `include/clogx/`).

All other headers (`dispatcher.h`, `log_async.h`, `log_dispatcher.h`, `log_formatter.h`, `log_signal.h`, `log_rate_limit.h`, `queue.h`, `rotate.h`, `socket_async.h`) are internal implementation details and subject to change between releases.

Semantic versioning is followed: `MAJOR.MINOR.PATCH`.
- `MAJOR` bump: breaking public API changes
- `MINOR` bump: backward-compatible new features
- `PATCH` bump: backward-compatible bug fixes

The canonical project version lives in the `VERSION` file at the repository root. Both Makefile (`make all`) and CMake (`cmake --build`) auto-generate `include/clogx_version.h` from it. Do not hard-code version strings in source or build files.

## ABI Stability

clogx ships a versioned shared library (`@@CLOGX_0_2` on Linux via `clogx.map`, an `-exported_symbols_list` whitelist in `clogx.exports` on macOS) and a plugin ABI (`CLOGX_PLUGIN_ABI_VERSION`). The rule: **the export surface is a reviewed, explicit list, never an accident of what compiles.**

- The exported symbol set is generated from the `CLOGX_API`-marked public functions and locked in `clogx.map` (GNU/ELF version script) and `clogx.exports` (macOS export list). Any new public symbol must be added to both files, and removed public symbols must be deleted from both.
- `scripts/check_abi_exports.sh` (run as part of `make check` and the `abi-exports` CI job) verifies both directions: every symbol in the shared library is whitelisted, and every whitelisted symbol is actually exported. A leaked export or a missing symbol fails the gate.
- `SO_VERSION` is bumped only on a `MAJOR` release. Together with the version script it makes the soname (`libclogx.so.0`) the ABI contract downstream linkers enforce.
- Public symbols must be marked `CLOGX_API` (defined in `clog_port.h`) or `-fvisibility=hidden` will silently strip them from the shared library. Check `nm -D build/libclogx.so` after adding one.

## Releases

Bump the version and tag a release with `scripts/release.sh`:

```bash
./scripts/release.sh [--dry-run] [--push] [patch|minor|major|X.Y.Z]
```

The script keeps every version reference in sync (`VERSION`, `include/clogx_version.h`, `vcpkg.json`, the Makefile fallback, and the `CHANGELOG.md` heading), extracts the new CHANGELOG section as the annotated tag message, and with `--push` pushes both the commit and the tag. Without an explicit bump it auto-selects from the commit history: BREAKING CHANGE → major, `feat` → minor, else patch.

Safety checks built into the script:

- Refuses to run when the working tree has changes outside the five version files (so release commits never mix with unrelated work).
- Refuses to run when local HEAD is not equal to `origin/master` — it fetches first, so a stale local copy of the remote cannot mask an outdated branch.
- On an aborted run it prints rollback hints for the version files and the tag.

The GitHub Release itself is not created by the script — create it manually from the tag:

```bash
gh release create vX.Y.Z --generate-notes
```

CI enforces version consistency on every push/PR via `scripts/check_version_consistency.sh`.
