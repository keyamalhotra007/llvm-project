//===- ArgumentValueSpecializationCost.h -----------------------*- C++ -*-===//
//
//===----------------------------------------------------------------------===//
//
// Cost model for PEBS-guided argument value specialization.
//
// Adapted from llvm/lib/Transforms/IPO/FunctionSpecialization.cpp
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_IPO_ARGUMENTVALUESPECIALIZATIONCOST_H
#define LLVM_TRANSFORMS_IPO_ARGUMENTVALUESPECIALIZATIONCOST_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstVisitor.h"
#include "llvm/Support/Compiler.h"
#include <cstdint>

namespace llvm {

// Just a shorter abbreviation to improve indentation.
using Cost = InstructionCost;

// Map of known constants found during the specialization bonus estimation.
using ConstMap = DenseMap<Value *, Constant *>;

struct ArgCandidate {
  uint64_t Value;
  unsigned Percentage;
};

/// One argument's constant substitution for a specialization candidate.
  struct ArgSubstitution {
    Argument *Arg;
    Constant *C;
  };

/// Tracks accumulated clone-codesize growth per original callee, so that
/// repeated specializations of the same function (for different call sites)
/// are charged against a shared budget rather than each being evaluated in
/// isolation.
using GrowthMap = DenseMap<Function *, unsigned>;

/// Whole-function codesize cost, summed via TargetTransformInfo.
LLVM_ABI unsigned estimateFunctionCodeSize(Function &F,
                                           TargetTransformInfo &TTI);

/// Structural eligibility checks, adapted from
/// FunctionSpecializer::isCandidateFunction().
LLVM_ABI bool isEligibleForSpecialization(Function &F);

/// Call after committing to a specialization, to charge its size against the
/// callee's growth budget for subsequent candidates.
/// Call after committing to a specialization, to charge its size against the
/// callee's growth budget for subsequent candidates.
LLVM_ABI void chargeGrowth(Function &Callee, TargetTransformInfo &TTI,
                            GrowthMap &Growth, Cost CodeSizeSavings);

/// Score a candidate specialization: either a single hot argument, or a
/// combination of several hot arguments at the same call site. Higher is
/// better. Returns 0 if the hard size/growth gates are violated (i.e. this
/// candidate must not be specialized at all).
unsigned computeSpecializationScore(Function &Callee,
                                     ArrayRef<ArgSubstitution> Substitutions,
                                     double JointPercentBound, double CallFreq,
                                     std::function<BlockFrequencyInfo &(Function &)> GetBFI,
                                     TargetTransformInfo &TTI, GrowthMap &Growth,
                                     Cost *OutCodeSizeSavings = nullptr);
                                     

class ArgSpecCostVisitor : public InstVisitor<ArgSpecCostVisitor, Constant *> {
  std::function<BlockFrequencyInfo &(Function &)> GetBFI;
  Function *F;
  const DataLayout &DL;
  TargetTransformInfo &TTI;

  // Fresh per candidate.
  ConstMap KnownConstants; // substitutions

  // Basic blocks known to be unreachable after constant propagation.
  DenseSet<BasicBlock *> DeadBlocks;

  // PHI nodes we have visited before.
  DenseSet<PHINode *> VisitedPHIs;

  // PHI nodes we have visited once without successfully constant folding
  // them. Once every specialization argument has been processed, it should
  // be possible to determine whether those PHIs can be folded (some of
  // their incoming values may have become constant or dead).
  SmallVector<Instruction *> PendingPHIs;

  ConstMap::iterator LastVisited;

public:
  ArgSpecCostVisitor(std::function<BlockFrequencyInfo &(Function &)> GetBFI,
                     Function *F, const DataLayout &DL,
                     TargetTransformInfo &TTI)
      : GetBFI(GetBFI), F(F), DL(DL), TTI(TTI) {}

