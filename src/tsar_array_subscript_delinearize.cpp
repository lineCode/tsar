#include "tsar_array_subscript_delinearize.h"
#include "DelinearizeJSON.h"
#include "tsar_pass.h"
#include "tsar_transformation.h"
#include "tsar_utility.h"
#include "tsar_query.h"
#include "MemoryAccessUtils.h"

#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/PassAnalysisSupport.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/ADT/SmallVector.h>
#include "llvm/Analysis/InstructionSimplify.h"
#include <llvm/IR/DEBUGInfoMetadata.h>
#include <llvm/ADT/Sequence.h>
#include <llvm/IR/Type.h>
#include <bcl/Json.h>

#include <vector>
#include <utility>
#include <memory>
#include <string>
#include <cmath>

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "delinearize"

char ArraySubscriptDelinearizePass::ID = 0;
INITIALIZE_PASS_IN_GROUP_BEGIN(ArraySubscriptDelinearizePass, "delinearize",
  "Array Subscript Delinearize", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_IN_GROUP_END(ArraySubscriptDelinearizePass, "delinearize",
  "Array Subscript Delinearize", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())

STATISTIC(NumDelinearizedSubscripts, "Number of delinearized subscripts");

static cl::opt<unsigned> MaxSCEVCompareDepth(
  "scalar-evolution-max-scev-compare-depth-local-copy", cl::Hidden,
  cl::desc("Maximum depth of recursive SCEV complexity comparisons"),
  cl::init(32));

static cl::opt<unsigned> MaxValueCompareDepth(
  "scalar-evolution-max-value-compare-depth-local-copy", cl::Hidden,
  cl::desc("Maximum depth of recursive value complexity comparisons"),
  cl::init(2));

namespace {
std::pair<const SCEV *, const SCEV *> findCoefficientsInSCEVMulExpr(const SCEVMulExpr *MulExpr,
  ScalarEvolution &SE) {
  assert(MulExpr && "MulExpr must not be null");
  SmallVector<const SCEV*, 2> AMultipliers, BMultipliers;
  bool hasAddRec = false;

  for (int i = 0; i < MulExpr->getNumOperands(); ++i) {
    auto *Op = MulExpr->getOperand(i);
    switch (Op->getSCEVType()) {
    case scTruncate:
    case scZeroExtend:
    case scSignExtend: {
      auto *InnerOp = cast<SCEVCastExpr>(Op)->getOperand();
      if (auto *AddRec = dyn_cast<SCEVAddRecExpr>(InnerOp)) {
        hasAddRec = true;

        auto *AddRecStepRecurrence = AddRec->getStepRecurrence(SE);
        auto *AddRecStart = AddRec->getStart();

        if (Op->getSCEVType() == scTruncate) {
          AMultipliers.push_back(SE.getTruncateExpr(AddRecStepRecurrence, Op->getType()));
          BMultipliers.push_back(SE.getTruncateExpr(AddRecStart, Op->getType()));
        }
        if (Op->getSCEVType() == scSignExtend) {
          AMultipliers.push_back(SE.getSignExtendExpr(AddRecStepRecurrence, Op->getType()));
          BMultipliers.push_back(SE.getSignExtendExpr(AddRecStart, Op->getType()));
        }
        if (Op->getSCEVType() == scZeroExtend) {
          AMultipliers.push_back(SE.getZeroExtendExpr(AddRecStepRecurrence, Op->getType()));
          BMultipliers.push_back(SE.getZeroExtendExpr(AddRecStart, Op->getType()));
        }
      } else {
        AMultipliers.push_back(Op);
        BMultipliers.push_back(Op);
      }
      break;
    }
    case scAddRecExpr: {
      hasAddRec = true;

      auto *AddRec = cast<SCEVAddRecExpr>(Op);

      auto *AddRecStepRecurrence = AddRec->getStepRecurrence(SE);
      auto *AddRecStart = AddRec->getStart();

      AMultipliers.push_back(AddRecStepRecurrence);
      BMultipliers.push_back(AddRecStart);

      break;
    }
    default: {
      AMultipliers.push_back(Op);
      BMultipliers.push_back(Op);
      break;
    }
    }

  }
  if (!hasAddRec) {
    return std::make_pair(SE.getConstant(APInt(sizeof(int), 0, true)), MulExpr);
  }
  auto *ACoefficient = SE.getMulExpr(AMultipliers);
  auto *BCoefficient = SE.getMulExpr(BMultipliers);
  return std::make_pair(ACoefficient, BCoefficient);
}
}

namespace tsar {
std::pair<const SCEV *, const SCEV *> findCoefficientsInSCEV(const SCEV *Expr, ScalarEvolution &SE) {
  assert(Expr && "Expression must not be null");
  switch (Expr->getSCEVType()) {
  case scTruncate:
  case scZeroExtend:
  case scSignExtend: {
    return findCoefficientsInSCEV(cast<SCEVCastExpr>(Expr)->getOperand(), SE);
  }
  case scAddRecExpr: {
    auto *AddRec = cast<SCEVAddRecExpr>(Expr);

    auto *AddRecStepRecurrence = AddRec->getStepRecurrence(SE);
    if (auto *CastExpr = dyn_cast<SCEVCastExpr>(AddRecStepRecurrence)) {
      AddRecStepRecurrence = CastExpr->getOperand();
    }

    auto *AddRecStart = AddRec->getStart();
    if (auto *CastExpr = dyn_cast<SCEVCastExpr>(AddRecStart)) {
      AddRecStart = CastExpr->getOperand();
    }

    return std::make_pair(AddRecStepRecurrence, AddRecStart);
  }
  case scAddExpr:
  case scConstant:
  case scUnknown: {
    return std::make_pair(SE.getConstant(APInt(sizeof(int), 0, true)), Expr);
  }
  case scMulExpr: {
    return findCoefficientsInSCEVMulExpr(cast<SCEVMulExpr>(Expr), SE);
  }
  default: {
    return std::make_pair(SE.getConstant(APInt(sizeof(int), 0, true)),
      SE.getConstant(APInt(sizeof(int), 0, true)));
  }
  }
}
}

const Array * DelinearizeInfo::findArray(const Value *BasePtr) const {
  auto ResultItr = mArrays.find_as(const_cast<Value *>(BasePtr));
  if (ResultItr != mArrays.end())
    return &(*ResultItr);
  return nullptr;
}

std::pair<const Array *, const Array::Element *>
DelinearizeInfo::findElement(const Value *ElementPtr) const {
  auto Itr = mElements.find(ElementPtr);
  if (Itr != mElements.end()) {
    auto *TargetArray = Itr->getArray();
    auto *TargetElement = TargetArray->getElement(Itr->getElementIdx());
    return std::make_pair(TargetArray, TargetElement);
  }
  return std::make_pair(nullptr, nullptr);
}

void DelinearizeInfo::fillElementsMap() {
  mElements.clear();
  for (auto &ArrayEntry : mArrays) {
    int Idx = 0;
    for (auto Itr = ArrayEntry.begin(), ItrE = ArrayEntry.end();
         Itr != ItrE; ++Itr, ++Idx)
      mElements.try_emplace(Itr->Ptr, &ArrayEntry, Idx);
  }
}

namespace {
static int
CompareValueComplexity(SmallSet<std::pair<Value *, Value *>, 8> &EqCache,
  const LoopInfo *const LI, Value *LV, Value *RV,
  unsigned Depth) {
  if (Depth > MaxValueCompareDepth || EqCache.count({ LV, RV }))
    return 0;

  // Order pointer values after integer values. This helps SCEVExpander form
  // GEPs.
  bool LIsPointer = LV->getType()->isPointerTy(),
    RIsPointer = RV->getType()->isPointerTy();
  if (LIsPointer != RIsPointer)
    return (int)LIsPointer - (int)RIsPointer;

  // Compare getValueID values.
  unsigned LID = LV->getValueID(), RID = RV->getValueID();
  if (LID != RID)
    return (int)LID - (int)RID;

  // Sort arguments by their position.
  if (const auto *LA = dyn_cast<Argument>(LV)) {
    const auto *RA = cast<Argument>(RV);
    unsigned LArgNo = LA->getArgNo(), RArgNo = RA->getArgNo();
    return (int)LArgNo - (int)RArgNo;
  }

  if (const auto *LGV = dyn_cast<GlobalValue>(LV)) {
    const auto *RGV = cast<GlobalValue>(RV);

    const auto IsGVNameSemantic = [&](const GlobalValue *GV) {
      auto LT = GV->getLinkage();
      return !(GlobalValue::isPrivateLinkage(LT) ||
        GlobalValue::isInternalLinkage(LT));
    };

    // Use the names to distinguish the two values, but only if the
    // names are semantically important.
    if (IsGVNameSemantic(LGV) && IsGVNameSemantic(RGV))
      return LGV->getName().compare(RGV->getName());
  }

  // For instructions, compare their loop depth, and their operand count.  This
  // is pretty loose.
  if (const auto *LInst = dyn_cast<Instruction>(LV)) {
    const auto *RInst = cast<Instruction>(RV);

    // Compare loop depths.
    const BasicBlock *LParent = LInst->getParent(),
      *RParent = RInst->getParent();
    if (LParent != RParent) {
      unsigned LDepth = LI->getLoopDepth(LParent),
        RDepth = LI->getLoopDepth(RParent);
      if (LDepth != RDepth)
        return (int)LDepth - (int)RDepth;
    }

    // Compare the number of operands.
    unsigned LNumOps = LInst->getNumOperands(),
      RNumOps = RInst->getNumOperands();
    if (LNumOps != RNumOps)
      return (int)LNumOps - (int)RNumOps;

    for (unsigned Idx : seq(0u, LNumOps)) {
      int Result =
        CompareValueComplexity(EqCache, LI, LInst->getOperand(Idx),
          RInst->getOperand(Idx), Depth + 1);
      if (Result != 0)
        return Result;
    }
  }

  EqCache.insert({ LV, RV });
  return 0;
}

static int CompareSCEVComplexity(
  SmallSet<std::pair<const SCEV *, const SCEV *>, 8> &EqCacheSCEV,
  const LoopInfo *const LI, const SCEV *LHS, const SCEV *RHS,
  unsigned Depth = 0) {
  // Fast-path: SCEVs are uniqued so we can do a quick equality check.
  if (LHS == RHS)
    return 0;

  // Primarily, sort the SCEVs by their getSCEVType().
  unsigned LType = LHS->getSCEVType(), RType = RHS->getSCEVType();
  if (LType != RType)
    return (int)LType - (int)RType;

  if (Depth > MaxSCEVCompareDepth || EqCacheSCEV.count({ LHS, RHS }))
    return 0;
  // Aside from the getSCEVType() ordering, the particular ordering
  // isn't very important except that it's beneficial to be consistent,
  // so that (a + b) and (b + a) don't end up as different expressions.
  switch (static_cast<SCEVTypes>(LType)) {
  case scUnknown: {
    const SCEVUnknown *LU = cast<SCEVUnknown>(LHS);
    const SCEVUnknown *RU = cast<SCEVUnknown>(RHS);

    SmallSet<std::pair<Value *, Value *>, 8> EqCache;
    int X = CompareValueComplexity(EqCache, LI, LU->getValue(), RU->getValue(),
      Depth + 1);
    if (X == 0)
      EqCacheSCEV.insert({ LHS, RHS });
    return X;
  }

  case scConstant: {
    const SCEVConstant *LC = cast<SCEVConstant>(LHS);
    const SCEVConstant *RC = cast<SCEVConstant>(RHS);

    // Compare constant values.
    const APInt &LA = LC->getAPInt();
    const APInt &RA = RC->getAPInt();
    unsigned LBitWidth = LA.getBitWidth(), RBitWidth = RA.getBitWidth();
    if (LBitWidth != RBitWidth)
      return (int)LBitWidth - (int)RBitWidth;
    return LA.ult(RA) ? -1 : 1;
  }

  case scAddRecExpr: {
    const SCEVAddRecExpr *LA = cast<SCEVAddRecExpr>(LHS);
    const SCEVAddRecExpr *RA = cast<SCEVAddRecExpr>(RHS);

    // Compare addrec loop depths.
    const Loop *LLoop = LA->getLoop(), *RLoop = RA->getLoop();
    if (LLoop != RLoop) {
      unsigned LDepth = LLoop->getLoopDepth(), RDepth = RLoop->getLoopDepth();
      if (LDepth != RDepth)
        return (int)LDepth - (int)RDepth;
    }

    // Addrec complexity grows with operand count.
    size_t LNumOps = LA->getNumOperands(), RNumOps = RA->getNumOperands();
    if (LNumOps != RNumOps)
      return (int)LNumOps - (int)RNumOps;

    // Lexicographically compare.
    for (unsigned i = 0; i != LNumOps; ++i) {
      int X = CompareSCEVComplexity(EqCacheSCEV, LI, LA->getOperand(i),
        RA->getOperand(i), Depth + 1);
      if (X != 0)
        return X;
    }
    EqCacheSCEV.insert({ LHS, RHS });
    return 0;
  }

  case scAddExpr:
  case scMulExpr:
  case scSMaxExpr:
  case scUMaxExpr: {
    const SCEVNAryExpr *LC = cast<SCEVNAryExpr>(LHS);
    const SCEVNAryExpr *RC = cast<SCEVNAryExpr>(RHS);

    // Lexicographically compare n-ary expressions.
    size_t LNumOps = LC->getNumOperands(), RNumOps = RC->getNumOperands();
    if (LNumOps != RNumOps)
      return (int)LNumOps - (int)RNumOps;

    for (unsigned i = 0; i != LNumOps; ++i) {
      if (i >= RNumOps)
        return 1;
      int X = CompareSCEVComplexity(EqCacheSCEV, LI, LC->getOperand(i),
        RC->getOperand(i), Depth + 1);
      if (X != 0)
        return X;
    }
    EqCacheSCEV.insert({ LHS, RHS });
    return 0;
  }

  case scUDivExpr: {
    const SCEVUDivExpr *LC = cast<SCEVUDivExpr>(LHS);
    const SCEVUDivExpr *RC = cast<SCEVUDivExpr>(RHS);

    // Lexicographically compare udiv expressions.
    int X = CompareSCEVComplexity(EqCacheSCEV, LI, LC->getLHS(), RC->getLHS(),
      Depth + 1);
    if (X != 0)
      return X;
    X = CompareSCEVComplexity(EqCacheSCEV, LI, LC->getRHS(), RC->getRHS(),
      Depth + 1);
    if (X == 0)
      EqCacheSCEV.insert({ LHS, RHS });
    return X;
  }

  case scTruncate:
  case scZeroExtend:
  case scSignExtend: {
    const SCEVCastExpr *LC = cast<SCEVCastExpr>(LHS);
    const SCEVCastExpr *RC = cast<SCEVCastExpr>(RHS);

    // Compare cast expressions by operand.
    int X = CompareSCEVComplexity(EqCacheSCEV, LI, LC->getOperand(),
      RC->getOperand(), Depth + 1);
    if (X == 0)
      EqCacheSCEV.insert({ LHS, RHS });
    return X;
  }

  case scCouldNotCompute:
    llvm_unreachable("Attempt to use a SCEVCouldNotCompute object!");
  }
  llvm_unreachable("Unknown SCEV kind!");
}

static inline int sizeOfSCEV(const SCEV *S) {
  struct FindSCEVSize {
    int Size;
    FindSCEVSize() : Size(0) {}

    bool follow(const SCEV *S) {
      ++Size;
      // Keep looking at all operands of S.
      return true;
    }
    bool isDone() const {
      return false;
    }
  };

  FindSCEVSize F;
  SCEVTraversal<FindSCEVSize> ST(F);
  ST.visitAll(S);
  return F.Size;
}

struct SCEVDivision : public SCEVVisitor<SCEVDivision, void> {
public:
  // Computes the Quotient and Remainder of the division of Numerator by
  // Denominator.
  static void divide(ScalarEvolution &SE, const SCEV *Numerator,
    const SCEV *Denominator, const SCEV **Quotient,
    const SCEV **Remainder) {
    assert(Numerator && Denominator && "Uninitialized SCEV");

    SCEVDivision D(SE, Numerator, Denominator);

    // Check for the trivial case here to avoid having to check for it in the
    // rest of the code.
    if (Numerator == Denominator) {
      *Quotient = D.One;
      *Remainder = D.Zero;
      return;
    }

    if (Numerator->isZero()) {
      *Quotient = D.Zero;
      *Remainder = D.Zero;
      return;
    }

    // A simple case when N/1. The quotient is N.
    if (Denominator->isOne()) {
      *Quotient = Numerator;
      *Remainder = D.Zero;
      return;
    }

    // Split the Denominator when it is a product.
    if (const SCEVMulExpr *T = dyn_cast<SCEVMulExpr>(Denominator)) {
      const SCEV *Q, *R;
      *Quotient = Numerator;
      for (const SCEV *Op : T->operands()) {
        divide(SE, *Quotient, Op, &Q, &R);
        *Quotient = Q;

        // Bail out when the Numerator is not divisible by one of the terms of
        // the Denominator.
        if (!R->isZero()) {
          *Quotient = D.Zero;
          *Remainder = Numerator;
          return;
        }
      }
      *Remainder = D.Zero;
      return;
    }

    D.visit(Numerator);
    *Quotient = D.Quotient;
    *Remainder = D.Remainder;
  }

  // Except in the trivial case described above, we do not know how to divide
  // Expr by Denominator for the following functions with empty implementation.
  void visitTruncateExpr(const SCEVTruncateExpr *Numerator) {
    const SCEV *CurrentNumerator, *CurrentDenominator, *Q, *R;
    if (auto *CastDenominator = dyn_cast<SCEVCastExpr>(Denominator)) {
      if (Numerator->getOperand()->getType()
        == CastDenominator->getOperand()->getType()) {
        CurrentNumerator = Numerator->getOperand();
        CurrentDenominator = CastDenominator->getOperand();
      } else {
        CurrentNumerator = nullptr;
        CurrentDenominator = nullptr;
      }
    } else {
      if (Numerator->getOperand()->getType() == Denominator->getType()) {
        CurrentNumerator = Numerator->getOperand();
        CurrentDenominator = Denominator;
      } else {
        CurrentNumerator = nullptr;
        CurrentDenominator = nullptr;
      }
    }
    if (CurrentNumerator && CurrentDenominator) {
      divide(SE, CurrentNumerator, CurrentDenominator, &Q, &R);
      Quotient = SE.getTruncateExpr(Q, Numerator->getType());
      Remainder = SE.getTruncateExpr(R, Numerator->getType());
    }
  }

  void visitZeroExtendExpr(const SCEVZeroExtendExpr *Numerator) {
    const SCEV *CurrentNumerator, *CurrentDenominator, *Q, *R;
    if (auto *CastDenominator = dyn_cast<SCEVCastExpr>(Denominator)) {
      if (Numerator->getOperand()->getType()
        == CastDenominator->getOperand()->getType()) {
        CurrentNumerator = Numerator->getOperand();
        CurrentDenominator = CastDenominator->getOperand();
      } else {
        CurrentNumerator = nullptr;
        CurrentDenominator = nullptr;
      }
    } else {
      if (Numerator->getOperand()->getType() == Denominator->getType()) {
        CurrentNumerator = Numerator->getOperand();
        CurrentDenominator = Denominator;
      } else {
        CurrentNumerator = nullptr;
        CurrentDenominator = nullptr;
      }
    }
    if (CurrentNumerator && CurrentDenominator) {
      divide(SE, CurrentNumerator, CurrentDenominator, &Q, &R);
      Quotient = SE.getZeroExtendExpr(Q, Numerator->getType());
      Remainder = SE.getZeroExtendExpr(R, Numerator->getType());
    }
  }

  void visitSignExtendExpr(const SCEVSignExtendExpr *Numerator) {
    const SCEV *CurrentNumerator, *CurrentDenominator, *Q, *R;
    if (auto *CastDenominator = dyn_cast<SCEVCastExpr>(Denominator)) {
      if (Numerator->getOperand()->getType()
        == CastDenominator->getOperand()->getType()) {
        CurrentNumerator = Numerator->getOperand();
        CurrentDenominator = CastDenominator->getOperand();
      } else {
        CurrentNumerator = nullptr;
        CurrentDenominator = nullptr;
      }
    } else {
      if (Numerator->getOperand()->getType() == Denominator->getType()) {
        CurrentNumerator = Numerator->getOperand();
        CurrentDenominator = Denominator;
      } else {
        CurrentNumerator = nullptr;
        CurrentDenominator = nullptr;
      }
    }
    if (CurrentNumerator && CurrentDenominator) {
      divide(SE, CurrentNumerator, CurrentDenominator, &Q, &R);
      Quotient = SE.getSignExtendExpr(Q, Numerator->getType());
      Remainder = SE.getSignExtendExpr(R, Numerator->getType());
    }
  }

  void visitUDivExpr(const SCEVUDivExpr *Numerator) {}
  void visitSMaxExpr(const SCEVSMaxExpr *Numerator) {}
  void visitUMaxExpr(const SCEVUMaxExpr *Numerator) {}
  void visitUnknown(const SCEVUnknown *Numerator) {}
  void visitCouldNotCompute(const SCEVCouldNotCompute *Numerator) {}

  void visitConstant(const SCEVConstant *Numerator) {
    if (const SCEVConstant *D = dyn_cast<SCEVConstant>(Denominator)) {
      APInt NumeratorVal = Numerator->getAPInt();
      APInt DenominatorVal = D->getAPInt();
      uint32_t NumeratorBW = NumeratorVal.getBitWidth();
      uint32_t DenominatorBW = DenominatorVal.getBitWidth();

      if (NumeratorBW > DenominatorBW)
        DenominatorVal = DenominatorVal.sext(NumeratorBW);
      else if (NumeratorBW < DenominatorBW)
        NumeratorVal = NumeratorVal.sext(DenominatorBW);

      APInt QuotientVal(NumeratorVal.getBitWidth(), 0);
      APInt RemainderVal(NumeratorVal.getBitWidth(), 0);
      APInt::sdivrem(NumeratorVal, DenominatorVal, QuotientVal, RemainderVal);
      Quotient = SE.getConstant(QuotientVal);
      Remainder = SE.getConstant(RemainderVal);
      return;
    }
  }

  void visitAddRecExpr(const SCEVAddRecExpr *Numerator) {
    const SCEV *StartQ, *StartR, *StepQ, *StepR;
    if (!Numerator->isAffine())
      return cannotDivide(Numerator);
    divide(SE, Numerator->getStart(), Denominator, &StartQ, &StartR);
    divide(SE, Numerator->getStepRecurrence(SE), Denominator, &StepQ, &StepR);
    // Bail out if the types do not match.
    Type *Ty = Denominator->getType();
    if (Ty != StartQ->getType() || Ty != StartR->getType() ||
      Ty != StepQ->getType() || Ty != StepR->getType())
      return cannotDivide(Numerator);
    Quotient = SE.getAddRecExpr(StartQ, StepQ, Numerator->getLoop(),
      Numerator->getNoWrapFlags());
    Remainder = SE.getAddRecExpr(StartR, StepR, Numerator->getLoop(),
      Numerator->getNoWrapFlags());
  }

  void visitAddExpr(const SCEVAddExpr *Numerator) {
    SmallVector<const SCEV *, 2> Qs, Rs;
    Type *Ty = Denominator->getType();

    for (const SCEV *Op : Numerator->operands()) {
      const SCEV *Q, *R;
      divide(SE, Op, Denominator, &Q, &R);

      // Bail out if types do not match.
      if (Ty != Q->getType() || Ty != R->getType())
        return cannotDivide(Numerator);

      Qs.push_back(Q);
      Rs.push_back(R);
    }

    if (Qs.size() == 1) {
      Quotient = Qs[0];
      Remainder = Rs[0];
      return;
    }

    Quotient = SE.getAddExpr(Qs);
    Remainder = SE.getAddExpr(Rs);
  }

  void visitMulExpr(const SCEVMulExpr *Numerator) {
    SmallVector<const SCEV *, 2> Qs;
    Type *Ty = Denominator->getType();

    bool FoundDenominatorTerm = false;
    for (const SCEV *Op : Numerator->operands()) {
      // Bail out if types do not match.
      if (Ty != Op->getType())
        return cannotDivide(Numerator);

      if (FoundDenominatorTerm) {
        Qs.push_back(Op);
        continue;
      }

      // Check whether Denominator divides one of the product operands.
      const SCEV *Q, *R;
      divide(SE, Op, Denominator, &Q, &R);
      if (!R->isZero()) {
        Qs.push_back(Op);
        continue;
      }

      // Bail out if types do not match.
      if (Ty != Q->getType())
        return cannotDivide(Numerator);

      FoundDenominatorTerm = true;
      Qs.push_back(Q);
    }

    if (FoundDenominatorTerm) {
      Remainder = Zero;
      if (Qs.size() == 1)
        Quotient = Qs[0];
      else
        Quotient = SE.getMulExpr(Qs);
      return;
    }

    if (!isa<SCEVUnknown>(Denominator))
      return cannotDivide(Numerator);

    // The Remainder is obtained by replacing Denominator by 0 in Numerator.
    ValueToValueMap RewriteMap;
    RewriteMap[cast<SCEVUnknown>(Denominator)->getValue()] =
      cast<SCEVConstant>(Zero)->getValue();
    Remainder = SCEVParameterRewriter::rewrite(Numerator, SE, RewriteMap, true);

    if (Remainder->isZero()) {
      // The Quotient is obtained by replacing Denominator by 1 in Numerator.
      RewriteMap[cast<SCEVUnknown>(Denominator)->getValue()] =
        cast<SCEVConstant>(One)->getValue();
      Quotient =
        SCEVParameterRewriter::rewrite(Numerator, SE, RewriteMap, true);
      return;
    }

    // Quotient is (Numerator - Remainder) divided by Denominator.
    const SCEV *Q, *R;
    const SCEV *Diff = SE.getMinusSCEV(Numerator, Remainder);
    // This SCEV does not seem to simplify: fail the division here.
    if (sizeOfSCEV(Diff) > sizeOfSCEV(Numerator))
      return cannotDivide(Numerator);
    divide(SE, Diff, Denominator, &Q, &R);
    if (R != Zero)
      return cannotDivide(Numerator);
    Quotient = Q;
  }

private:
  SCEVDivision(ScalarEvolution &S, const SCEV *Numerator,
    const SCEV *Denominator)
    : SE(S), Denominator(Denominator) {
    Zero = SE.getZero(Denominator->getType());
    One = SE.getOne(Denominator->getType());

    // We generally do not know how to divide Expr by Denominator. We
    // initialize the division to a "cannot divide" state to simplify the rest
    // of the code.
    cannotDivide(Numerator);
  }

  // Convenience function for giving up on the division. We set the quotient to
  // be equal to zero and the remainder to be equal to the numerator.
  void cannotDivide(const SCEV *Numerator) {
    Quotient = Zero;
    Remainder = Numerator;
  }

  ScalarEvolution &SE;
  const SCEV *Denominator, *Quotient, *Remainder, *Zero, *One;
};

Value *GetUnderlyingObjectWithDbgCheck(Value *V, const DataLayout &DL,
  unsigned MaxLookup) {
  if (!V->getType()->isPointerTy())
    return V;
  for (unsigned Count = 0; MaxLookup == 0 || Count < MaxLookup; ++Count) {
    SmallVector<DbgValueInst *, 1> DbgValues;
    findDbgValues(DbgValues, V);
    if (!DbgValues.empty()) {
      return V;
    }
    if (GEPOperator *GEP = dyn_cast<GEPOperator>(V)) {
      V = GEP->getPointerOperand();
    } else if (Operator::getOpcode(V) == Instruction::BitCast ||
      Operator::getOpcode(V) == Instruction::AddrSpaceCast) {
      V = cast<Operator>(V)->getOperand(0);
    } else if (GlobalAlias *GA = dyn_cast<GlobalAlias>(V)) {
      if (GA->isInterposable())
        return V;
      V = GA->getAliasee();
    } else {
      if (auto CS = CallSite(V))
        if (Value *RV = CS.getReturnedArgOperand()) {
          V = RV;
          continue;
        }

      // See if InstructionSimplify knows any relevant tricks.
      if (Instruction *I = dyn_cast<Instruction>(V))
        // TODO: Acquire a DominatorTree and AssumptionCache and use them.
        if (Value *Simplified = SimplifyInstruction(I, DL, nullptr)) {
          V = Simplified;
          continue;
        }

      return V;
    }
    assert(V->getType()->isPointerTy() && "Unexpected operand type!");
  }
  return V;
}

const DIVariable* findVariableDbg(Value* V) {
  assert(V && "Value must not be null");
  if (auto *GV = dyn_cast<GlobalVariable>(V)) {
    SmallVector<DIMemoryLocation, 1> DILocs;
    auto DIM = findMetadata(GV, DILocs);
    return DIM ? DIM->Var : nullptr;
  }

  if (auto *AI = dyn_cast<AllocaInst>(V)) {
    SmallVector<DIMemoryLocation, 1> DILocs;
    auto DIM = findMetadata(AI, DILocs);
    return DIM ? DIM->Var : nullptr;
  }

  //TODO replace by a new function from tsar_ulility
  auto DIIList = FindDbgAddrUses(V);
  if (DIIList.size() == 1) {
    return DIIList.front()->getVariable();
  }

  SmallVector<DbgValueInst *, 1> DbgValues;
  findDbgValues(DbgValues, V);
  if (!DbgValues.empty()) {
    return DbgValues[0]->getVariable();
  }

  return nullptr;
}

Value *findRootArray(Instruction *I) {
  Value *PointerOp = nullptr;
  if (auto *SI = dyn_cast<StoreInst>(I)) {
    PointerOp = SI->getPointerOperand();
  } else if (auto *LI = dyn_cast<LoadInst>(I)) {
    PointerOp = LI->getPointerOperand();
  /*} else if (auto *CI = dyn_cast<CallInst>(I)) {

  } */
  } else {
    LLVM_DEBUG(
      dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] In search of root array was ignored:\n\t";
      I->dump();
    );
  }

  if (!PointerOp) {
    return nullptr;
  }

  if (auto *Gep = dyn_cast<GetElementPtrInst>(PointerOp)) {
    Value * RootArr = (GetUnderlyingObjectWithDbgCheck(Gep, Gep->getModule()->getDataLayout(), 0));
    //For function argument GetUnderlyingObject returns LoadInst with
    //RootArr in operands
    //����� �������� �� ��� ���� �������, �������� �� alloc � ���������� ����������
    //foo(int **A) {
    //  int *B = *A;
    //  B[10] = 0;
    if (auto *LI = dyn_cast<LoadInst>(RootArr)) {
      RootArr = LI->getPointerOperand();
    }

    LLVM_DEBUG(
      dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] RootArr " << *RootArr << "\n";
    );
    return RootArr;
  }

  if (auto *GV = dyn_cast<GlobalVariable>(PointerOp)) {
    LLVM_DEBUG(
      dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] RootArrGV " << *GV << "\n";
    );
    return GV;
  }

  return nullptr;
}

