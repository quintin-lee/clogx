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
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/Support/raw_ostream.h"

namespace clang::tidy::clogx {

// Custom PPCallbacks to track includes and macro usage
class IncludeTrackingCallbacks : public PPCallbacks {
public:
  IncludeTrackingCallbacks(UnusedIncludesCheck *Check, const SourceManager &SM)
      : Check(Check), SM(SM) {}

  void InclusionDirective(SourceLocation HashLoc, const Token &IncludeTok,
                          StringRef FileName, bool IsAngled,
                          CharSourceRange FilenameRange,
                          OptionalFileEntryRef File,
                          StringRef SearchPath, StringRef RelativePath,
                          const Module *SuggestedModule,
                          bool ModuleImported,
                          SrcMgr::CharacteristicKind FileType) override {
    if (!File)
      return;

    SourceLocation IncludeLoc = SM.getSpellingLoc(HashLoc);

    // Store include information keyed by the INCLUDED file's FileEntry,
    // plus the file that contains the directive (the includer).
    UnusedIncludesCheck::IncludeInfo Info;
    Info.File = &(*File).getFileEntry();
    Info.Includer = SM.getFileEntryForID(SM.getFileID(IncludeLoc));
    Info.Loc = IncludeLoc;
    Info.IsSystemHeader = (FileType == SrcMgr::C_System ||
                           FileType == SrcMgr::C_ExternCSystem);
    Info.FileName = FileName.str();

    Check->IncludedFiles.push_back(Info);
  }

  // Mark a macro definition file as used when the macro is expanded
  void MacroExpands(const Token &MacroNameTok, const MacroDefinition &MD,
                    SourceRange Range, const MacroArgs *Args) override {
    noteMacroUse(MacroNameTok.getLocation(), MD);
  }

  // Mark a macro definition file as used when tested via 'defined'
  void Defined(const Token &MacroNameTok, const MacroDefinition &MD,
               SourceRange Range) override {
    noteMacroUse(Range.getBegin(), MD);
  }

  // Mark a macro definition file as used when tested via #ifdef
  void Ifdef(SourceLocation Loc, const Token &MacroNameTok,
             const MacroDefinition &MD) override {
    noteMacroUse(Loc, MD);
  }

  // Mark a macro definition file as used when tested via #ifndef
  void Ifndef(SourceLocation Loc, const Token &MacroNameTok,
              const MacroDefinition &MD) override {
    noteMacroUse(Loc, MD);
  }

  // Mark a macro definition file as used when tested via #elifdef
  void Elifdef(SourceLocation Loc, const Token &MacroNameTok,
               const MacroDefinition &MD) override {
    noteMacroUse(Loc, MD);
  }

