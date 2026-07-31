# clogx-unused-includes clang-tidy Check Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement a custom clang-tidy check that detects both direct and transitive unused `#include` directives in C99 source files.

**Architecture:** Full AST-based analysis using Clang's `RecursiveASTVisitor` for symbol usage tracking and `PPCallbacks` for include tracking. The check is built as a loadable shared library.

**Tech Stack:** C++ (Clang/LLVM API), CMake, clang-tidy framework

---

## File Structure

### Files to Create

| File | Purpose |
|------|---------|
| `clang-tidy/clogx-unused-includes/CMakeLists.txt` | Build config for the check as shared library |
| `clang-tidy/clogx-unused-includes/UnusedIncludesCheck.h` | Check class declaration |
| `clang-tidy/clogx-unused-includes/UnusedIncludesCheck.cpp` | Check implementation with AST analysis |
| `tests/clang-tidy/unused-includes/test_used.c` | Test: all includes used (no warnings) |
| `tests/clang-tidy/unused-includes/test_unused_direct.c` | Test: direct unused include (warning) |
| `tests/clang-tidy/unused-includes/test_unused_transitive.c` | Test: transitive unused (warning) |
| `tests/clang-tidy/unused-includes/test_macro_used.c` | Test: macro from header used (no warning) |
| `tests/clang-tidy/unused-includes/test_system_headers.c` | Test: system headers behavior |

### Files to Modify

| File | Changes |
|------|---------|
| `CMakeLists.txt` | Add `CLOG_BUILD_CLANG_TIDY_CHECKS` option and subdirectory |
| `.clang-tidy` | Enable `clogx-unused-includes` check |
| `Makefile` | Add `tidy-check` target for custom checks |

---

## Chunk 1: Build Infrastructure

### Task 1: Create CMakeLists.txt for the check

**Files:**
- Create: `clang-tidy/clogx-unused-includes/CMakeLists.txt`

- [ ] **Step 1: Create directory structure**

```bash
mkdir -p clang-tidy/clogx-unused-includes
```

- [ ] **Step 2: Write CMakeLists.txt**

```cmake
# Build configuration for clogx-unused-includes clang-tidy check
cmake_minimum_required(VERSION 3.14)

# Find LLVM/Clang packages
find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
message(STATUS "Using ClangConfig.cmake in: ${Clang_DIR}")

# Add check as shared library
add_library(clogx-unused-includes SHARED
    UnusedIncludesCheck.cpp
)

# Include directories
target_include_directories(clogx-unused-includes PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
)

# Compile definitions
target_compile_definitions(clogx-unused-includes PRIVATE
    ${LLVM_DEFINITIONS}
)

# Link against Clang libraries
target_link_libraries(clogx-unused-includes PRIVATE
    clangAST
    clangASTMatchers
    clangBasic
    clangFrontend
    clangSerialization
    clangTooling
    clangTidy
)

# Set output directory
set_target_properties(clogx-unused-includes PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/clang-tidy"
)
```

- [ ] **Step 3: Verify CMakeLists.txt syntax**

Run: `cd clang-tidy/clogx-unused-includes && cmake -S . -B /tmp/test-build 2>&1 | head -20`
Expected: CMake configuration output (may fail due to missing LLVM, but syntax should be valid)

- [ ] **Step 4: Commit**

```bash
git add clang-tidy/clogx-unused-includes/CMakeLists.txt
git commit -m "chore(clang-tidy): add CMakeLists.txt for unused-includes check"
```

---

### Task 2: Integrate with root CMakeLists.txt

**Files:**
- Modify: `CMakeLists.txt` (add option and subdirectory)

- [ ] **Step 1: Add option to root CMakeLists.txt**

Add after line 15 (after `CLOG_ENABLE_CLANG_TIDY` option):

```cmake
option(CLOG_BUILD_CLANG_TIDY_CHECKS "Build custom clang-tidy checks" OFF)
```

- [ ] **Step 2: Add subdirectory inclusion**

Add after the clang-tidy enable block (after line 25):

```cmake
if(CLOG_BUILD_CLANG_TIDY_CHECKS)
    add_subdirectory(clang-tidy/clogx-unused-includes)
endif()
```