void findArrayDimesionsFromDbgInfo(Value *RootArr,
  SmallVectorImpl<int64_t> &Dimensions) {
  assert(RootArr && "RootArray must not be null");

  Dimensions.clear();

  auto *VarDbg = findVariableDbg(RootArr);
  if (!VarDbg)
    return;

  auto VarDbgType = VarDbg->getType();
  if (auto *GVE = dyn_cast<DIGlobalVariableExpression>(VarDbgType)) {
    //GVE->
  }

  DINodeArray TypeElements = nullptr;
  bool IsFirstDimPointer = false;
  if (auto *ArrayCompositeType = dyn_cast<DICompositeType>(VarDbgType)) {
    if (ArrayCompositeType->getTag() == dwarf::DW_TAG_array_type) {
      TypeElements = ArrayCompositeType->getElements();
    }
  } else if (auto *DerivedType = dyn_cast<DIDerivedType>(VarDbgType)) {
    if (DerivedType->getTag() == dwarf::DW_TAG_pointer_type ||
      DerivedType->getTag() == dwarf::DW_TAG_array_type) {
      if (DerivedType->getTag() == dwarf::DW_TAG_pointer_type) {
        IsFirstDimPointer = true;
      }

      if (auto *InnerCompositeType =
        dyn_cast<DICompositeType>(DerivedType->getBaseType())) {
        if (InnerCompositeType->getTag() == dwarf::DW_TAG_array_type) {
          TypeElements = InnerCompositeType->getElements();
        }
      } else {
        //TypeElements = DerivedType->getBaseType();
      }
    }
  }

  LLVM_DEBUG(
    if (IsFirstDimPointer && TypeElements) {
      dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Array dimensions count: "
        << TypeElements.size() + 1 << "\n\t"
        << RootArr->getName() << "[]";
    } else {
      dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Array dimensions count: "
      << TypeElements.size() << "\n\t"
      << RootArr->getName();
    }
  );

  if (IsFirstDimPointer) {
    Dimensions.push_back(-1);
  }

  if (!TypeElements) {
    return;
  }

  for (unsigned int i = 0; i < TypeElements.size(); ++i) {
    if (auto *DimSize = dyn_cast<DISubrange>(TypeElements[i])) {
      auto DimSizeCount = DimSize->getCount();
      int64_t DimSizeValue = -1;
      if (DimSizeCount.is<ConstantInt*>()) {
        DimSizeValue = DimSizeCount.get<ConstantInt*>()->getLimitedValue();
        LLVM_DEBUG(
          dbgs() << "\tConstInt: " << DimSizeValue << "\n";
        );
      } else if (DimSizeCount.is<DIVariable *>()) {
        LLVM_DEBUG(
          dbgs() << "\tDIVar: ";
          DimSizeCount.get<DIVariable *>()->dump();
        );
      } else {
        LLVM_DEBUG(
          dbgs() << "\tDimSizeCount is null\n";
        );
      }
      Dimensions.push_back(DimSizeValue);
      LLVM_DEBUG(
        dbgs() << "[";
      if (DimSizeValue > 0)
        dbgs() << DimSizeValue;
      dbgs() << "]";
      );
    }
  }
  LLVM_DEBUG(
    dbgs() << "\n";
  );
}