  // Mark a macro definition file as used when tested via #elifndef
  void Elifndef(SourceLocation Loc, const Token &MacroNameTok,
                const MacroDefinition &MD) override {
    noteMacroUse(Loc, MD);
  }

private:
  void noteMacroUse(SourceLocation UseLoc, const MacroDefinition &MD) {
    // Ignore macro uses inside system headers: these are almost always
    // internal glibc/clang header cooperation (e.g. #ifdef __need_NULL
    // in stddef.h), not genuine user usage of the defining header.
    if (SM.isInSystemHeader(UseLoc))
      return;
    if (const MacroInfo *MI = MD.getMacroInfo()) {
      Check->noteMacroUsed(UseLoc, MI->getDefinitionLoc(), SM);
    }
  }

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
                                              Preprocessor *ModuleExpanderPP) {
  this->PP = PP;
  PP->addPPCallbacks(std::make_unique<IncludeTrackingCallbacks>(this, SM));
}

void UnusedIncludesCheck::registerMatchers(ast_matchers::MatchFinder *Finder) {
  // Match all expressions and statements to find symbol usage
  Finder->addMatcher(ast_matchers::expr().bind("expr"), this);
  Finder->addMatcher(ast_matchers::stmt().bind("stmt"), this);

  // Match all type locations to find type usage (typedefs, structs, enums)
  Finder->addMatcher(ast_matchers::typeLoc().bind("typeLoc"), this);
}

void UnusedIncludesCheck::check(
    const ast_matchers::MatchFinder::MatchResult &Result) {
  if (!Result.SourceManager)
    return;
  if (!AST)
    AST = Result.Context;

  const auto *E = Result.Nodes.getNodeAs<Expr>("expr");
  if (E) {
    collectUsedSymbols(E, *Result.SourceManager);
  }

  const auto *S = Result.Nodes.getNodeAs<Stmt>("stmt");
  if (S) {
    collectUsedSymbols(S, *Result.SourceManager);
  }

  if (const TypeLoc *TL = Result.Nodes.getNodeAs<TypeLoc>("typeLoc")) {
    collectUsedTypes(*TL, *Result.SourceManager);
  }
}

void UnusedIncludesCheck::onEndOfTranslationUnit() {
  // Types and macros referenced only inside macro bodies are invisible to
  // the AST (bodies are not parsed until expansion). Analyze macro bodies
  // directly so that e.g. a macro using uint32_t marks stdint.h as used.
  if (PP && AST) {
    analyzeMacroBodies(*PP);
  }

  // Build the include graph: for each include directive, an edge from the
  // includer file to the included file.
  llvm::DenseMap<const FileEntry *, llvm::SmallVector<const FileEntry *, 8>>
      IncludeGraph;
  for (const auto &Include : IncludedFiles) {
    if (Include.Includer && Include.File && Include.Includer != Include.File) {
      IncludeGraph[Include.Includer].push_back(Include.File);
    }
  }

  for (const auto &Include : IncludedFiles) {
    // Skip system headers if configured
    if (IgnoreSystemHeaders && Include.IsSystemHeader) {
      continue;
    }

    // An include directive in file A for header B is "used" when the
    // transitive closure of B's includes (including B itself) contains a
    // file that provides a symbol referenced from within A. A symbol may be
    // declared in a sub-header of B (e.g. uint32_t lives in
    // bits/stdint-uintn.h, included by stdint.h), so closure matching is
    // required rather than a direct FileEntry comparison.
    if (Include.Includer && Include.File) {
      llvm::DenseSet<const FileEntry *> Closure;
      llvm::SmallVector<const FileEntry *, 32> Worklist = {Include.File};
      while (!Worklist.empty()) {
        const FileEntry *F = Worklist.pop_back_val();
        if (!Closure.insert(F).second)
          continue;
        auto It = IncludeGraph.find(F);
        if (It != IncludeGraph.end()) {
          for (const FileEntry *Child : It->second)
            Worklist.push_back(Child);
        }
      }

      const auto &UsedInIncluder = UsedFilesByFile.find(Include.Includer);
      bool Used = false;
      if (UsedInIncluder != UsedFilesByFile.end()) {
        for (const FileEntry *F : Closure) {
          if (UsedInIncluder->second.count(F)) {
            Used = true;
            break;
          }
        }
      }
      if (Used)
        continue;
    }

    diag(Include.Loc, "unused include '%0'") << Include.FileName;
  }
}

void UnusedIncludesCheck::noteMacroUsed(const SourceLocation &UseLoc,
                                        const SourceLocation &DefLoc,
                                        const SourceManager &SM) {
  if (IgnoreMacros)
    return;
  if (!UseLoc.isValid() || !DefLoc.isValid())
    return;

  const FileEntry *DefFile = SM.getFileEntryForID(SM.getFileID(DefLoc));
  const FileEntry *UseFile = SM.getFileEntryForID(SM.getFileID(UseLoc));
  if (DefFile && UseFile && DefFile != UseFile) {
    UsedFilesByFile[UseFile].insert(DefFile);
  }
}

void UnusedIncludesCheck::noteSymbolUsed(const SourceLocation &UseLoc,
                                         const Decl *D,
                                         const SourceManager &SM) {
  if (!D || !UseLoc.isValid() || !D->getLocation().isValid())
    return;

  const FileEntry *DeclFile = SM.getFileEntryForID(SM.getFileID(D->getLocation()));
  const FileEntry *UseFile = SM.getFileEntryForID(SM.getFileID(UseLoc));
  if (DeclFile && UseFile && DeclFile != UseFile) {
    UsedFilesByFile[UseFile].insert(DeclFile);
  }
}

void UnusedIncludesCheck::analyzeMacroBodies(const Preprocessor &PP) {
  const SourceManager &SM = PP.getSourceManager();
  // Build a map from type name to the file declaring it, so identifier
  // tokens inside macro bodies can be resolved to their providing file.
  llvm::DenseMap<IdentifierInfo *, const FileEntry *> TypeFiles;
  for (const Decl *D : AST->getTranslationUnitDecl()->decls()) {
    const NamedDecl *ND = nullptr;
    if (const auto *TND = dyn_cast<TypedefNameDecl>(D)) {
      ND = TND;
    } else if (const auto *RD = dyn_cast<RecordDecl>(D)) {
      ND = RD;
    } else if (const auto *ED = dyn_cast<EnumDecl>(D)) {
      ND = ED;
    }
    if (!ND || !ND->getIdentifier())
      continue;
    const FileEntry *F = SM.getFileEntryForID(SM.getFileID(ND->getLocation()));
    if (F)
      TypeFiles[ND->getIdentifier()] = F;
  }

  // Scan every macro definition for identifier tokens that reference
  // another macro or a type; record those providing files against the file
  // that defines the macro.
  for (Preprocessor::macro_iterator It = PP.macro_begin(), End = PP.macro_end();
       It != End; ++It) {
    MacroDirective *MD = It->second.getLatest();
    if (!MD)
      continue;
    MacroInfo *MI = MD->getMacroInfo();
    if (!MI || !MI->getDefinitionLoc().isValid())
      continue;
    const FileEntry *MacroDefFile =
        SM.getFileEntryForID(SM.getFileID(MI->getDefinitionLoc()));
    if (!MacroDefFile)
      continue;

    for (const Token &Tok : MI->tokens()) {
      if (!Tok.is(tok::identifier))
        continue;
      IdentifierInfo *II = Tok.getIdentifierInfo();
      if (!II)
        continue;

      const FileEntry *ProvidedBy = nullptr;
      if (const MacroInfo *UsedMacro = PP.getMacroInfo(II)) {
        if (UsedMacro->getDefinitionLoc().isValid())
          ProvidedBy = SM.getFileEntryForID(
              SM.getFileID(UsedMacro->getDefinitionLoc()));
      } else {
        auto TypeIt = TypeFiles.find(II);
        if (TypeIt != TypeFiles.end())
          ProvidedBy = TypeIt->second;
      }
      if (ProvidedBy && ProvidedBy != MacroDefFile)
        UsedFilesByFile[MacroDefFile].insert(ProvidedBy);
    }
  }
}

void UnusedIncludesCheck::collectUsedSymbols(const Stmt *S,
                                             const SourceManager &SM) {
  if (!S)
    return;

  // Collect function calls
  if (const auto *CE = dyn_cast<CallExpr>(S)) {
    if (const FunctionDecl *FD = CE->getDirectCallee()) {
      noteSymbolUsed(CE->getBeginLoc(), FD, SM);
    }
  }

  // Collect DeclRefExpr (variable/type references)
  if (const auto *DRE = dyn_cast<DeclRefExpr>(S)) {
    if (const NamedDecl *ND = DRE->getDecl()) {
      noteSymbolUsed(DRE->getBeginLoc(), ND, SM);
    }
  }

  // Collect MemberExpr (struct member access)
  if (const auto *ME = dyn_cast<MemberExpr>(S)) {
    if (const NamedDecl *ND = ME->getMemberDecl()) {
      noteSymbolUsed(ME->getBeginLoc(), ND, SM);
    }
  }

  // Recursively process child statements
  for (const Stmt *Child : S->children()) {
    collectUsedSymbols(Child, SM);
  }
}

void UnusedIncludesCheck::collectUsedTypes(const TypeLoc &TL,
                                           const SourceManager &SM) {
  QualType T = TL.getType();
  if (T.isNull())
    return;

  // Walk through qualifiers, pointers, arrays and references to find
  // typedef/tag usage at any depth (bounded to avoid pathological types).
  for (unsigned Depth = 0; Depth < 8; ++Depth) {
    T = T.getUnqualifiedType();

    // Typedef usage (e.g. uint32_t, clogx_config_t)
    if (const auto *TT = T->getAs<TypedefType>()) {
      if (const TypedefNameDecl *TD = TT->getDecl()) {
        noteSymbolUsed(TL.getBeginLoc(), TD, SM);
      }
      return;
    }

    // Tag usage (struct/enum/union referenced)
    if (TagDecl *TD = T->getAsTagDecl()) {
      noteSymbolUsed(TL.getBeginLoc(), TD, SM);
      return;
    }

    // Step into common wrappers
    if (const auto *PT = T->getAs<PointerType>()) {
      T = PT->getPointeeType();
      continue;
    }
    if (const auto *AT = T->getAsArrayTypeUnsafe()) {
      T = AT->getElementType();
      continue;
    }
    if (const auto *RT = T->getAs<ReferenceType>()) {
      T = RT->getPointeeType();
      continue;
    }
    break;
  }
}

} // namespace clang::tidy::clogx

// Module to register the check
class UnusedIncludesModule : public clang::tidy::ClangTidyModule {
public:
  void addCheckFactories(
      clang::tidy::ClangTidyCheckFactories &CheckFactories) override {
    CheckFactories.registerCheck<clang::tidy::clogx::UnusedIncludesCheck>(
        "clogx-unused-includes");
  }
};

// Register the module
static clang::tidy::ClangTidyModuleRegistry::Add<UnusedIncludesModule>
    X("clogx-unused-includes-module",
      "Registers the clogx-unused-includes check");