- [ ] **Step 3: Verify CMake configuration**

Run: `cmake -S . -B build -DCLOG_BUILD_CLANG_TIDY_CHECKS=ON 2>&1 | grep -E "(clang-tidy|unused-includes)"`
Expected: Status messages about clang-tidy check

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt
git commit -m "chore(cmake): add CLOG_BUILD_CLANG_TIDY_CHECKS option"
```

---

## Chunk 2: Check Implementation

### Task 3: Implement UnusedIncludesCheck.h

**Files:**
- Create: `clang-tidy/clogx-unused-includes/UnusedIncludesCheck.h`

- [ ] **Step 1: Write the header file**

```cpp
//===--- UnusedIncludesCheck.h - clang-tidy check ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CLOGX_UNUSED_INCLUDES_CHECK_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CLOGX_UNUSED_INCLUDES_CHECK_H

#include "../ClangTidyCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Preprocessor.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace clang::tidy::clogx {

/// A clang-tidy check that detects unused #include directives.
///
/// This check analyzes the AST to find all referenced symbols and cross-references
/// them with declarations from included headers. It reports headers where no
/// symbols are used.
class UnusedIncludesCheck : public ClangTidyCheck {
public:
  UnusedIncludesCheck(StringRef Name, ClangTidyContext *Context);

  void registerPPCallbacks(const SourceManager &SM, Preprocessor *PP,
                           PreprocessorDesugarHelper *DP,
                           MacroExpansions &MEs) override;

  void registerMatchers(ast_matchers::MatchFinder *Finder) override;

  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;

  void onEndOfTranslationUnit() override;

private:
  // Track included files and their locations
  struct IncludeInfo {
    FileID File;
    SourceLocation Loc;
    bool IsSystemHeader;
    std::string FileName;
  };

  // Track which declarations come from which file
  std::map<FileID, std::set<std::string>> FileDeclarations;

  // Track all included files
  std::vector<IncludeInfo> IncludedFiles;

  // Track used symbols (function names, type names, etc.)
  std::set<std::string> UsedSymbols;

  // Track macro expansions
  std::set<std::string> UsedMacros;

  // Configuration options
  bool IgnoreSystemHeaders;
  bool IgnoreMacros;

  // Helper methods
  void collectDeclarations(const Decl *D);
  void collectUsedSymbols(const Stmt *S);
  bool isDeclarationFromIncludedFile(const Decl *D, FileID &FromFile);
  bool isSymbolUsedInFile(const std::string &Symbol, FileID File);
};

} // namespace clang::tidy::clogx

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CLOGX_UNUSED_INCLUDES_CHECK_H
```

- [ ] **Step 2: Verify header syntax**

Run: `clang++ -fsyntax-only -std=c++17 -I$(llvm-config --includedir) clang-tidy/clogx-unused-includes/UnusedIncludesCheck.h 2>&1`
Expected: No errors (may have warnings about missing implementation)

- [ ] **Step 3: Commit**

```bash
git add clang-tidy/clogx-unused-includes/UnusedIncludesCheck.h
git commit -m "feat(clang-tidy): add UnusedIncludesCheck header declaration"
```

---

### Task 4: Implement UnusedIncludesCheck.cpp

**Files:**
- Create: `clang-tidy/clogx-unused-includes/UnusedIncludesCheck.cpp`

- [ ] **Step 1: Write the implementation file**

```cpp
//===--- UnusedIncludesCheck.cpp - clang-tidy check --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "UnusedIncludesCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/MacroArgs.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/Support/raw_ostream.h"

using namespace ast_matchers;

namespace clang::tidy::clogx {

// Custom PPCallbacks to track includes
class IncludeTrackingCallbacks : public PPCallbacks {
public:
  IncludeTrackingCallbacks(UnusedIncludesCheck *Check, const SourceManager &SM)
      : Check(Check), SM(SM) {}

