//===- ASTMergeAction.cpp - AST Merging Frontend Action----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2018 DVM System Group
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
//
// This file implements frontend action adapter which merges ASTs together.
//
//===----------------------------------------------------------------------===//

#include "tsar/Frontend/Clang/ASTMergeAction.h"
#include "tsar/Analysis/Clang/SourceLocationTraverse.h"
#include "tsar/Frontend/Clang/ASTImportInfo.h"
#include "tsar/Frontend/Clang/Passes.h"
#include "tsar/Patch/clang/AST/ASTImporter.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include <clang/Frontend/ASTUnit.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/ASTDiagnostic.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Sema/SemaDiagnostic.h>
#include <llvm/ADT/SmallPtrSet.h>

using namespace clang;
using namespace llvm;
using namespace tsar;

namespace clang {
/// \brief This callback tries to perform manual update of imported TypeLocs.
///
/// TODO (kaniandr@gmail.com): at this moment "trivial" type source info
/// is used for imported types. It means that TypeLocs have not a full
/// information about types. For example, size expression is not specified for
/// objects of ArrayTypeLoc class. We try to update TypeLocs in some cases.
/// This class should be removed when ASTImporter will be fixed.
class TypeLocCallback : public ast_matchers::MatchFinder::MatchCallback {
public:
  /// Creates callback.
  explicit TypeLocCallback() {}

  /// Updates type TypeLocs according to underlining types.
  void run(const ast_matchers::MatchFinder::MatchResult &Result) override {
    auto TL = Result.Nodes.getNodeAs<TypeLoc>("typeLoc");
    if (auto ATL = TL->getAs<ArrayTypeLoc>())
      if (!ATL.getSizeExpr())
         if (auto VAT = dyn_cast<VariableArrayType>(ATL.getTypePtr()))
           ATL.setSizeExpr(VAT->getSizeExpr());
  }
};

/// \brief This callback for ASTMatcher tries to perform manual import of
/// variable array types if clang::ASTImporter can not perform it itself.
///
/// TODO (kaniandr@gmail.com): VariableArrayType with null size expression is
/// not imported by ASTImporter (VisitVariableArrayType() in ASTImporter.cpp).
/// Such import is implemented hear to avoid patching of ASTImporter.cpp,
/// only ASTImporter.h is patched. The patch marks this class as friend for
/// ASTImporter to enable access to ASTImporter::ImportedTypes map.
/// VariableArrayType with null size expression occurs in ImplicitCastExpr in a
/// function call. This class should be removed when ASTImporter will be fixed.
class VariableArrayCallback : public ast_matchers::MatchFinder::MatchCallback {
public:
  /// Creates callback.
  explicit VariableArrayCallback(ASTImporter &Importer,
      DiagnosticsEngine &Diags) : mImporter(&Importer), mDiags(&Diags) {}

  /// Imports variable array type which is used in implicit cast expression.
  void run(const ast_matchers::MatchFinder::MatchResult &Result) override {
    auto VAT = Result.Nodes.getNodeAs<VariableArrayType>("vaType");
    if (VAT->getSizeExpr() || mImporter->ImportedTypes.count(VAT))
      return;
    auto To = VisitVariableArrayType(VAT);
    if (To.isNull())
      toDiag(*mDiags, mImporter->Import(VAT->getLBracketLoc()),
        diag::err_import);
    else
      toDiag(*mDiags, mImporter->Import(VAT->getLBracketLoc()),
        diag::warn_import_variable_array);
  }

private:
  /// Imports variable array type.
  QualType VisitVariableArrayType(const VariableArrayType *T) {
    QualType ToElementType = mImporter->Import(T->getElementType());
    // We try to import type manually if built-in clang::ASTImporter has been
    // unsuccessful.
    if (ToElementType.isNull() && !T->getElementType().isNull() &&
        isa<VariableArrayType>(T->getElementType().getTypePtr())) {
      ToElementType = VisitVariableArrayType(
        cast<VariableArrayType>(T->getElementType().getTypePtr()));
      ToElementType = mImporter->getToContext().getQualifiedType(
        ToElementType, T->getElementType().getLocalQualifiers());
    }
    if (ToElementType.isNull())
      return QualType();
    Expr *Size = mImporter->Import(T->getSizeExpr());
    if (!Size && T->getSizeExpr())
      return QualType();
    SourceRange Brackets = mImporter->Import(T->getBracketsRange());
    auto To = mImporter->getToContext().getVariableArrayType(
      ToElementType, Size, T->getSizeModifier(),
      T->getIndexTypeCVRQualifiers(), Brackets);
    mImporter->ImportedTypes[T] = To.getTypePtr();
    return To;
  }