GetElementPtrInst *findGepOperand(Instruction *I) {
  assert(I && "Instruction must not be null");
  /*LLVM_DEBUG(
    dbgs() << "Find Gep operand start\n";
  );*/
  for (unsigned int i = 0; i < I->getNumOperands(); ++i) {
    /*LLVM_DEBUG(
      I->getOperand(i)->dump();
    );*/
    if (auto *Gep = dyn_cast<GetElementPtrInst>(I->getOperand(i))) {
      LLVM_DEBUG(
        dbgs() << "Find Gep operand finish with gep\n";
      Gep->dump();
      );
      return Gep;
    }
  }
  LLVM_DEBUG(
    dbgs() << "Find Gep operand finish with nullptr\n";
  );
  return nullptr;
}

void findGeps(Instruction *I, SmallVectorImpl<int64_t> &Dimensions,
  SmallVectorImpl<GetElementPtrInst *> &Geps) {
  assert(I && "Instruction must not be null");
  auto *CurrentInst = I;
  do {
    LLVM_DEBUG(
      dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE]GEPS search current inst: ";
      CurrentInst->dump();
      /*dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE]GEPS size: " << Geps.size() << "\n";
      dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE]Dimensions size: " << Dimensions.size() << "\n";*/
    );
    auto *Gep = findGepOperand(CurrentInst);
    if (Gep) {
      Geps.push_back(Gep);
      CurrentInst = Gep;
    } else {
      CurrentInst = nullptr;
      for (unsigned int i = 0; i < I->getNumOperands(); ++i) {
        if (auto *LI = dyn_cast<LoadInst>(I->getOperand(i))) {
          /*LLVM_DEBUG(
            dbgs() << "Found LI";
            LI->dump();
          );*/
          if (auto* InnerGep = findGepOperand(LI)) {
            /*LLVM_DEBUG(
              dbgs() << "Found InnerGep";
              InnerGep->dump();
            );*/
            Geps.push_back(InnerGep);
            CurrentInst = InnerGep;
            break;
          } else {
           /* LLVM_DEBUG(
              dbgs() << "Dont found InnerGep";
            );*/
          }
        }
      }
    }
  } while (CurrentInst && (Geps.size() < Dimensions.size()));
  std::reverse(Geps.begin(), Geps.end());
  LLVM_DEBUG(
    dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE]GEPS size: " << Geps.size() << "\n";
  );
}