  bool isBlockExecutable(BasicBlock *BB) const {
    return !DeadBlocks.contains(BB);
  }

  // for a candidate argument A being replaced by constant C
  // it walks every user instruction of A that lives in a currently-executable block 
  // and recursively computes the cumulative code-size savings.
  LLVM_ABI Cost getCodeSizeSavingsForArg(Argument *A, Constant *C);

  LLVM_ABI Cost getCodeSizeSavingsFromPendingPHIs();

  // runs after all code-size analysis is done
  // and computes the weighted latency savings for every instruction that ended up in KnownConstants. 
  // It weights each instruction's latency cost by its relative execution frequency, obtained from BlockFrequencyInfo,
  // so instructions in hot paths matter more than those in cold paths.
  LLVM_ABI Cost getLatencySavingsForKnownConstants();



private:
  friend class InstVisitor<ArgSpecCostVisitor, Constant *>;

  // if V is already a literal Constant in the IR, return it directly; otherwise look it up in KnownConstants
  Constant *findConstantFor(Value *V) const;

  // checks whether a control-flow successor block becomes provably unreachable: it's true only 
  // 1. pred = BB (block whose terminator got folded to a constant)
  // 2. pred = Succ (a self-loop)
  // 3. already known non-executable 
  bool canEliminateSuccessor(BasicBlock *BB, BasicBlock *Succ) const;


  // Skips instructions already resolved 
  // Special-cases SwitchInst and BranchInst because folding a branch condition doesn't produce a new constant value, it produces dead basic blocks
  // For every other instruction type, dispatches to the appropriate visit* method (via the InstVisitor mechanism) to attempt constant folding.
  // If folding fails, it stops recursing down that path. An unfolded instruction blocks further propagation.
  // If it succeeds, it records the instruction's own code-size cost as "saved" (since it will disappear after optimizations), caches the result in KnownConstants, and recurses into that instruction's users, continuing the chain of constant propagation.
  Cost getCodeSizeSavingsForUser(Instruction *User, Value *Use = nullptr,
                                 Constant *C = nullptr);

  // does a work-list traversal starting from newly-dead blocks: 
  // for each dead block, it sums the code-size cost of its instructions
  // then propagates deadness transitively to successors that also become unreachable via canEliminateSuccessor. 
  Cost estimateBasicBlocks(SmallVectorImpl<BasicBlock *> &WorkList);
  Cost estimateSwitchInst(SwitchInst &I);
  Cost estimateBranchInst(BranchInst &I);

  // Transitively Incoming Values (TIV) is a set of Values that can "feed" a
  // value to the initial PHI-node. It is defined like this:
  //
  // * the initial PHI-node belongs to TIV.
  //
  // * for every PHI-node in TIV, its operands belong to TIV
  //
  // If TIV for the initial PHI-node (P) contains more than one constant or a
  // value that is not a PHI-node, then P cannot be folded to a constant.
  //
  // As soon as we detect these cases, we bail, without constructing the
  // full TIV. Otherwise P can be folded to the one constant in TIV.
  bool discoverTransitivelyIncomingValues(Constant *Const, PHINode *Root,
                                          DenseSet<PHINode *> &TransitivePHIs);

  Constant *visitInstruction(Instruction &I) { return nullptr; }
  Constant *visitPHINode(PHINode &I);
  Constant *visitFreezeInst(FreezeInst &I);
  Constant *visitCallBase(CallBase &I);
  Constant *visitLoadInst(LoadInst &I);
  Constant *visitGetElementPtrInst(GetElementPtrInst &I);
  Constant *visitSelectInst(SelectInst &I);
  Constant *visitCastInst(CastInst &I);
  Constant *visitCmpInst(CmpInst &I);
  Constant *visitUnaryOperator(UnaryOperator &I);
  Constant *visitBinaryOperator(BinaryOperator &I);
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_IPO_ARGUMENTVALUESPECIALIZATIONCOST_H