  ASTImporter *mImporter;
  DiagnosticsEngine *mDiags;
};

/// \brief This callback for ASTMatcher try to perform manual import of case
/// statement.
///
/// TODO (kaniandr@gmail.com): ASTImporter does not import getSubStmt() for
/// a case statements. This callback performs such import.
/// This class should be removed when ASTImporter will be fixed.
class CaseStmtCallback : public ast_matchers::MatchFinder::MatchCallback {
public:
  /// Creates callback.
  explicit CaseStmtCallback(ASTImporter &Importer,
      DiagnosticsEngine &Diags) : mImporter(&Importer), mDiags(&Diags) {}

  /// Imports case statements.
  void run(const ast_matchers::MatchFinder::MatchResult &Result) override {
    auto FromCS = Result.Nodes.getNodeAs<CaseStmt>("case");
    auto ToCS = mImporter->Import(const_cast<CaseStmt *>(FromCS));
    if (!ToCS || !FromCS->getSubStmt())
      return;
    auto ToSubStmt =
      mImporter->Import(const_cast<Stmt *>(FromCS->getSubStmt()));
    if (!ToSubStmt)
      toDiag(*mDiags, ToCS->getLocStart(), diag::err_import);
    else
      toDiag(*mDiags, ToCS->getLocStart(), diag::warn_import_case);
    cast<CaseStmt>(ToCS)->setSubStmt(ToSubStmt);
  }

private:
  ASTImporter *mImporter;
  DiagnosticsEngine *mDiags;
};
}

namespace tsar {
/// This is implementation of ASTImporter for the general use in analyzer.
class GeneralImporter : public ASTImporter {
public:
  GeneralImporter(ASTContext &ToContext, FileManager &ToFileManager,
      ASTContext &FromContext, FileManager &FromFileManager,
      bool MinimalImport) :
    ASTImporter(ToContext, ToFileManager, FromContext, FromFileManager,
      MinimalImport) {}

  DeclarationName HandleNameConflict(DeclarationName Name, DeclContext *DC,
      unsigned IDNS, NamedDecl **Decls, unsigned NumDecls) override {
    for (unsigned I = 0; I < NumDecls; ++I) {
      if (mInternalFuncs.count(Decls[I])) {
        // Conflicts for internal functions from the current unit are ignored.
        // This conflicts occur because ASTImporter treats all names similar to
        // name of internal function as a conflict (even for function
        // definitions).
        mLastConflict = Decls[I];
        return Name;
      }
      ToDiag(Decls[I]->getLocation(), diag::err_redefinition_different_kind)
        << Name;
    }
    return DeclarationName();
  }

  Decl *Imported(Decl *From, Decl *To) override {
    To = ASTImporter::Imported(From, To);
    if (auto FromF = dyn_cast<FunctionDecl>(From)) {
      if (!FromF->hasExternalFormalLinkage()) {
        mInternalFuncs.insert(To);
        if (mLastConflict && isa<FunctionDecl>(mLastConflict)) {
          auto Recent = cast<FunctionDecl>(mLastConflict->getMostRecentDecl());
          cast<FunctionDecl>(To)->setPreviousDecl(Recent);
          mLastConflict = nullptr;
        }
      }
    }
    return To;
  }
private:
  SmallPtrSet<Decl *, 8> mInternalFuncs;
  Decl *mLastConflict = nullptr;
};

/// This is implementation of ASTImporter stores some information about
/// import process in an specified external storage.
class ExtendedImporter : public GeneralImporter {
public:
  ExtendedImporter(ASTContext &ToContext, FileManager &ToFileManager,
      ASTContext &FromContext, FileManager &FromFileManager,
      bool MinimalImport, ASTImportInfo &Out) :
    GeneralImporter(ToContext, ToFileManager, FromContext, FromFileManager,
      MinimalImport), mOut(Out) {}