void findLLVMIdxs(Value *RootArr, SmallVectorImpl<GetElementPtrInst *> &Geps,
  ScalarEvolution &SE, SmallVectorImpl<Value *> &Idxs) {
  assert(RootArr && "RootArr must not be null");
  assert(!Geps.empty() && "Geps size must not be zero");
  Idxs.clear();
  for (auto &Gep : Geps) {
    int numOperands = Gep->getNumOperands();
    if (numOperands == 2) {
      Idxs.push_back(Gep->getOperand(1));
    } else {
      if (auto *SecondOp = dyn_cast<ConstantData>(Gep->getOperand(1))) {
        if (!SecondOp->isZeroValue()) {
          Idxs.push_back(Gep->getOperand(1));
        }
      } else {
        Idxs.push_back(Gep->getOperand(1));
      }
      for (int i = 2; i < numOperands; ++i) {
        Idxs.push_back(Gep->getOperand(i));
      }
    }
  }
}

void countPrimeNumbers(uint64_t Bound, std::vector<uint64_t> &Primes) {
  //Implements Sieve of Atkin with cache

  enum {
    PRIMES_CACHE_SIZE = 60
  };

  static uint64_t CachedPrimes[PRIMES_CACHE_SIZE] = {
    2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53, 59,
    61, 67, 71, 73, 79, 83, 89, 97, 101, 103, 107, 109, 113, 127, 131,
    137, 139, 149, 151, 157, 163, 167, 173, 179, 181, 191, 193, 197,
    199, 211, 223, 227, 229, 233, 239, 241, 251, 257, 263, 269, 271,
    277, 281
  };

  Primes.clear();

  if (Bound <= CachedPrimes[PRIMES_CACHE_SIZE - 1]) {
    for (int i = 0; i < PRIMES_CACHE_SIZE; i++) {
      if (CachedPrimes[i] <= Bound) {
        Primes.push_back(CachedPrimes[i]);
      } else {
        break;
      }
    }
    return;
  }

  std::vector<bool> IsPrime;
  IsPrime.resize(Bound + 1);
  for (int i = 0; i <= Bound; i++) {
    IsPrime[i] = false;
  }
  IsPrime[2] = true;
  IsPrime[3] = true;
  uint64_t BoundSqrt = (int)sqrt(Bound);

  uint64_t x2 = 0, y2, n;
  for (int i = 1; i <= BoundSqrt; i++) {
    x2 += 2 * i - 1;
    y2 = 0;
    for (int j = 1; j <= BoundSqrt; j++) {
      y2 += 2 * j - 1;
      n = 4 * x2 + y2;
      if ((n <= Bound) && (n % 12 == 1 || n % 12 == 5)) {
        IsPrime[n] = !IsPrime[n];
      }

      n -= x2;
      if ((n <= Bound) && (n % 12 == 7)) {
        IsPrime[n] = !IsPrime[n];
      }

      n -= 2 * y2;
      if ((i > j) && (n <= Bound) && (n % 12 == 11)) {
        IsPrime[n] = !IsPrime[n];
      }
    }
  }

  for (int i = 5; i <= BoundSqrt; i++) {
    if (IsPrime[i]) {
      n = i * i;
      for (uint64_t j = n; j <= Bound; j += n) {
        IsPrime[j] = false;
      }
    }
  }

  Primes.push_back(2);
  Primes.push_back(3);
  Primes.push_back(5);
  for (int i = 6; i <= Bound; i++) {
    if ((IsPrime[i]) && (i % 3) && (i % 5)) {
      Primes.push_back(i);
    }
  }
}

