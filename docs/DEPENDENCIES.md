# Third-party Dependencies

This document lists third-party software dependencies used by clogx, their versions, licenses, and locations.

## Direct Dependencies

| Dependency | Version | License | Location | Purpose |
|------------|---------|---------|----------|---------|
| **libyaml** | 0.2.5 | BSD-3-Clause | `deps/libyaml/` (vendored) or system package `libyaml-dev` | YAML configuration parsing via libyaml API |
| **OpenSSL** | >= 1.0.2 | Apache 2.0 | System package (`libssl-dev`) | Optional TLS transport for socket sink (enabled with `TLS=1` or `CLOG_ENABLE_TLS=ON`) |
| **pthread** | N/A | Various (glibc) | System library | Threading support for async logging queue and signal handling |

## Build Tools

| Tool | License | Usage |
|------|---------|-------|
| **clang-format** | Apache 2.0 | Source code formatting (`make format`, `make check-format`) |
| **clang-tidy** | Apache 2.0 | Static analysis (`make tidy`, `make check-tidy`, `make tidy-check`) |
| **gcov / lcov** | GPL v3 / GPL v2+ | Branch coverage (`make coverage`, `make coverage-gcov`) |
| **valgrind** | GPL v2+ | Memory checking (`make test-valgrind`, ASan/UBSan compatible) |
| **AFL / libFuzzer** | Apache 2.0 / BSD | Fuzz testing (`make fuzz-build`) |

## Vendored Dependencies

clogx vendors a copy of **libyaml 0.2.5** in `deps/libyaml/` when system-wide installation is unavailable. The vendored copy is stripped of build artifacts and only includes necessary source files. Its license (BSD-3-Clause) is included in the vendored directory's LICENSE file.

## License Notice

All licenses for third-party dependencies are reproduced in their respective directories. When distributing software linked with clogx, ensure compliance with the applicable licenses of libyaml, OpenSSL, and any other included third-party code.

## Optional Features

- **TLS Support**: Requires OpenSSL >= 1.0.2. Enable via `make TLS=1` or CMake option `-DCLOG_ENABLE_TLS=ON`. Without TLS, the socket sink operates in plaintext only.
