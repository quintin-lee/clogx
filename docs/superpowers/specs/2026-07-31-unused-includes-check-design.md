# Design Spec: clogx-unused-includes clang-tidy Check

**Date:** 2026-07-31
**Status:** Approved
**Author:** Sisyphus (AI Agent)

## Summary

Implement a custom clang-tidy check `clogx-unused-includes` that detects both direct and transitive unused `#include` directives in C99 source files. The check uses full AST-based analysis for maximum accuracy.

## Goals

1. Detect `#include` directives where nothing from that header is referenced
2. Detect headers included transitively but not directly needed
3. Treat all symbols equally (function calls, type references, macro usage, variable access)
4. Integrate into clogx's build system as part of `make tidy` / `make check-tidy`

## Non-Goals

- Support for C++ templates (project is C99 only)
- Auto-fix removal of includes (future enhancement)
- Cross-file include optimization (per-file analysis only)

## Approach: Full AST-Based

Use Clang's `RecursiveASTVisitor` to traverse the entire AST and collect all referenced symbols. Cross-reference with declarations from included headers to determine usage.

### Why Full AST?

- Most accurate — handles complex type hierarchies and macro expansions
- Well-supported by Clang API
- Consistent with how existing clang-tidy checks work

## Architecture

### Check Registration

```cpp
class UnusedIncludesCheck : public ClangTidyCheck {
public:
  UnusedIncludesCheck(StringRef Name, ClangTidyContext *Context);
  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
};
```

**Configuration options:**
```yaml
Checks: '-*,clogx-unused-includes'
CheckOptions:
  - key: clogx-unused-includes.IgnoreSystemHeaders
    value: false
  - key: clogx-unused-includes.IgnoreMacros
    value: false
```

### Core Algorithm

#### Phase 1: Include Tracking
- Use `PPCallbacks` to record every `#include` directive
- Store: filename, location, whether it's a system header (`<>` vs `""`)
- Track the "inclusion graph" for transitive dependency detection

#### Phase 2: Symbol Usage Analysis
- Use `RecursiveASTVisitor` to walk the entire AST
- Collect every referenced symbol:
  - Function declarations and calls
  - Type references (struct, enum, typedef)
  - Variable references
  - Macro expansions (via `PPCallbacks`)
- For each included header, check if ANY of its declarations are in the "used" set

#### Phase 3: Transitive Analysis
- If header A includes header B, and you use symbols from B:
  - Direct include of B → **used**
  - Indirect include via A → B is **transitively used**, but A may be **unused**
- The check reports headers where NO symbols from that header (or its transitive includes) are used

### Data Flow

```
Source File
  ↓
Preprocessor (track #includes) → Include Map
  ↓
AST Visitor (track used symbols) → Used Symbols Set
  ↓
Cross-reference: Include Map vs Used Symbols Set
  ↓
Report: Headers with zero used symbols
```

## File Structure

```
clang-tidy/
├── clogx-unused-includes/
│   ├── CMakeLists.txt          # Build config for the check
│   ├── UnusedIncludesCheck.h   # Check header
│   └── UnusedIncludesCheck.cpp # Check implementation
```

### Build Integration

In project root `CMakeLists.txt`:

```cmake
option(CLOG_BUILD_CLANG_TIDY_CHECKS "Build custom clang-tidy checks" OFF)

if(CLOG_BUILD_CLANG_TIDY_CHECKS)
    add_subdirectory(clang-tidy)
endif()
```

### Usage

```bash
cmake -S . -B build -DCLOG_BUILD_CLANG_TIDY_CHECKS=ON
cmake --build build
clang-tidy --load build/clang-tidy/libclogx-unused-includes.so ...
```

## Error Reporting

### Warning Format

```
warning: unused include 'foo.h' [-clogx-unused-includes]
  source.c:3:10: note: included here
```

### Diagnostic Levels

- `Warning` for unused includes (default)
- `Error` when configured with `WarningsAsErrors: '*'`

### Fix-it Hints (Future)

- Auto-fix available: remove the `#include` line
- For transitive includes: suggest removing the parent include if it was only included for the now-unused header

## Testing Strategy

### Test File Structure

```
tests/
├── clang-tidy/
│   ├── unused-includes/
│   │   ├── test_used.c           # All includes used → no warnings
│   │   ├── test_unused_direct.c  # Direct unused include → warning
│   │   ├── test_unused_transitive.c # Transitive unused → warning
│   │   ├── test_macro_used.c     # Macro from header used → no warning
│   │   └── test_system_headers.c # System headers behavior
│   └── check_test.c             # Test runner
```

### Test Approach

- Each test file is compiled with the check enabled
- Expected warnings are annotated with `// CHECK:` comments
- Run via `clang-tidy --load ... --checks='-*,clogx-unused-includes' test.c`

### Example Test

```c
#include <stdio.h>   // CHECK: warning: unused include 'stdio.h'
#include "log.h"     // No warning - LOG_INFO used

int main(void) {
    LOG_INFO("test");
    return 0;
}
```

## Edge Cases and Limitations

### Handled Edge Cases

1. **Include guards** — tracked via preprocessor, not affected
2. **Conditional includes** (`#ifdef`) — only track includes that are actually processed
3. **Macro-generated includes** — track via `MacroExpansion` callback
4. **Forward declarations** — if a header is included only for forward declarations that are never used, it's unused
5. **Circular includes** — handled via visited set in transitive analysis

### Known Limitations

1. **Platform-specific headers** — some headers have platform-specific declarations; the check may not detect all usages on all platforms
2. **Complex macro usage** — deeply nested macros may obscure symbol usage
3. **Template specializations** — C++ templates may have usage patterns that are hard to detect (less relevant for C99)
4. **Build system includes** — headers added via `-I` flags may have false positives if not properly resolved

### Configuration to Address Limitations

```yaml
CheckOptions:
  - key: clogx-unused-includes.IgnoreSystemHeaders
    value: false  # Set to true to skip <system> headers
  - key: clogx-unused-includes.IgnoreMacros
    value: false  # Set to true to skip macro-expanded includes
```

## Implementation Checklist

- [ ] Create `clang-tidy/clogx-unused-includes/` directory structure
- [ ] Implement `UnusedIncludesCheck.h` with check class declaration
- [ ] Implement `UnusedIncludesCheck.cpp` with full AST-based analysis
- [ ] Add CMakeLists.txt for building the check as a shared library
- [ ] Update root CMakeLists.txt with `CLOG_BUILD_CLANG_TIDY_CHECKS` option
- [ ] Create test files in `tests/clang-tidy/unused-includes/`
- [ ] Add test runner and integrate with `make tidy`
- [ ] Update `.clang-tidy` to enable the new check
- [ ] Update README.md with usage instructions
- [ ] Run on clogx source files and fix any reported issues

## References

- [Clang-Tidy Developer's Manual](https://clang.llvm.org/extra/clang-tidy/Contributing.html)
- [RecursiveASTVisitor](https://clang.llvm.org/docs/RAVFrontendAction.html)
- [PPCallbacks](https://clang.llvm.org/docs/html/Source/CodeGeneration.html)
- [Existing check: misc-include-cleaner](https://clang.llvm.org/extra/clang-tidy/checks/misc/include-cleaner.html)