void countConstantMultipliers(const SCEVConstant *Const,
  ScalarEvolution &SE, SmallVectorImpl<const SCEV *> &Multipliers) {
  uint64_t ConstValue = Const->getAPInt().getLimitedValue();
  assert(ConstValue && "Constant value is zero");

  if (ConstValue < 0) {
    Multipliers.push_back(SE.getConstant(Const->getType(), -1, true));
    ConstValue *= -1;
  }
  if (ConstValue == 1) {
    Multipliers.push_back(SE.getConstant(Const->getType(), 1, false));
    return;
  }
  std::vector<uint64_t> Primes;
  countPrimeNumbers(ConstValue, Primes);
  size_t i = Primes.size() - 1;
  LLVM_DEBUG(
    dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Constant Multipliers:\n";
  );
  while (ConstValue > 1) {
    if (ConstValue % Primes[i] == 0) {
      Multipliers.push_back(SE.getConstant(Const->getType(), Primes[i], false));
      LLVM_DEBUG(
        dbgs() << "\t";
        Multipliers[Multipliers.size() - 1]->dump();
      );
      ConstValue /= Primes[i];
    } else {
      i--;
    }
  }
}

const SCEV* findGCD(SmallVectorImpl<const SCEV *> &Expressions,
  ScalarEvolution &SE) {
  assert(!Expressions.empty() && "GCD Expressions size must not be zero");

  SmallVector<const SCEV *, 3> Terms;

  //Release AddRec Expressions, multipliers are in step and start expressions
  for (auto *Expr : Expressions) {
    switch (Expr->getSCEVType()) {
    case scTruncate:
    case scZeroExtend:
    case scSignExtend: {
      auto *CastExpr = cast<SCEVCastExpr>(Expr);
      auto *InnerOp = CastExpr->getOperand();
      switch (InnerOp->getSCEVType()) {
      case scAddRecExpr: {
        auto *AddRec = cast<SCEVAddRecExpr>(InnerOp);
        auto *AddRecStepRecurrence = AddRec->getStepRecurrence(SE);
        auto *AddRecStart = AddRec->getStart();
        if (Expr->getSCEVType() == scTruncate) {
          Terms.push_back(SE.getTruncateExpr(AddRecStepRecurrence, Expr->getType()));
          Terms.push_back(SE.getTruncateExpr(AddRecStart, Expr->getType()));
        }
        if (Expr->getSCEVType() == scSignExtend) {
          Terms.push_back(SE.getSignExtendExpr(AddRecStepRecurrence, Expr->getType()));
          Terms.push_back(SE.getSignExtendExpr(AddRecStart, Expr->getType()));
        }
        if (Expr->getSCEVType() == scZeroExtend) {
          Terms.push_back(SE.getZeroExtendExpr(AddRecStepRecurrence, Expr->getType()));
          Terms.push_back(SE.getZeroExtendExpr(AddRecStart, Expr->getType()));
        }
        break;
      }
      case scUnknown:
      case scAddExpr:
      case scMulExpr: {
        Terms.push_back(Expr);
        break;
      }
      }
      break;
    }
    case scConstant:
    case scUnknown:
    case scAddExpr: {
      Terms.push_back(Expr);
      break;
    }
    case scMulExpr: {
      auto *MulExpr = cast<SCEVMulExpr>(Expr);
      bool hasAddRec = false;
      SmallVector<const SCEV *, 3> StepMultipliers, StartMultipliers;
      for (int i = 0; i < MulExpr->getNumOperands(); ++i) {
        auto *Op = MulExpr->getOperand(i);
        switch (Op->getSCEVType()) {
        case scTruncate:
        case scZeroExtend:
        case scSignExtend: {
          auto *InnerOp = cast<SCEVCastExpr>(Op)->getOperand();

          if (auto *AddRec = dyn_cast<SCEVAddRecExpr>(InnerOp)) {
            hasAddRec = true;
            auto *AddRecStepRecurrence = AddRec->getStepRecurrence(SE);
            auto *AddRecStart = AddRec->getStart();
            if (Op->getSCEVType() == scTruncate) {
              StepMultipliers.push_back(SE.getTruncateExpr(AddRecStepRecurrence, Op->getType()));
              if (!AddRecStart->isZero()) {
                StartMultipliers.push_back(SE.getTruncateExpr(AddRecStart, Op->getType()));
              }
            }
            if (Op->getSCEVType() == scSignExtend) {
              StepMultipliers.push_back(SE.getSignExtendExpr(AddRecStepRecurrence, Op->getType()));
              if (!AddRecStart->isZero()) {
                StartMultipliers.push_back(SE.getSignExtendExpr(AddRecStart, Op->getType()));
              }
            }
            if (Op->getSCEVType() == scZeroExtend) {
              StepMultipliers.push_back(SE.getZeroExtendExpr(AddRecStepRecurrence, Op->getType()));
              if (!AddRecStart->isZero()) {
                StartMultipliers.push_back(SE.getZeroExtendExpr(AddRecStart, Op->getType()));
              }
            }
          } else if (auto *InnerMulExpr = dyn_cast<SCEVMulExpr>(InnerOp)) {
            StepMultipliers.push_back(Op);
            StartMultipliers.push_back(Op);
          } else if (InnerOp->getSCEVType() == scUnknown ||
            InnerOp->getSCEVType() == scAddExpr) {
            StepMultipliers.push_back(Op);
            StartMultipliers.push_back(Op);
          }
         break;
        }
        case scAddRecExpr: {
          auto *AddRec = cast<SCEVAddRecExpr>(Op);
          hasAddRec = true;
          auto *AddRecStepRecurrence = AddRec->getStepRecurrence(SE);
          auto *AddRecStart = AddRec->getStart();
          StepMultipliers.push_back(AddRecStepRecurrence);
          if (!AddRecStart->isZero()) {
            StartMultipliers.push_back(AddRecStart);
          }
          break;
        }
        case scUnknown:
        case scAddExpr:
        case scConstant: {
          StepMultipliers.push_back(Op);
          StartMultipliers.push_back(Op);
          break;
        }
        }
      }
      if (hasAddRec && !StartMultipliers.empty()) {
        SmallVector<const SCEV *, 2> AddRecInnerExpressions = {
          SE.getMulExpr(StartMultipliers),
          SE.getMulExpr(StepMultipliers)
        };
        Terms.push_back(findGCD(AddRecInnerExpressions, SE));
      } else if (!StepMultipliers.empty()) {
        Terms.push_back(SE.getMulExpr(StepMultipliers));
      }
     break;
    }
    case scAddRecExpr: {
      auto *AddRec = cast<SCEVAddRecExpr>(Expr);
      auto *AddRecStepRecurrence = AddRec->getStepRecurrence(SE);
      /*SCEVConstant *AddRecStartConstantMultiplier = nullptr;
      SCEVConstant *AddRecStepConstantMultiplier = nullptr;*/

      if (auto *MulExpr = dyn_cast<SCEVMulExpr>(AddRecStepRecurrence)) {
        SmallVector<const SCEV *, 2> Multipliers;
        for (int i = 0; i < MulExpr->getNumOperands(); ++i) {
          auto *Op = MulExpr->getOperand(i);
          if (Op->getSCEVType() == scUnknown ||
            Op->getSCEVType() == scTruncate ||
            Op->getSCEVType() == scSignExtend ||
            Op->getSCEVType() == scZeroExtend ||
            Op->getSCEVType() == scAddExpr ||
            Op->getSCEVType() == scConstant) {
            Multipliers.push_back(Op);
          }
          //if (auto *Const = dyn_cast<SCEVConstant>(Op)) {

          //}
        }
        AddRecStepRecurrence = SE.getMulExpr(Multipliers);
      }

      auto *AddRecStart = AddRec->getStart();
      if (auto *MulExpr = dyn_cast<SCEVMulExpr>(AddRecStart)) {
        SmallVector<const SCEV *, 2> Multipliers;
        for (int i = 0; i < MulExpr->getNumOperands(); ++i) {
          auto *Op = MulExpr->getOperand(i);
          if (Op->getSCEVType() == scUnknown ||
            Op->getSCEVType() == scTruncate ||
            Op->getSCEVType() == scSignExtend ||
            Op->getSCEVType() == scZeroExtend ||
            Op->getSCEVType() == scAddExpr ||
            Op->getSCEVType() == scConstant) {
            Multipliers.push_back(Op);
          }
        }
        AddRecStart = SE.getMulExpr(Multipliers);
      }
      SmallVector<const SCEV *, 2> AddRecInnerExpressions = {AddRecStart, AddRecStepRecurrence};
      //AddRecInnerExpressions.push_back();
      Terms.push_back(findGCD(AddRecInnerExpressions, SE));
      /*Terms.push_back(AddRecStepRecurrence);
      Terms.push_back(AddRecStart);*/
      break;
    }
    }
  }

  LLVM_DEBUG(
    dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] GCD Terms:\n";
    for (auto *Term : Terms) {
      dbgs() << "\t";
      Term->dump();
    });

  if (Terms.empty()) {
    return SE.getConstant(Expressions[0]->getType(), 1, true);
  }

  SmallVector<const SCEV *, 3> Dividers;

  const SCEV* OpeningSCEV = nullptr;

  //Finding not zero SCEV in Terms
  for (auto *Term : Terms) {
    if (!Term->isZero()) {
      OpeningSCEV = Term;
      break;
    }
  }
  if (!OpeningSCEV) {
    return SE.getConstant(Expressions[0]->getType(), 0, true);
  }

  //Start from multipliers of first SCEV, then exclude them step by step
  if (auto *Mul = dyn_cast<SCEVMulExpr>(OpeningSCEV)) {
    for (int i = 0; i < Mul->getNumOperands(); ++i) {
      if (auto *Const = dyn_cast<SCEVConstant>(Mul->getOperand(i))) {
        SmallVector<const SCEV *, 3> ConstMultipliers;
        countConstantMultipliers(Const, SE, ConstMultipliers);
        Dividers.append(ConstMultipliers.begin(), ConstMultipliers.end());
      } else {
        Dividers.push_back(Mul->getOperand(i));
      }
    }
  } else {
    if (auto *Const = dyn_cast<SCEVConstant>(OpeningSCEV)) {
      SmallVector<const SCEV *, 3> ConstMultipliers;
      countConstantMultipliers(Const, SE, ConstMultipliers);
      Dividers.append(ConstMultipliers.begin(), ConstMultipliers.end());
    } else {
      Dividers.push_back(OpeningSCEV);
    }
  }

  for (int i = 1; i < Terms.size(); ++i) {
    auto *CurrentTerm = Terms[i];
    SmallVector<const SCEV *, 3> ActualStepDividers;

    for (auto *Divider : Dividers) {
      const SCEV *Q, *R;
      SCEVDivision::divide(SE, CurrentTerm, Divider, &Q, &R);
      if (R->isZero()) {
        ActualStepDividers.push_back(Divider);
        CurrentTerm = Q;
        if (ActualStepDividers.size() == Dividers.size()) {
          break;
        }
      }
    }

    Dividers = ActualStepDividers;
    if (Dividers.size() == 0) {
      return SE.getConstant(Expressions[0]->getType(), 1, true);
    }
  }

  if (Dividers.size() == 1) {
    return Dividers[0];
  } else {
    return SE.getMulExpr(Dividers);
  }

}

