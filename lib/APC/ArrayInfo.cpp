//===---- ArrayInfo.cpp ----- APC Array Collector ---------------*- C++ -*-===//
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
// This file implements per-function pass to obtain general information about
// arrays which are accessed in a function. Not all arrays are collected, if
// some information of an array have been lost then it will be ignored.
//
//===----------------------------------------------------------------------===//

#include "AstWrapperImpl.h"
#include "tsar/Analysis/Memory/Delinearization.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/APC/APCContext.h"
#include "tsar/APC/Passes.h"
#include "tsar/Core/Query.h"
#include "tsar/Support/Diagnostic.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/NumericUtils.h"
#include "tsar/Support/Tags.h"
#include <apc/Distribution/Array.h>
#include <apc/ParallelizationRegions/ParRegions.h>
#include <apc/Utils/types.h>
#include <bcl/utility.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Pass.h>
#include <llvm/Support/Format.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "apc-array-info"

using namespace llvm;
using namespace tsar;

namespace {
class APCArrayInfoPass : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  APCArrayInfoPass() : FunctionPass(ID) {
    initializeAPCArrayInfoPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void print(raw_ostream &OS, const Module *M) const override;
  void releaseMemory() override { mArrays.clear(); }

private:
  std::vector<apc::Array *> mArrays;
  bool mMultipleLaunch = false;
};
}

char APCArrayInfoPass::ID = 0;

INITIALIZE_PASS_IN_GROUP_BEGIN(APCArrayInfoPass, "apc-array-info",
  "Array Collector (APC)", true, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())
  INITIALIZE_PASS_DEPENDENCY(DelinearizationPass)
  INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(APCContextWrapper)
  INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
  INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PASS_IN_GROUP_END(APCArrayInfoPass, "apc-array-info",
  "Array Collector (APC)", true, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())

FunctionPass * llvm::createAPCArrayInfoPass() { return new APCArrayInfoPass; }

void APCArrayInfoPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<DelinearizationPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<APCContextWrapper>();
  AU.addRequired<EstimateMemoryPass>();
  AU.addRequired<DIEstimateMemoryPass>();
  AU.setPreservesAll();
}