  Decl *Imported(Decl *From, Decl *To) override {
    To = GeneralImporter::Imported(From, To);
    SmallVector<SourceLocation, 5> FromLocs, ToLocs;
    traverseSourceLocation(From,
      [&FromLocs, this](SourceLocation Loc) {FromLocs.push_back(Import(Loc));});
    traverseSourceLocation(To,
      [&ToLocs](SourceLocation Loc) { ToLocs.push_back(Loc); });
    assert(FromLocs.size() == ToLocs.size() &&
      "Different lists of known locations for the source and the result of import!");
    auto &RedeclLocs = mOut.RedeclLocs.try_emplace(To, ToLocs).first->second;
    RedeclLocs.push_back(FromLocs);
    return To;
  }
private:
  ASTImportInfo &mOut;
};

std::pair<Decl *, Decl *> ASTMergeAction::ImportVarDecl(VarDecl *FromV,
    ASTImporter &Importer, std::vector<VarDecl *> &TentativeDefinitions) const {
  auto ToD = Importer.Import(FromV);
  if (!ToD)
    return std::make_pair(FromV, nullptr);
  auto ToV = cast<VarDecl>(ToD);
  switch (ToV->isThisDeclarationADefinition()) {
  case VarDecl::TentativeDefinition:
    TentativeDefinitions.push_back(ToV); break;
  case VarDecl::DeclarationOnly:
    // Let us check that the result of previously imported declaration
    // which is similar to FromV is used but some data is lost.
    // Let us consider an example.
    //   extern int X; // (1)
    //   int X; // (2)
    // At first, (1) will be imported and ToV will be constructed.
    // So result of import of (2) is this ToV (new VarDecl will not be
    // constructed). However the storage class of ToV is extern and
    // in LLVM IR it will be external location without definition. To
    // solve this problem additional VarDecl can be created which has
    // the same storage class as imported declaration (2).
    if (FromV->isThisDeclarationADefinition() ==
        VarDecl::TentativeDefinition) {
      VarDecl *ToTentative = VarDecl::Create(
        ToV->getASTContext(), ToV->getDeclContext(),
        Importer.Import(FromV->getInnerLocStart()),
        Importer.Import(FromV->getLocation()),
        ToV->getDeclName().getAsIdentifierInfo(),
        ToV->getType(), ToV->getTypeSourceInfo(),
        FromV->getStorageClass());
      ToTentative->setQualifierInfo(ToV->getQualifierLoc());
      ToTentative->setAccess(ToV->getAccess());
      ToTentative->setLexicalDeclContext(ToV->getLexicalDeclContext());
      ToV->getLexicalDeclContext()->addDeclInternal(ToTentative);
      if (!ToV->isFileVarDecl() && ToV->isUsed())
        ToTentative->setIsUsed();
      TentativeDefinitions.push_back(ToTentative);
      ToTentative->setPreviousDecl(ToV);
      ToV = ToTentative;
    }
    break;
  }
  return std::make_pair(FromV, ToV);
}

std::pair<Decl *, Decl *> ASTMergeAction::ImportFunctionDecl(
    FunctionDecl *FromF, ASTImporter &Importer) const {
  // It is not safe to import prototype. In this case parameters from
  // prototype will be imported but body will be imported from definition.
  // This leads to loss of information about parameters in the body.
  // Parameters in the definition and prototype does not linked together.
  const FunctionDecl *FuncWithBody = nullptr;
  FromF->hasBody(FuncWithBody);
  if (FromF == FuncWithBody) {
    for (auto Redecl : FromF->redecls()) {
      if (Redecl == FromF)
        continue;
      if (auto ToRedecl = Importer.GetAlreadyImportedOrNull(Redecl)) {
        cast<FunctionDecl>(ToRedecl)->setBody(nullptr);
      }
    }
  }
  auto ToF = Importer.Import(FromF);
  if (ToF && FromF != FuncWithBody)
    cast<FunctionDecl>(ToF)->setBody(nullptr);
  return std::make_pair(FromF, ToF);
}


void ASTMergeAction::PrepareToImport(ASTUnit &Unit,
    DiagnosticsEngine &Diags, ASTImporter &Importer) const {
  ast_matchers::MatchFinder Finder;
  VariableArrayCallback VAC(Importer, Diags);
  Finder.addMatcher(
    ast_matchers::implicitCastExpr(
      ast_matchers::hasImplicitDestinationType(
        ast_matchers::pointsTo(
          ast_matchers::variableArrayType().bind("vaType")))), &VAC);
  CaseStmtCallback CSC(Importer, Diags);
  Finder.addMatcher(ast_matchers::caseStmt().bind("case"), &CSC);
  Finder.matchAST(Unit.getASTContext());
}

void ASTMergeAction::FinalizeImport(clang::ASTContext &ToContext,
    clang::DiagnosticsEngine &Diags) const {
  ast_matchers::MatchFinder Finder;
  TypeLocCallback TLC;
  Finder.addMatcher(ast_matchers::typeLoc().bind("typeLoc"), &TLC);
  Finder.matchAST(ToContext);
}

ASTImporter * ASTMergeAction::newImporter(
    ASTContext &ToContext, FileManager &ToFileManager,
    ASTContext &FromContext, FileManager &FromFileManager, bool MinimalImport) {
  return new GeneralImporter(ToContext, ToFileManager,
    FromContext, FromFileManager, MinimalImport);
}

ASTImporter * ASTMergeActionWithInfo::newImporter(
    ASTContext &ToContext, FileManager &ToFileManager,
    ASTContext &FromContext, FileManager &FromFileManager, bool MinimalImport) {
  mImportInfo->WasImport = true;
  return new ExtendedImporter(ToContext, ToFileManager,
    FromContext, FromFileManager, MinimalImport, *mImportInfo);
}

void ASTMergeAction::ExecuteAction() {
  CompilerInstance &CI = getCompilerInstance();
  CI.getDiagnostics().getClient()->BeginSourceFile(
    CI.getASTContext().getLangOpts());
  CI.getDiagnostics().SetArgToStringFn(&FormatASTNodeDiagnosticArgument,
    &CI.getASTContext());
  IntrusiveRefCntPtr<DiagnosticIDs>
    DiagIDs(CI.getDiagnostics().getDiagnosticIDs());
  std::vector<VarDecl *> TentativeDefinitions;
  // TODO (kaniandr@gmail.com): This is a hack which is used to load locations
  // from external storage (decls_begin() performs implicit load).
  // View the following explanation. Some locations from external storage will
  // not be find if localUncachedLookup() will be used. So some conflicts will
  // not be recognized. Let us consider un example:
  // - file bar.c contains `static int f() { return 0;}`
  // - file foo.c contains `int f`;
  // - command is `tsar -m bar.c foo.c -emit-llvm`
  // In this case `int f` will not be find and function 'f()' will be
  // successfully loaded. However this leads to assertion fail when deferred
  // locations f will be emitted by CodeGenModule::EmitDeferred().
  CI.getASTContext().getTranslationUnitDecl()->decls_begin();
  for (unsigned I = 0, N = mASTFiles.size(); I != N; ++I) {
    IntrusiveRefCntPtr<DiagnosticsEngine>
      Diags(new DiagnosticsEngine(DiagIDs, &CI.getDiagnosticOpts(),
        new ForwardingDiagnosticConsumer(
          *CI.getDiagnostics().getClient()), /*ShouldOwnClient=*/true));
    std::unique_ptr<ASTUnit> Unit =
      ASTUnit::LoadFromASTFile(mASTFiles[I], CI.getPCHContainerReader(),
        ASTUnit::LoadEverything, Diags, CI.getFileSystemOpts(), false);
    if (!Unit)
      continue;
    std::unique_ptr<ASTImporter> Importer(
      newImporter(CI.getASTContext(), CI.getFileManager(),
        Unit->getASTContext(), Unit->getFileManager(), /*MinimalImport=*/false));
    TranslationUnitDecl *TU = Unit->getASTContext().getTranslationUnitDecl();
    PrepareToImport(*Unit, CI.getDiagnostics(), *Importer);
    for (auto *D : TU->decls()) {
      // Don't re-import __va_list_tag, __builtin_va_list.
      if (const auto *ND = dyn_cast<NamedDecl>(D))
        if (IdentifierInfo *II = ND->getIdentifier())
          if (II->isStr("__va_list_tag") || II->isStr("__builtin_va_list"))
            continue;
      Decl *ToD = nullptr;
      if (auto *F = dyn_cast<FunctionDecl>(D)) {
        std::tie(D, ToD) = ImportFunctionDecl(F, *Importer);
      } else if (auto *V = dyn_cast<VarDecl>(D)) {
        std::tie(D, ToD) = ImportVarDecl(V, *Importer, TentativeDefinitions);
      } else {
        /// TODO (kaniandr@gmail.com): This is a hack which is necessary due to
        /// implementation of import of `TypeSourceInfo` is unfinished.
        /// Only basic info is supported. For example, attributes is ignored.
        /// This means that `typedef int INT __attribute__((__mode__(__HI__)));`
        /// will be imported as int instead of short. So we replace underlying
        /// type of source `D` with `short` before import.
        /// For details, see `ASTImporter::Import(TypeSourceInfo *FromTSI)`.
        if (auto Typedef = dyn_cast<TypedefNameDecl>(D)) {
          assert(Typedef->getTypeSourceInfo() &&
            "TypeSourceInfo must not be null for a named typedef");
          if (Typedef->getTypeSourceInfo()->getType()
              != Typedef->getUnderlyingType()) {
            auto Stash = Typedef->getTypeSourceInfo()->getType();
            Typedef->getTypeSourceInfo()->overrideType(
              Typedef->getUnderlyingType());
            toDiag(CI.getDiagnostics(), Importer->Import(D->getLocation()),
              diag::warn_import_typedef);
          }
        }
        ToD = Importer->Import(D);
      }
      if (ToD) {
        DeclGroupRef DGR(ToD);
        CI.getASTConsumer().HandleTopLevelDecl(DGR);
        continue;
      }
      // Report errors.
      if (auto *ND = dyn_cast<NamedDecl>(D)) {
        if (auto ImportedName = Importer->Import(ND->getDeclName())) {
          toDiag(CI.getDiagnostics(), Importer->Import(D->getLocation()),
            diag::err_import_named) << ImportedName;
          continue;
        }
      }
      toDiag(CI.getDiagnostics(), Importer->Import(D->getLocation()),
        diag::err_import);
    }
  }
  // Note LLVM IR will not be generated for tentative definitions
  // without call of ASTConsumer::CompleteTentativeDefinition() function.
  SmallPtrSet<VarDecl *, 32> Seen;
  for (auto *V : TentativeDefinitions) {
    VarDecl *VD = V->getActingDefinition();
    if (!VD || VD->isInvalidDecl() || !Seen.insert(VD).second)
      continue;
    CI.getASTConsumer().CompleteTentativeDefinition(VD);
  }
  FinalizeImport(CI.getASTContext(), CI.getDiagnostics());
  WrapperFrontendAction::ExecuteAction();
  CI.getDiagnostics().getClient()->EndSourceFile();
}

bool ASTMergeAction::BeginSourceFileAction(CompilerInstance &CI) {
  /// TODO(kaniandr@gmail.com): This is a hack. It is necessary to set ASTUnit
  /// for an action to be wrapped, but WrapperFrontendAction is set it to null.
  /// So we believe that it is safe to set it after BeginSourceFileAction() call
  /// for the wrapped action. Note, that this is protected member, so only
  /// WrapperFrontendAction can access it.
  /// Note that WrapperFrontendAction will unset ASTUnit for both (current and
  /// wrapped actions), so it is necessary to save ASTUnit.
  auto ASTUnit = takeCurrentASTUnit();
  auto Ret = WrapperFrontendAction::BeginSourceFileAction(CI);
  getWrappedAction().setCurrentInput(getCurrentInput(), std::move(ASTUnit));
  return Ret;
}

ASTMergeAction::ASTMergeAction(
    std::unique_ptr<clang::FrontendAction> WrappedAction,
    clang::ArrayRef<std::string> ASTFiles) :
  PublicWrapperFrontendAction(WrappedAction.release()),
  mASTFiles(ASTFiles.begin(), ASTFiles.end()) {}
}

INITIALIZE_PASS(ImmutableASTImportInfoPass, "clang-import-info",
  "AST Import Immutable Information (Clang)", false, false)

char ImmutableASTImportInfoPass::ID = 0;

ImmutablePass * llvm::createImmutableASTImportInfoPass(
    const ASTImportInfo &Info) {
  return new ImmutableASTImportInfoPass(&Info);
}