void collectArrays(Function &F, ScalarEvolution &SE, TargetLibraryInfo &TLI,
  DenseMap<Value *, SmallVector <int64_t, 3> > &CachedArraysDbgInfo,
  DelinearizeInfo::ArraySet &AnalyzedArrays) {

  for (auto &BB :  F) {
    for (auto &I : BB) {
      auto ProcessAccessInstruction = [&AnalyzedArrays, &SE, &CachedArraysDbgInfo](Instruction &I) {
        Value *RootArr = findRootArray(&I);
        if (!RootArr) {
          return;
        }

        SmallVector<int64_t, 3> Dimensions;
        auto ArrayDbgInfoCache = CachedArraysDbgInfo.find(RootArr);
        if (ArrayDbgInfoCache != CachedArraysDbgInfo.end()) {
          Dimensions = ArrayDbgInfoCache->second;
        } else {
          findArrayDimesionsFromDbgInfo(RootArr, Dimensions);
          CachedArraysDbgInfo.insert(std::make_pair(RootArr, Dimensions));
        }

        SmallVector<GetElementPtrInst *, 3> Geps;
        findGeps(&I, Dimensions, Geps);

        if (Geps.empty()) {
          return;
        }

        SmallVector<Value *, 3> Idxs;
        findLLVMIdxs(RootArr, Geps, SE, Idxs);

        if (Idxs.empty()) {
          return;
        }

        Array *CurrentArray;
        auto ArrayEntryIt = AnalyzedArrays.find_as(RootArr);
        if (ArrayEntryIt != AnalyzedArrays.end()) {
          CurrentArray = &(*ArrayEntryIt);
        } else {
          auto InsertResult = AnalyzedArrays.insert(Array(RootArr));
          if (!InsertResult.second) {
            return;
          }
          CurrentArray = &(*InsertResult.first);
        }

        auto &El = CurrentArray->addElement(Geps[0]);

        //In some cases zero subscript is dropping out by optimizing passes
        if (Idxs.size() < Dimensions.size()) {
          El.IsValid = false;
          Type *IdxsType;
          if (Idxs.size()) {
            IdxsType = Idxs.front()->getType();
          } else {
            IdxsType = Geps[0]->getOperand(1)->getType();
          }
          for (int i = 0; i < Dimensions.size() - Idxs.size(); i++) {
            El.Subscripts.push_back(SE.getZero(IdxsType));
            LLVM_DEBUG(
              dbgs() << "Adding exta zero subscript\n";
            );
          }
        } else {
          El.IsValid = true;
        }
        for (auto *Idx : Idxs) {
          auto *IdxSCEV = SE.getSCEV(Idx);
          El.Subscripts.push_back(IdxSCEV);
        }

        LLVM_DEBUG(
          dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE]Inst: ";
        I.dump();
        dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE]Idxs: " << Idxs.size() << "\n";
        for (auto *Idx : Idxs) {
          dbgs() << "\t";
          Idx->dump();
          auto *IdxSCEV = SE.getSCEV(Idx);
          dbgs() << "\t\t";
          IdxSCEV->dump();
        }
        );
      };

      for_each_memory(I, TLI,
        [&ProcessAccessInstruction] (Instruction &I, MemoryLocation,
          unsigned,  AccessInfo IsRead, AccessInfo IsWrite) {
          if (IsRead != AccessInfo::No || IsWrite != AccessInfo::No) {
            ProcessAccessInstruction(I);
          }
        },
        [&ProcessAccessInstruction](Instruction &I, AccessInfo IsRead,
          AccessInfo IsWrite) {
          if (IsRead != AccessInfo::No || IsWrite != AccessInfo::No) {
            ProcessAccessInstruction(I);
          }
        }
      );
    }
  }
}