  void InclusionDirective(SourceLocation HashLoc, const Token &IncludeTok,
                          StringRef FileName, bool IsAngled,
                          CharSourceRange FilenameRange,
                          OptionalFileEntryRef File,
                          StringRef SearchPath, StringRef RelativePath,
                          const Module *Imported,
                          SrcMgr::CharacteristicKind FileType) override {
    if (!File)
      return;

    SourceLocation IncludeLoc = SM.getSpellingLoc(HashLoc);
    FileID IncludeFile = SM.getFileID(IncludeLoc);

    // Store include information
    UnusedIncludesCheck::IncludeInfo Info;
    Info.File = IncludeFile;
    Info.Loc = IncludeLoc;
    Info.IsSystemHeader = SM.isInSystemHeader(IncludeLoc);
    Info.FileName = FileName.str();

    Check->IncludedFiles.push_back(Info);
  }

private:
  UnusedIncludesCheck *Check;
  const SourceManager &SM;
};

UnusedIncludesCheck::UnusedIncludesCheck(StringRef Name,
                                         ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context),
      IgnoreSystemHeaders(Options.get("IgnoreSystemHeaders", false)),
      IgnoreMacros(Options.get("IgnoreMacros", false)) {}

void UnusedIncludesCheck::registerPPCallbacks(const SourceManager &SM,
                                              Preprocessor *PP,
                                              PreprocessorDesugarHelper *DP,
                                              MacroExpansions &MEs) {
  PP->addPPCallbacks(std::make_unique<IncludeTrackingCallbacks>(this, SM));
}

void UnusedIncludesCheck::registerMatchers(MatchFinder *Finder) {
  // Match all declarations to collect what's declared in each file
  Finder->addMatcher(decl().bind("decl"), this);

  // Match all used expressions/statements to find symbol usage
  Finder->addMatcher(expr().bind("expr"), this);
  Finder->addMatcher(stmt().bind("stmt"), this);
}

void UnusedIncludesCheck::check(const MatchFinder::MatchResult &Result) {
  const auto *D = Result.Nodes.getNodeAs<Decl>("decl");
  if (D) {
    collectDeclarations(D);
  }

  const auto *E = Result.Nodes.getNodeAs<Expr>("expr");
  if (E) {
    collectUsedSymbols(E);
  }

  const auto *S = Result.Nodes.getNodeAs<Stmt>("stmt");
  if (S) {
    collectUsedSymbols(S);
  }
}

void UnusedIncludesCheck::onEndOfTranslationUnit() {
  // Now analyze which includes are unused
  for (const auto &Include : IncludedFiles) {
    // Skip system headers if configured
    if (IgnoreSystemHeaders && Include.IsSystemHeader) {
      continue;
    }

    // Check if any symbol from this file is used
    bool Used = false;
    if (FileDeclarations.count(Include.File)) {
      for (const auto &Symbol : FileDeclarations[Include.File]) {
        if (UsedSymbols.count(Symbol) || UsedMacros.count(Symbol)) {
          Used = true;
          break;
        }
      }
    }

    if (!Used) {
      diag(Include.Loc, "unused include '%0'") << Include.FileName;
    }
  }
}

void UnusedIncludesCheck::collectDeclarations(const Decl *D) {
  // Skip invalid declarations
  if (!D || !D->hasValidDeclContext())
    return;

  const SourceManager &SM = D->getASTContext().getSourceManager();
  FileID File = SM.getFileID(D->getLocation());

  // Collect function declarations
  if (const auto *FD = dyn_cast<FunctionDecl>(D)) {
    FileDeclarations[File].insert(FD->getNameAsString());
  }

  // Collect variable declarations
  else if (const auto *VD = dyn_cast<VarDecl>(D)) {
    FileDeclarations[File].insert(VD->getNameAsString());
  }

  // Collect type declarations (struct, enum, typedef)
  else if (const auto *TD = dyn_cast<TagDecl>(D)) {
    FileDeclarations[File].insert(TD->getNameAsString());
  }

  // Collect typedef declarations
  else if (const auto *TDD = dyn_cast<TypedefNameDecl>(D)) {
    FileDeclarations[File].insert(TDD->getNameAsString());
  }
}

void UnusedIncludesCheck::collectUsedSymbols(const Stmt *S) {
  if (!S)
    return;

  const ASTContext &Context = S->getASTContext();
  const SourceManager &SM = Context.getSourceManager();

  // Collect function calls
  if (const auto *CE = dyn_cast<CallExpr>(S)) {
    if (const FunctionDecl *FD = CE->getDirectCallee()) {
      UsedSymbols.insert(FD->getNameAsString());
    }
  }

  // Collect DeclRefExpr (variable/type references)
  if (const auto *DRE = dyn_cast<DeclRefExpr>(S)) {
    if (const NamedDecl *ND = DRE->getDecl()) {
      UsedSymbols.insert(ND->getNameAsString());
    }
  }

  // Collect MemberExpr (struct member access)
  if (const auto *ME = dyn_cast<MemberExpr>(S)) {
    if (const NamedDecl *ND = ME->getMemberDecl()) {
      UsedSymbols.insert(ND->getNameAsString());
    }
  }

  // Recursively process child statements
  for (const Stmt *Child : S->children()) {
    collectUsedSymbols(Child);
  }
}

bool UnusedIncludesCheck::isDeclarationFromIncludedFile(const Decl *D,
                                                       FileID &FromFile) {
  if (!D)
    return false;

  const SourceManager &SM = D->getASTContext().getSourceManager();
  FromFile = SM.getFileID(D->getLocation());

  // Check if this file is in our included files list
  for (const auto &Include : IncludedFiles) {
    if (Include.File == FromFile) {
      return true;
    }
  }
  return false;
}

bool UnusedIncludesCheck::isSymbolUsedInFile(const std::string &Symbol,
                                             FileID File) {
  if (FileDeclarations.count(File)) {
    return FileDeclarations[File].count(Symbol) > 0;
  }
  return false;
}

} // namespace clang::tidy::clogx