bool APCArrayInfoPass::runOnFunction(Function &F) {
  releaseMemory();
  auto &DL = F.getParent()->getDataLayout();
  auto &DI = getAnalysis<DelinearizationPass>().getDelinearizeInfo();
  auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  auto &AT = getAnalysis<EstimateMemoryPass>().getAliasTree();
  auto &DIAT = getAnalysis<DIEstimateMemoryPass>().getAliasTree();
  auto &APCCtx = getAnalysis<APCContextWrapper>().get();
  for (auto *A: DI.getArrays()) {
    if (!A->isDelinearized() || !A->hasMetadata())
      continue;
    SmallVector<DIMemoryLocation, 4> DILocs;
    // TODO (kaniandr@gmail.com): add processing of array-members of structures.
    // Note, that delinearization of such array-member should be implemented
    // first. We can use GEP to determine which member of a structure
    // is accessed.
    auto DILoc = findMetadata(A->getBase(), DILocs, &DT,
      A->isAddressOfVariable() ? MDSearch::AddressOfVariable : MDSearch::Any);
    assert(DILoc && DILoc->isValid() &&
      "Metadata must be available for an array!");
    auto DIElementTy = arrayElementDIType(DILoc->Var->getType());
    if (!DIElementTy)
      continue;
    auto DeclLoc = std::make_pair(DILoc->Var->getLine(), 0);
    if (DILoc->Loc)
      DeclLoc = std::make_pair(DILoc->Loc->getLine(), DILoc->Loc->getColumn());
    auto Filename = (DILoc->Var->getFilename().empty() ?
      StringRef(F.getParent()->getSourceFileName()) : DILoc->Var->getFilename());
    auto DeclScope = std::make_pair(Distribution::l_COMMON, std::string(""));
    if (auto DILocalVar = dyn_cast<DILocalVariable>(DILoc->Var)) {
      if (DILocalVar->isParameter())
        DeclScope.first = Distribution::l_PARAMETER;
      else
        DeclScope.first = Distribution::l_LOCAL;
      DeclScope.second =  F.getName();
    }
    // Unique name is '<file>:line:column:@<function>%<variable>.<member>'.
    auto UniqueName =
      (Filename + ":" + Twine(DeclLoc.first) + ":" + Twine(DeclLoc.second) +
        "@" + F.getName() + "%" + DILoc->Var->getName()).str();
    std::decay<
      decltype(std::declval<apc::Array>().GetDeclInfo())>
        ::type::value_type::second_type ShrinkedDeclLoc;
    auto getDbgLoc = [&F, &DILoc, &DeclLoc]() {
      DebugLoc DbgLoc(DILoc->Loc);
      if (!DbgLoc && F.getSubprogram()) {
        DbgLoc = DILocation::get(
          F.getContext(), DeclLoc.first, DeclLoc.second, F.getSubprogram());
      }
      return DbgLoc;
    };
    if (!bcl::shrinkPair(DeclLoc.first, DeclLoc.second, ShrinkedDeclLoc))
      emitUnableShrink(F.getContext(), F, getDbgLoc(), DS_Warning);
    // TODO (kaniandr@gmail.com): what should we do in case of multiple
    // allocation of the same array. There are different memory locations
    // and different MDNodes for such arrays. Howevere, declaration points
    // for these locations are identical.
    auto *EM = AT.find(MemoryLocation(A->getBase(), 0));
    assert(EM && "Estimate memory must be presented in alias tree!");
    auto RawDIM =
      getRawDIMemoryIfExists(*EM->getTopLevelParent(), F.getContext(), DL, DT);
    assert(RawDIM && "Unknown raw memory!");
    auto APCSymbol = new apc::Symbol(*DILoc);
    APCCtx.addSymbol(APCSymbol);
    auto APCArray = new apc::Array(UniqueName, DILoc->Var->getName(),
      A->getNumberOfDims(), APCCtx.getNumberOfArrays(),
      Filename, ShrinkedDeclLoc, std::move(DeclScope), APCSymbol,
      { APCCtx.getDefaultRegion().GetName() }, getSize(DIElementTy));
    if (!APCCtx.addArray(RawDIM, APCArray)) {
      // This pass may be executed in analysis mode. It depends on -print-only
      // and -print-step options. In case of parallelization pass manager must
      // invokes this pass only once for each function.
      delete APCArray;
      mArrays.push_back(APCCtx.findArray(RawDIM));
      mMultipleLaunch = true;
      continue;
    }
    mArrays.push_back(APCArray);
    auto Sizes = APCArray->GetSizes();
    for (std::size_t I = 0, EI = A->getNumberOfDims(); I < EI; ++I) {
      APCArray->SetMappedDim(I);
      Sizes[I].first = 0;
      if (auto Size = dyn_cast<SCEVConstant>(A->getDimSize(I))) {
        if (!castAPInt(Size->getAPInt(), false, Sizes[I].second))
          emitTypeOverflow(F.getContext(), F, getDbgLoc(),
            "unable to represent upper bound of " + Twine(I+1) + "dimension",
            DS_Warning);
      }
    }
    APCArray->SetSizes(Sizes);
  }
  return false;
}

void APCArrayInfoPass::print(raw_ostream &OS, const Module *M) const {
  if (mMultipleLaunch)
    OS << "warning: possible multiple launches of the pass for the same "
          "function: print merged results\n";
  for (auto *A : mArrays) {
    OS << format("%s [short=%s, unique=%s, id=%d]\n", A->GetName().c_str(),
      A->GetShortName().c_str(), A->GetArrayUniqKey().c_str(), A->GetId());
    auto ScopInfo =  A->GetLocation();
    switch (ScopInfo.first) {
    case Distribution::l_LOCAL:
      OS << "  local variable in '" << ScopInfo.second << "'\n"; break;
    case Distribution::l_PARAMETER:
      OS << "  argument of '" << ScopInfo.second << "'\n"; break;
    case Distribution::l_COMMON:
      OS << "  global variable\n"; break;
    default:
      llvm_unreachable("Unsupported scope!");
    }
    auto DeclInfo = A->GetDeclInfo();
    OS << "  declaration:\n";
    for (auto &Info : DeclInfo) {
      std::pair<unsigned, unsigned> DeclLoc;
      bcl::restoreShrinkedPair(Info.second, DeclLoc.first, DeclLoc.second);
      OS << "    "
         << Info.first << ":" << DeclLoc.first << ":" << DeclLoc.second << "\n";
    }
    OS << "  size of element: " << A->GetTypeSize() << "\n";
    OS << "  number of dimensions: " << A->GetDimSize() << "\n";
    auto &DimSizes = A->GetSizes();
    for (std::size_t I = 0, EI = A->GetDimSize(); I < EI; ++I) {
      OS << "    " << I << ": size is ";
      if (DimSizes[I].second < 0)
        OS << "unknown";
      else
        OS << format("[%d, %d)", DimSizes[I].first, DimSizes[I].second);
      OS << ", may" << (A->IsDimMapped(I) ? "" : " not") << " be mapped\n";
    }
    OS << "  parallel regions: ";
    for (auto &PR : A->GetRegionsName())
      OS << PR << " ";
    OS << "\n";
  }
}