void fillArrayDimensionsSizes(SmallVectorImpl<int64_t> &Dimensions, ScalarEvolution &SE,
  Array *CurrentArray) {
  assert(CurrentArray && "Current Array must not be null");
  assert(!CurrentArray->empty() && "Acesses size must not be zero");

  if (Dimensions.empty()) {
    size_t DimensionsCount = CurrentArray->getElement(0)->Subscripts.size();

    for (int i = 1; i < CurrentArray->size(); ++i) {
      if (CurrentArray->getElement(0)->Subscripts.size() != DimensionsCount) {
        //CurrentArray->setValid(false);
        LLVM_DEBUG(
          dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE]Array " <<
            CurrentArray->getBase()->getName() << " is unrealible\n";
        );
        return;
      }
    }

    Dimensions.resize(DimensionsCount);
    for (int i = 0; i < DimensionsCount; ++i) {
      Dimensions[i] = -1;
    }
  }

  //Fill const dimensions sizes
  CurrentArray->setNumberOfDims(Dimensions.size());

  Type *Ty = CurrentArray->getElement(0)->Subscripts[0]->getType();

  size_t LastConstDimension = Dimensions.size();
  //Find last (from left to right) dimension with constant size, extreme left is always unknown
  for (int i = Dimensions.size() - 1; i >= 0; --i) {
    if (Dimensions[i] > 0) {
      LastConstDimension = i;
      CurrentArray->setDimSize(i, SE.getConstant(Ty, Dimensions[i], false));
      LLVM_DEBUG(
        dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Set dim size ";
        CurrentArray->getDimSize(i)->dump();
      );
    } else {
      break;
    }
  }


  if (Dimensions[0] > 0) {
    if (LastConstDimension != 0) {
      CurrentArray->setDimSize(0, SE.getConstant(Ty, Dimensions[0], false));
      LLVM_DEBUG(
        dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Set first dim size ";
        CurrentArray->getDimSize(0)->dump();
      );
    }
  } else {
    CurrentArray->setDimSize(0, SE.getCouldNotCompute());
    LLVM_DEBUG(
      dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Set first dim size ";
      CurrentArray->getDimSize(0)->dump();
    );
  }

  //if (Dimensions[0] < 0) {
  //  CurrentArray->setDimSize(0, SE.getCouldNotCompute());
  //}

  size_t FirstUnknownDimension = LastConstDimension - 1;

  LLVM_DEBUG(
    if (LastConstDimension < Dimensions.size()) {
      dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Filling array const dims from " <<
        LastConstDimension << " to " << Dimensions.size() - 1 << "\n";
    }
  );

  if (FirstUnknownDimension <= 0) {
    return;
  }

  LLVM_DEBUG(
    dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Start filling\n";
  );

  auto *PrevioslyDimsSizesProduct = SE.getConstant(Ty, 1, false);

  for (int i = FirstUnknownDimension - 1; i >= 0; --i) {
    LLVM_DEBUG(
      dbgs() << "i: " << i << "\n";
    );
    const SCEV *Q, *R, *DimSize;

    //Find divider (dimension size), switch by constant or variable
    if (Dimensions[i + 1] > 0) {
      LLVM_DEBUG(
        dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Const dim size\n";
      );
      DimSize = SE.getConstant(Ty, Dimensions[i + 1], false);
    } else {
      LLVM_DEBUG(
        dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Var dim size\n";
      );
      SmallVector<const SCEV *, 3> Expressions;
      for (auto CurrentElementIt = CurrentArray->begin();
        CurrentElementIt != CurrentArray->end(); ++CurrentElementIt) {
        if (CurrentElementIt->Subscripts.size() != Dimensions.size()) {
          CurrentElementIt->IsValid = false;
        } else {
          LLVM_DEBUG(
            dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Expressions\n";
          );
          for (int j = i; j >= 0; --j) {
            Expressions.push_back(CurrentElementIt->Subscripts[j]);
            LLVM_DEBUG(
              dbgs() << "\t";
              CurrentElementIt->Subscripts[j]->dump();
            );
          }
        }
      }
      DimSize = findGCD(Expressions, SE);
      LLVM_DEBUG(
        dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] GCD: ";
        DimSize->dump();
      );

      SCEVDivision::divide(SE, DimSize, PrevioslyDimsSizesProduct, &Q, &R);
      LLVM_DEBUG(
        dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] PrevioslyDimsSizesProduct: ";
        PrevioslyDimsSizesProduct->dump();
        dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Q: ";
        Q->dump();
        dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] R: ";
        R->dump();
      );
      if (R->isZero()) {
        DimSize = Q;
      } else {
        DimSize = SE.getConstant(Ty, 1, false);
        //CurrentArray->setValid(false);
        assert(false && "Cant divide dim size");
        break;
      }
    }
    if (DimSize->isZero()) {
      DimSize = SE.getCouldNotCompute();
      //CurrentArray->setValid(false);
    } else {
      PrevioslyDimsSizesProduct = SE.getMulExpr(PrevioslyDimsSizesProduct, DimSize);
    }

    CurrentArray->setDimSize(i + 1, DimSize);
    LLVM_DEBUG(
      dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Set dim size ";
      DimSize->dump();
    );
  }
}