// Register the check with clang-tidy
static ClangTidyCheckRegistry::Register<clang::tidy::clogx::UnusedIncludesCheck>
    X("clogx-unused-includes", "Detects unused #include directives");
```

- [ ] **Step 2: Verify compilation**

Run: `cmake --build build --target clogx-unused-includes 2>&1 | tail -20`
Expected: Successful compilation or errors to fix

- [ ] **Step 3: Commit**

```bash
git add clang-tidy/clogx-unused-includes/UnusedIncludesCheck.cpp
git commit -m "feat(clang-tidy): implement UnusedIncludesCheck with AST analysis"
```

---

## Chunk 3: Test Files

### Task 5: Create test_used.c (all includes used)

**Files:**
- Create: `tests/clang-tidy/unused-includes/test_used.c`

- [ ] **Step 1: Create test directory**

```bash
mkdir -p tests/clang-tidy/unused-includes
```

- [ ] **Step 2: Write test_used.c**

```c
// Test: All includes are used - should produce NO warnings
#include <stdio.h>
#include <stdlib.h>
#include "log.h"

int main(void) {
    // Using stdio.h
    printf("Hello\n");

    // Using stdlib.h
    int *p = malloc(sizeof(int));
    free(p);

    // Using log.h
    LOG_INFO("Test message");

    return 0;
}
```

- [ ] **Step 3: Commit**

```bash
git add tests/clang-tidy/unused-includes/test_used.c
git commit -m "test(clang-tidy): add test for all includes used"
```

---

### Task 6: Create test_unused_direct.c (direct unused include)

**Files:**
- Create: `tests/clang-tidy/unused-includes/test_unused_direct.c`

- [ ] **Step 1: Write test_unused_direct.c**

```c
// Test: Direct unused include - should produce warning for stdio.h
#include <stdio.h>
#include <stdlib.h>
#include "log.h"

int main(void) {
    // NOT using stdio.h (no printf, fprintf, etc.)

    // Using stdlib.h
    int *p = malloc(sizeof(int));
    free(p);

    // Using log.h
    LOG_INFO("Test message");

    return 0;
}
```

- [ ] **Step 2: Commit**

```bash
git add tests/clang-tidy/unused-includes/test_unused_direct.c
git commit -m "test(clang-tidy): add test for direct unused include"
```

---

### Task 7: Create test_unused_transitive.c (transitive unused)

**Files:**
- Create: `tests/clang-tidy/unused-includes/test_unused_transitive.c`

- [ ] **Step 1: Write test_unused_transitive.c**

```c
// Test: Transitive unused include - should produce warning for stdio.h
// stdlib.h is used, but stdio.h is not (even though it's included)
#include <stdio.h>
#include <stdlib.h>
#include "log.h"

