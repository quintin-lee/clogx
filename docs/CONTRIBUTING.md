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
make check        # Full quality gate: format → build → test
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
- All `.c`, `.h`, and `.md` files are formatted with **clang-format** using the project's `.clang-format` configuration.
- Run `make format` to automatically apply formatting.
- Compiler warnings treated as errors: `-Wall -Wextra -Wconversion` are enforced in the build.
- Follow existing code patterns and style observed in the repository.

## Commit Messages

Follow **[conventional commits](https://www.conventionalcommits.org/)**:

- `feat:` — new feature
- `fix:` — bug fix
- `docs:` — documentation only
- `chore:` — build/tooling changes
- `perf:` — performance improvement
- `test:` — adding or modifying tests

Example: `feat: add TLS socket sink support`

## Updating CHANGELOG

All non-trivial changes must update `CHANGELOG.md` using conventional commit semantics. Add an entry under `Unreleased` for new features and fixes; versioned entries remain unchanged.

## Pull Request Process

1. Branch from `main`: `git checkout -b feature/my-feature main`
2. Implement feature + related tests
3. Ensure all tests pass (`make check` and relevant sanitizer builds)
4. Update documentation if needed (API docs, examples, CHANGELOG)
5. Run `make format` to ensure code is properly formatted
6. Push branch and open a Pull Request against `main`
7. PR description should reference any related issues and summarize changes

The CI suite will run automatically on your PR. All checks must pass before merge.

## Public API Contract

Only the following headers in `include/` define the stable public API: `log.h`, `log_config.h`, `log_limits.h`, `log_record.h`, `log_sink.h`, `clogx_version.h`.

All other headers (`dispatcher.h`, `log_async.h`, `log_dispatcher.h`, `log_formatter.h`, `log_signal.h`, `log_rate_limit.h`, `queue.h`, `rotate.h`, `clog_port.h`) are internal implementation details and subject to change between releases.

Semantic versioning is followed: `MAJOR.MINOR.PATCH`.
- `MAJOR` bump: breaking public API changes
- `MINOR` bump: backward-compatible new features
- `PATCH` bump: backward-compatible bug fixes

The canonical project version lives in the `VERSION` file at the repository root. Both Makefile (`make all`) and CMake (`cmake --build`) auto-generate `include/clogx_version.h` from it. Do not hard-code version strings in source or build files.