void cleanSubscripts(Array *CurrentArray, ScalarEvolution &SE) {
  assert(CurrentArray && "Current Array must not be null");
  assert(!CurrentArray->empty() && "Acesses size must not be zero");

  size_t LastConstDimension = CurrentArray->getNumberOfDims();
  //find last (from left to right) dimension with constant size, extreme left is always unknown
  for (int i = CurrentArray->getNumberOfDims() - 1; i > 0; --i) {
    if (isa<SCEVConstant>(CurrentArray->getDimSize(i))) {
      LastConstDimension = i;
    } else {
      break;
    }
  }

  size_t FirstUnknownDimension = LastConstDimension - 1;

  const SCEV *Q, *R;

  Type *Ty = CurrentArray->getElement(0)->Subscripts[0]->getType();
  //LLVM_DEBUG(
  //  dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] SCEV Type: ";
  //  Ty->dump();
  //);
  auto *PrivioslyDimsSizesProduct = SE.getConstant(Ty, 1, true);

  for (int i = FirstUnknownDimension - 1; i >= 0; --i) {
    if (dyn_cast<SCEVCouldNotCompute>(CurrentArray->getDimSize(i + 1))) {
      continue;
    }
    PrivioslyDimsSizesProduct = SE.getMulExpr(PrivioslyDimsSizesProduct,
      CurrentArray->getDimSize(i + 1));
    for (auto CurrentElementIt = CurrentArray->begin();
      CurrentElementIt != CurrentArray->end();
      ++CurrentElementIt) {
      auto *CurrentSCEV = CurrentElementIt->Subscripts[i];
        SCEVDivision::divide(SE, CurrentSCEV, PrivioslyDimsSizesProduct, &Q, &R);
        LLVM_DEBUG(
          dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] SCEV: ";
          CurrentSCEV->dump();
          dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Divider: ";
          PrivioslyDimsSizesProduct->dump();
          dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] R: ";
          R->dump();
          dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Q: ";
          Q->dump();
        );
        if (R->isZero()) {
          CurrentSCEV = Q;
        } else {
          assert(false && "Cant divide access");
        }
      LLVM_DEBUG(
        dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Set ";
        CurrentSCEV->dump();
      );
      CurrentElementIt->Subscripts[i] = CurrentSCEV;
    }
  }
}

//TODO �������� ����� isValid ��� �������� ���� ������ ���������� ���������
void findSubscripts(DelinearizeInfo::ArraySet &AnalyzedArrays, Function &F,
  ScalarEvolution &SE, TargetLibraryInfo &TLI) {
  DenseMap<Value *, SmallVector <int64_t, 3> > CachedArraysDbgInfo;
  collectArrays(F, SE, TLI, CachedArraysDbgInfo, AnalyzedArrays);

  for (auto &CurrentArray : AnalyzedArrays) {
    SmallVector<int64_t, 3> Dimensions;
    auto ArrayDbgInfoCache = CachedArraysDbgInfo.find(CurrentArray.getBase());
    if (ArrayDbgInfoCache != CachedArraysDbgInfo.end()) {
      Dimensions = ArrayDbgInfoCache->second;
    } else {
      findArrayDimesionsFromDbgInfo(CurrentArray.getBase(), Dimensions);
      CachedArraysDbgInfo.insert(std::make_pair(CurrentArray.getBase(), Dimensions));
    }
    fillArrayDimensionsSizes(Dimensions, SE, &CurrentArray);
    if (CurrentArray.isValid()) {
      cleanSubscripts(&CurrentArray, SE);
    } else {
      LLVM_DEBUG(
        dbgs() << "[ARRAY SUBSCRIPT DELINEARIZE] Array Dimensions is empty";
      );
    }
  }
}

#ifdef LLVM_DEBUG
void delinearizationLog(const DelinearizeInfo &Info, ScalarEvolution &SE,
    raw_ostream  &OS) {
  for (auto &ArrayEntry : Info.getArrays()) {
    OS << "[DELINEARIZATION]: results for array ";
    ArrayEntry.getBase()->print(OS, true);
    OS << "\n";
    OS << "  number of dimensions: " << ArrayEntry.getNumberOfDims() << "\n";
    for (std::size_t I = 0, EI = ArrayEntry.getNumberOfDims(); I < EI; ++I) {
      OS << "    " << I << ": ";
      ArrayEntry.getDimSize(I)->print(OS);
      OS << "\n";
    }
    OS << "  accesses:\n";
    for (auto &El : ArrayEntry) {
      OS << "    address: ";
      El.Ptr->print(OS, true);
      OS << "\n";
      for (auto *S : El.Subscripts) {
        OS << "      SCEV: ";
        S->print(OS);
        OS << "\n";
        const SCEV *Coef, *ConstTerm;
        std::tie(Coef, ConstTerm) = findCoefficientsInSCEV(S, SE);
        OS << "      a: ";
        if (Coef)
          Coef->print(OS);
        else
          OS << "null";
        OS << "\n";
        OS << "      b: ";
        if (ConstTerm)
          ConstTerm->print(OS);
        else
          OS << "null";
        OS << "\n";
      }
    }
  }
}
#endif
}

bool ArraySubscriptDelinearizePass::runOnFunction(Function &F) {
  LLVM_DEBUG(
    dbgs() << "[DELINEARIZATION]: process function " << F.getName() << "\n");
  releaseMemory();
  auto &SE = getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
  findSubscripts(mDelinearizeInfo.getArrays(), F, SE, TLI);
  mDelinearizeInfo.fillElementsMap();
  LLVM_DEBUG(delinearizationLog(mDelinearizeInfo, SE, dbgs()));
  return false;
}

void ArraySubscriptDelinearizePass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<ScalarEvolutionWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.setPreservesAll();
}

void ArraySubscriptDelinearizePass::print(raw_ostream &OS, const Module *M) const {
  auto &SE = getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  RawDelinearizeInfo Info = tsar::toJSON(mDelinearizeInfo, SE);
  OS << json::Parser<RawDelinearizeInfo>::unparse(Info) << '\n';
}

FunctionPass * createArraySubscriptDelinearizePass() {
  return new ArraySubscriptDelinearizePass;
}

RawDelinearizeInfo tsar::toJSON(const DelinearizeInfo &Info, ScalarEvolution &SE) {
  RawDelinearizeInfo RawInfo;
  for (auto &ArrayEntry : Info.getArrays()) {
    std::string NameStr;
    raw_string_ostream NameOS(NameStr);
    NameOS.flush();
    ArrayEntry.getBase()->print(NameOS);
    std::vector<std::string> DimSizes(ArrayEntry.getNumberOfDims());
    for (std::size_t I = 0, EI = DimSizes.size(); I < EI; ++I) {
      raw_string_ostream DimSizeOS(DimSizes[I]);
      ArrayEntry.getDimSize(I)->print(DimSizeOS);
      DimSizeOS.flush();
    }
    std::vector<std::vector<std::vector<std::string>>> Accesses;
    for (auto &El : ArrayEntry) {
      std::vector<std::vector<std::string>> Subscripts;
      for (auto *S : El.Subscripts) {
        Subscripts.emplace_back(2);
        auto &CoefStr = Subscripts.back().front();
        auto &ConstTermStr = Subscripts.back().back();
        const SCEV *Coef, *ConstTerm;
        std::tie(Coef, ConstTerm) = findCoefficientsInSCEV(S, SE);
        if (Coef) {
          raw_string_ostream CoefOS(CoefStr);
          Coef->print(CoefOS);
          CoefOS.flush();
        } else {
          CoefStr = "null";
        }
        if (ConstTerm) {
          raw_string_ostream ConstTermOS(ConstTermStr);
          ConstTerm->print(ConstTermOS);
          ConstTermOS.flush();
        } else {
          ConstTermStr = "null";
        }
      }
      Accesses.push_back(std::move(Subscripts));
    }
    RawInfo[RawDelinearizeInfo::Sizes].emplace(NameStr, std::move(DimSizes));
    RawInfo[RawDelinearizeInfo::Accesses].emplace(
      std::move(NameStr), std::move(Accesses));
  }
  return RawInfo;
}