// Custom header that includes stdio.h (for testing transitive)
#include "test_helper.h"

int main(void) {
    // Using stdlib.h
    int *p = malloc(sizeof(int));
    free(p);

    // Using log.h
    LOG_INFO("Test message");

    // NOT using stdio.h directly
    // NOT using anything from test_helper.h

    return 0;
}
```

- [ ] **Step 2: Create test_helper.h**

```c
// test_helper.h - includes stdio.h for transitive testing
#ifndef TEST_HELPER_H
#define TEST_HELPER_H

#include <stdio.h>

// Some function that uses stdio.h
void helper_print(const char *msg);

#endif // TEST_HELPER_H
```

- [ ] **Step 3: Commit**

```bash
git add tests/clang-tidy/unused-includes/test_unused_transitive.c tests/clang-tidy/unused-includes/test_helper.h
git commit -m "test(clang-tidy): add test for transitive unused include"
```

---

### Task 8: Create test_macro_used.c (macro from header used)

**Files:**
- Create: `tests/clang-tidy/unused-includes/test_macro_used.c`

- [ ] **Step 1: Write test_macro_used.c**

```c
// Test: Macro from header is used - should produce NO warnings
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "log.h"

int main(void) {
    // Using assert macro from assert.h
    int x = 42;
    assert(x > 0);

    // Using printf from stdio.h
    printf("x = %d\n", x);

    // Using malloc from stdlib.h
    int *p = malloc(sizeof(int));
    free(p);

    // Using LOG_INFO from log.h
    LOG_INFO("Test message");

    return 0;
}
```

- [ ] **Step 2: Commit**

```bash
git add tests/clang-tidy/unused-includes/test_macro_used.c
git commit -m "test(clang-tidy): add test for macro usage from header"
```

---

### Task 9: Create test_system_headers.c (system headers behavior)

**Files:**
- Create: `tests/clang-tidy/unused-includes/test_system_headers.c`

- [ ] **Step 1: Write test_system_headers.c**

```c
// Test: System headers behavior with IgnoreSystemHeaders option
// When IgnoreSystemHeaders=true, system headers should not produce warnings
// When IgnoreSystemHeaders=false (default), system headers should produce warnings
#include <stdio.h>   // Should warn if IgnoreSystemHeaders=false
#include <stdlib.h>  // Used - no warning
#include <string.h>  // Should warn if IgnoreSystemHeaders=false
#include "log.h"     // Used - no warning

int main(void) {
    // Using stdlib.h
    int *p = malloc(sizeof(int));
    free(p);

    // Using log.h
    LOG_INFO("Test message");

    // NOT using stdio.h (no printf, fprintf, etc.)
    // NOT using string.h (no strlen, strcpy, etc.)

    return 0;
}
```

- [ ] **Step 2: Commit**

```bash
git add tests/clang-tidy/unused-includes/test_system_headers.c
git commit -m "test(clang-tidy): add test for system headers behavior"
```

---

## Chunk 4: Integration and Configuration

### Task 10: Update .clang-tidy configuration

**Files:**
- Modify: `.clang-tidy` (enable the new check)

- [ ] **Step 1: Update .clang-tidy**

Add `clogx-unused-includes` to the Checks list:

```yaml
Checks: >
  -*,
  clogx-unused-includes,
  bugprone-*,
  clang-analyzer-*,
  performance-*,
  portability-*,
  readability-redundant-control-flow,
  readability-redundant-string-cstr,
  -bugprone-easily-swappable-parameters,
  -bugprone-macro-parentheses,
  -clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling,
  -clang-analyzer-valist.Uninitialized
