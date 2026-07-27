//===- ArgumentValueSpecializationCost.cpp -------------------------------===//
//
// ArgumentValueSpecializationCost.h has descriptions of data structures.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/ArgumentValueSpecializationCost.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

static cl::opt<unsigned> ArgSpecHotnessThreshold(
    "argspec-hotness-threshold", cl::init(90), cl::Hidden,
    cl::desc("Minimum hotness percentage required for an "
             "argument value candidate to be considered for specialization"));

static cl::opt<unsigned> ArgSpecMaxCodeSizeGrowth(
    "argspec-max-codesize-growth", cl::init(2), cl::Hidden,
    cl::desc("Maximum cumulative codesize growth (as a multiple of the "
             "original function size) allowed per callee across all of its "
             "specializations"));

static cl::opt<unsigned> ArgSpecMaxAbsoluteSize(
    "argspec-max-absolute-size", cl::init(500), cl::Hidden,
    cl::desc("Cap on callee instruction count; never specialize a "
             "function larger than this regardless of hotness"));

unsigned llvm::estimateFunctionCodeSize(Function &F, TargetTransformInfo &TTI) {
  InstructionCost Cost = 0;
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      Cost += TTI.getInstructionCost(&I, TargetTransformInfo::TCK_CodeSize);

  if (!Cost.isValid())
    return ArgSpecMaxAbsoluteSize + 1; // treat as too big to specialize

  int64_t Value = Cost.getValue();
  assert(Value >= 0 && "CodeSize and Latency cannot be negative");
  return static_cast<unsigned>(Value);
}

bool llvm::isEligibleForSpecialization(Function &F) {
  if (F.isDeclaration())
    return false;

  // Duplicating this function is explicitly disallowed.
  if (F.hasFnAttribute(Attribute::NoDuplicate))
    return false;

  // It will get inlined into every caller anyways so specializing first is wasted work.
  if (F.hasFnAttribute(Attribute::AlwaysInline))
    return false;

  return true;
}


void llvm::chargeGrowth(Function &Callee, TargetTransformInfo &TTI,
                        GrowthMap &Growth) {
  Growth[&Callee] += estimateFunctionCodeSize(Callee, TTI);
}

// hotter combination on a smaller function with less prior growth gets higher score.
unsigned llvm::computeSpecializationScore(Function &Callee,
                                          uint8_t Percentage,
                                          TargetTransformInfo &TTI,
                                          GrowthMap &Growth) {
 
  if (!isEligibleForSpecialization(Callee))
    return 0;

  if (Percentage < ArgSpecHotnessThreshold)
    return 0;
 
  unsigned FuncSize = estimateFunctionCodeSize(Callee, TTI);
  if (FuncSize == 0 || FuncSize > ArgSpecMaxAbsoluteSize)
    return 0;
 
  unsigned PriorGrowth = Growth.lookup(&Callee);
  if ((PriorGrowth + FuncSize) / FuncSize > ArgSpecMaxCodeSizeGrowth)
    return 0;
 
 
  // --- Score: higher combined hotness and smaller (less-already-grown) callee both increase the score.
  //
  // Dividing by (FuncSize + PriorGrowth) rather than just FuncSize means a
  // callee that has already had other specializations charged against it
  // scores lower for further specialization - all else equal, prefer
  // spending your growth budget on callees you haven't already spent it on.

  uint64_t SizePenalty = FuncSize + PriorGrowth;
  uint64_t Score = (Percentage * 100) / SizePenalty;
 
  return Score == 0 ? 1 : static_cast<unsigned>(Score);
}

// Work in Progress 
Cost ArgSpecCostVisitor::getCodeSizeSavingsForUser(Instruction *User, Value *Use,
                                                Constant *C) {
  // We have already propagated a constant for this user.
  if (KnownConstants.contains(User))
    return 0;

  // Cache the iterator before visiting.
  LastVisited = Use ? KnownConstants.insert({Use, C}).first
                    : KnownConstants.end();

  Cost CodeSize = 0;
  if (auto *I = dyn_cast<SwitchInst>(User)) {
    CodeSize = estimateSwitchInst(*I); // special-cased: doesn't return a Constant
  } else if (auto *I = dyn_cast<BranchInst>(User)) {
    CodeSize = estimateBranchInst(*I); // special-cased: doesn't return a Constant
  } else {
    C = visit(*User); // generic InstVisitor dispatch
    if (!C) // didn't fold -> stop, don't recurse further
      return 0;
  }

  // Even though it doesn't make sense to bind switch and branch instructions
  // with a constant, unlike any other instruction type, it prevents estimating
  // their bonus multiple times.
  KnownConstants.insert({User, C});

  CodeSize += TTI.getInstructionCost(User, TargetTransformInfo::TCK_CodeSize);

  LLVM_DEBUG(dbgs() << "ArgSpecialization:     {CodeSize = " << CodeSize
                    << "} for user " << *User << "\n");

  for (auto *U : User->users())
    if (auto *UI = dyn_cast<Instruction>(U))
      if (UI != User && isBlockExecutable(UI->getParent()))
        CodeSize += getCodeSizeSavingsForUser(UI, User, C);

  return CodeSize;
}

Cost ArgSpecCostVisitor::getCodeSizeSavingsForArg(Argument *A, Constant *C) {
  for (auto *U : A->users())
    if (auto *UI = dyn_cast<Instruction>(U))
      if (isBlockExecutable(UI->getParent()))
        CodeSize += getCodeSizeSavingsForUser(UI, A, C);
  return CodeSize;
}

Constant *ArgSpecCostVisitor::findConstantFor(Value *V) const {
  if (auto *C = dyn_cast<Constant>(V))
    return C;                         // literal (e.g. a ConstantInt already in the IR)
  return KnownConstants.lookup(V);    // our own substitution map
}

bool ArgSpecCostVisitor::canEliminateSuccessor(BasicBlock *BB,
                                            BasicBlock *Succ) const {
  unsigned I = 0;
  return all_of(predecessors(Succ), [&I, BB, Succ, this](BasicBlock *Pred) {
    return I++ < MaxBlockPredecessors &&
           (Pred == BB || Pred == Succ || !isBlockExecutable(Pred));
  });
}

Cost ArgSpecCostVisitor::estimateBasicBlocks(SmallVectorImpl<BasicBlock*> &WorkList) {
  Cost CodeSize = 0;
  while (!WorkList.empty()) {
    BasicBlock *BB = WorkList.pop_back_val();
    if (!DeadBlocks.insert(BB).second) continue;   // already counted

    for (Instruction &I : *BB) {
      if (KnownConstants.contains(&I)) continue;    // don't double count already-folded insts
      CodeSize += TTI.getInstructionCost(&I, TCK_CodeSize);
    }

    for (BasicBlock *SuccBB : successors(BB))
      if (isBlockExecutable(SuccBB) && canEliminateSuccessor(BB, SuccBB))
        WorkList.push_back(SuccBB);   // propagate deadness transitively
  }
  return CodeSize;
}