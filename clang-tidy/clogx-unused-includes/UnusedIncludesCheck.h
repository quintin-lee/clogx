//===--- UnusedIncludesCheck.h - clang-tidy check ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CLOGX_UNUSED_INCLUDES_CHECK_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CLOGX_UNUSED_INCLUDES_CHECK_H

#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidyModule.h>
#include "clang/AST/ASTContext.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/FileEntry.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include <string>
#include <vector>

namespace clang::tidy::clogx {

/// A clang-tidy check that detects unused #include directives.
///
/// The check tracks every #include directive (via PPCallbacks) and every
/// symbol reference in the translation unit (via AST matchers). A header is
/// considered "used" when a symbol declared in it is referenced from a
/// different file. Headers with no such references are reported as unused.
class UnusedIncludesCheck : public ClangTidyCheck {
public:
  // Track included files and their locations
  struct IncludeInfo {
    const FileEntry *File;     // FileEntry of the INCLUDED file
    const FileEntry *Includer; // FileEntry of the file containing the directive
    SourceLocation Loc;        // location of the #include directive
    bool IsSystemHeader;
    std::string FileName;
  };

  UnusedIncludesCheck(StringRef Name, ClangTidyContext *Context);

  void registerPPCallbacks(const SourceManager &SM, Preprocessor *PP,
                           Preprocessor *ModuleExpanderPP) override;

  void registerMatchers(ast_matchers::MatchFinder *Finder) override;

  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;

  void onEndOfTranslationUnit() override;

  // Track included files (accessed by IncludeTrackingCallbacks)
  std::vector<IncludeInfo> IncludedFiles;

  // Mark a macro definition file as used when the macro is referenced from
  // a different file than the one it is defined in. Only applied when the
  // IgnoreMacros option is disabled.
  void noteMacroUsed(const SourceLocation &UseLoc, const SourceLocation &DefLoc,
                     const SourceManager &SM);

  // Mark a symbol's declaring file as used when the symbol is referenced
  // from a different file than the one declaring it.
  void noteSymbolUsed(const SourceLocation &UseLoc, const Decl *D,
                      const SourceManager &SM);

private:
  // Map from a file (includer) to the set of files that provide at least one
  // symbol referenced from within that includer's own content. Scoping by
  // includer is required: an include directive is only "used" if the symbols
  // it provides are actually consumed in the file containing the directive,
  // not merely somewhere else in the translation unit.
  llvm::DenseMap<const FileEntry *, llvm::DenseSet<const FileEntry *>>
      UsedFilesByFile;

  // Configuration options
  bool IgnoreSystemHeaders;
  bool IgnoreMacros;

  // Preprocessor (captured in registerPPCallbacks) and ASTContext (captured
  // from the first match result) — used for macro-body analysis at the end
  // of the translation unit.
  Preprocessor *PP = nullptr;
  ASTContext *AST = nullptr;

  // Helper methods
  void collectUsedSymbols(const Stmt *S, const SourceManager &SM);
  void collectUsedTypes(const TypeLoc &TL, const SourceManager &SM);
  void analyzeMacroBodies(const Preprocessor &PP);
};

} // namespace clang::tidy::clogx

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_CLOGX_UNUSED_INCLUDES_CHECK_H