```

- [ ] **Step 2: Add check options**

```yaml
CheckOptions:
  - key:   clang-analyzer-optin.performance.Padding.Threshold
    value: '16'
  - key:   clogx-unused-includes.IgnoreSystemHeaders
    value: false
  - key:   clogx-unused-includes.IgnoreMacros
    value: false
```

- [ ] **Step 3: Commit**

```bash
git add .clang-tidy
git commit -m "chore(clang-tidy): enable clogx-unused-includes check"
```

---

### Task 11: Add Makefile target

**Files:**
- Modify: `Makefile` (add tidy-check target)

- [ ] **Step 1: Add tidy-check target**

Add after the existing `tidy` target:

```makefile
# Run custom clang-tidy checks (requires building with CLOG_BUILD_CLANG_TIDY_CHECKS=ON)
tidy-check:
	@if [ ! -f build/clang-tidy/libclogx-unused-includes.so ]; then \
		echo "Error: Custom clang-tidy checks not built. Run:"; \
		echo "  cmake -S . -B build -DCLOG_BUILD_CLANG_TIDY_CHECKS=ON"; \
		echo "  cmake --build build"; \
		exit 1; \
	fi
	clang-tidy -p build \
		--load build/clang-tidy/libclogx-unused-includes.so \
		--checks='-*,clogx-unused-includes' \
		$(CLOG_SOURCES)
```

- [ ] **Step 2: Commit**

```bash
git add Makefile
git commit -m "chore(make): add tidy-check target for custom clang-tidy checks"
```

---

### Task 12: Update README.md with usage instructions

**Files:**
- Modify: `README.md` (add documentation for the new check)

- [ ] **Step 1: Add section to README.md**

Add after the "Static Analysis" section:

```markdown
### Custom clang-tidy Checks

The project includes a custom clang-tidy check `clogx-unused-includes` that detects unused `#include` directives.

**Build with custom checks:**

```bash
cmake -S . -B build -DCLOG_BUILD_CLANG_TIDY_CHECKS=ON
cmake --build build
```

**Run custom checks:**

```bash
make tidy-check
# Or directly:
clang-tidy -p build \
    --load build/clang-tidy/libclogx-unused-includes.so \
    --checks='-*,clogx-unused-includes' \
    $(CLOG_SOURCES)
```

**Configuration options:**

```yaml
CheckOptions:
  - key: clogx-unused-includes.IgnoreSystemHeaders
    value: false  # Set to true to skip <system> headers
  - key: clogx-unused-includes.IgnoreMacros
    value: false  # Set to true to skip macro-expanded includes
```
```

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "docs: add documentation for clogx-unused-includes check"
```

---

## Chunk 5: Verification

### Task 13: Build and test the check

**Files:**
- None (verification step)

- [ ] **Step 1: Build the check**

```bash
cmake -S . -B build -DCLOG_BUILD_CLANG_TIDY_CHECKS=ON
cmake --build build --target clogx-unused-includes
```

Expected: `build/clang-tidy/libclogx-unused-includes.so` exists

- [ ] **Step 2: Run on test files**

```bash
clang-tidy -p build \
    --load build/clang-tidy/libclogx-unused-includes.so \
    --checks='-*,clogx-unused-includes' \
    tests/clang-tidy/unused-includes/test_unused_direct.c
```

Expected: Warning about unused `stdio.h`

- [ ] **Step 3: Run on clogx sources**

```bash
make tidy-check
```

Expected: Any unused includes in clogx source files are reported

- [ ] **Step 4: Fix any reported issues**

If the check reports unused includes in clogx sources, remove them or add appropriate comments.

- [ ] **Step 5: Final commit**

```bash
git add -A
git commit -m "fix: address unused includes reported by clogx-unused-includes check"
```

---

## Summary

| Chunk | Tasks | Description |
|-------|-------|-------------|
| 1 | 1-2 | Build infrastructure (CMakeLists.txt, integration) |
| 2 | 3-4 | Check implementation (header + source) |
| 3 | 5-9 | Test files (5 test cases) |
| 4 | 10-12 | Integration and configuration |
| 5 | 13 | Verification and cleanup |

**Total tasks:** 13
**Estimated time:** 2-3 hours for implementation
