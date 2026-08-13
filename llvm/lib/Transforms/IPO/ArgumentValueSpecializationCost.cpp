//===- ArgumentValueSpecializationCost.cpp -------------------------------===//
//
// ArgumentValueSpecializationCost.h has descriptions of data structures.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/ArgumentValueSpecializationCost.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/ValueTracking.h"

#define DEBUG_TYPE "argspec"



using namespace llvm;

static cl::opt<unsigned> ArgSpecHotnessThreshold(
    "argspec-hotness-threshold", cl::init(50), cl::Hidden,
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

static cl::opt<unsigned> MaxBlockPredecessors(
    "argspec-max-block-predecessors", cl::init(2), cl::Hidden, cl::desc(
    "The maximum number of predecessors a basic block can have to be "
    "considered during the estimation of dead code"));

static cl::opt<unsigned> MaxIncomingPhiValues(
    "argspec-max-incoming-phi-values", cl::init(8), cl::Hidden,
    cl::desc("The maximum number of incoming values a PHI node can have to be "
             "considered during the specialization bonus estimation"));

static cl::opt<unsigned> MaxDiscoveryIterations(
    "argspec-max-discovery-iterations", cl::init(100),
                        cl::Hidden,
                        cl::desc("The maximum number of iterations allowed "
                                "when searching for transitive "
                                "phis"));

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

// Arguments:
// Callee: function being considered for specialization.
// Substitutions: combination of arguments being fixed to constants
// JointPercentBound: the estimated percentage of call sites where all args in this combo simultaneously hold these constant values
// CallFreq: how often the callee is invoked overall 
// GetBFI: to fetch BlockFrequencyInfo for a function
// TTI: used to price individual instructions
// Growth: running map of already-committed code growth per function
// OutCodeSizeSavings: output parameter to hand back the raw size-savings estimate

unsigned llvm::computeSpecializationScore(
    Function &Callee, ArrayRef<ArgSubstitution> Substitutions,
    double JointPercentBound, double CallFreq,
    std::function<BlockFrequencyInfo &(Function &)> GetBFI,
    TargetTransformInfo &TTI, GrowthMap &Growth, Cost *OutCodeSizeSavings) {

  if (!isEligibleForSpecialization(Callee))
    return 0;

  const DataLayout &DL = Callee.getParent()->getDataLayout();
  // Fresh visitor per candidate - KnownConstants, DeadBlocks,
  // VisitedPHIs, PendingPHIs all accumulate across the calls
  ArgSpecCostVisitor Visitor(GetBFI, &Callee, DL, TTI);

  Cost CodeSizeSavings = 0;
  for (const ArgSubstitution &Sub : Substitutions) 
    CodeSizeSavings += Visitor.getCodeSizeSavingsForArg(Sub.Arg, Sub.C); //compute the code-size savings from replacing that argument with its proposed constant

  CodeSizeSavings += Visitor.getCodeSizeSavingsFromPendingPHIs();

  Cost LatencySavings = Visitor.getLatencySavingsForKnownConstants();

  if (!CodeSizeSavings.isValid() || !LatencySavings.isValid())
    return 0; // TTI declined to cost something involved - treat as unscoreable

  int64_t SizeSaved = CodeSizeSavings.getValue();
  int64_t LatSaved = LatencySavings.getValue();
  if (SizeSaved <= 0 && LatSaved <= 0)
    return 0; // no benefit at all, reject before touching the growth budget

  // Hard growth gate. This is a *peek*, not a charge: Growth is only
  // mutated by chargeGrowth() in Pass 2, on the actual winning combo.
  unsigned FnSize = estimateFunctionCodeSize(Callee, TTI);
  unsigned ProspectiveGrowth =
      (SizeSaved > 0 && (unsigned)SizeSaved < FnSize) ? FnSize - SizeSaved : FnSize;
  unsigned CommittedGrowth = Growth.lookup(&Callee);

  constexpr unsigned GrowthBudgetMultiplier = 3; // TODO: tune after real perf data
  if (CommittedGrowth + ProspectiveGrowth > GrowthBudgetMultiplier * FnSize)
    return 0;

  constexpr double LatencyWeight = 1.0;      // TODO: tune
  constexpr double GuardOverheadWeight = 1.0; // TODO: tune

  double Benefit = (double(SizeSaved) + LatencyWeight * double(LatSaved)) *
                    (JointPercentBound / 100.0) * CallFreq;
  // A combo of N args needs an N-way ANDed guard; each extra compare adds
  // miss-branch overhead on the (100-JointPercentBound)% of calls that don't
  // match. Scale by combo size so gratuitously large combos need a clearly
  // bigger joint win to be worth it.
  double GuardCost = GuardOverheadWeight *
                      (1.0 - JointPercentBound / 100.0) * Substitutions.size();

  double Score = Benefit - GuardCost;
  if (Score <= 0.0)
    return 0;

  if (OutCodeSizeSavings)
    *OutCodeSizeSavings = CodeSizeSavings;
  return static_cast<unsigned>(Score);
}



void llvm::chargeGrowth(Function &Callee, TargetTransformInfo &TTI,
                         GrowthMap &Growth, Cost CodeSizeSavings) {
  unsigned FullSize = estimateFunctionCodeSize(Callee, TTI);
  unsigned NetSize = CodeSizeSavings.isValid() && CodeSizeSavings.getValue() < FullSize
                          ? FullSize - static_cast<unsigned>(CodeSizeSavings.getValue())
                          : FullSize;
  Growth[&Callee] += NetSize;
}


Cost ArgSpecCostVisitor::getCodeSizeSavingsForArg(Argument *A, Constant *C) {
  Cost CodeSize = 0;
  for (auto *U : A->users())
    if (auto *UI = dyn_cast<Instruction>(U))
      if (isBlockExecutable(UI->getParent()))
        CodeSize += getCodeSizeSavingsForUser(UI, A, C);
  return CodeSize;
}

Cost ArgSpecCostVisitor::getCodeSizeSavingsForUser(Instruction *User, Value *Use,
                                                Constant *C) {
  // We have already propagated a constant for this user.
  if (KnownConstants.contains(User))
    return 0;

  // Cache the iterator before visiting.
  LastVisited = Use ? KnownConstants.insert({Use, C}).first // inserts the pair (Use, C) into the KnownConstants map. .first grabs just the iterator half of that pair
                    : KnownConstants.end();

  Cost CodeSize = 0;
  if (auto *I = dyn_cast<SwitchInst>(User)) {
    CodeSize = estimateSwitchInst(*I); // special-cased: doesn't return a Constant
  } else if (auto *I = dyn_cast<BranchInst>(User)) {
    CodeSize = estimateBranchInst(*I); // special-cased: doesn't return a Constant
  } else {
    C = visit(*User); // InstVisitor dispatch
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

Cost ArgSpecCostVisitor::getCodeSizeSavingsFromPendingPHIs() {
  Cost CodeSize;
  while (!PendingPHIs.empty()) {
    Instruction *Phi = PendingPHIs.pop_back_val();
    // The pending PHIs could have been proven dead by now.
    if (isBlockExecutable(Phi->getParent()))
      CodeSize += getCodeSizeSavingsForUser(Phi);
  }
  return CodeSize;
}

/// Compute the latency savings from replacing all arguments with constants for
/// a specialization candidate. As this function computes the latency savings
/// for all Instructions in KnownConstants at once, it should be called only
/// after every instruction has been visited, i.e. after:
///
/// * getCodeSizeSavingsForArg has been run for every constant argument of a
///   specialization candidate
///
/// * getCodeSizeSavingsFromPendingPHIs has been run
///
/// to ensure that the latency savings are calculated for all Instructions we
/// have visited and found to be constant.
Cost ArgSpecCostVisitor::getLatencySavingsForKnownConstants() {
  auto &BFI = GetBFI(*F);
  Cost TotalLatency = 0;

  for (auto Pair : KnownConstants) {
    Instruction *I = dyn_cast<Instruction>(Pair.first);
    if (!I)
      continue;

    uint64_t Weight = BFI.getBlockFreq(I->getParent()).getFrequency() /
                      BFI.getEntryFreq().getFrequency();

    Cost Latency =
        Weight * TTI.getInstructionCost(I, TargetTransformInfo::TCK_Latency);

    LLVM_DEBUG(dbgs() << "ArgSpecialization:     {Latency = " << Latency
                      << "} for instruction " << *I << "\n");

    TotalLatency += Latency;
  }

  return TotalLatency;
}

Constant *ArgSpecCostVisitor::findConstantFor(Value *V) const {
  if (auto *C = dyn_cast<Constant>(V)) 
    return C;                         // literal (e.g. a ConstantInt already in the IR)
  return KnownConstants.lookup(V);    // visitor has already resolved to a constant
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
      CodeSize += TTI.getInstructionCost(&I, TargetTransformInfo::TCK_CodeSize);
    }

    for (BasicBlock *SuccBB : successors(BB))
      if (isBlockExecutable(SuccBB) && canEliminateSuccessor(BB, SuccBB))
        WorkList.push_back(SuccBB);   // propagate deadness transitively
  }
  return CodeSize;
}

//visit functions

Constant *ArgSpecCostVisitor::visitCmpInst(CmpInst &I) {
  assert(LastVisited != KnownConstants.end() && "Invalid iterator!");

  Constant *Const = LastVisited->second;
  bool ConstOnRHS = I.getOperand(1) == LastVisited->first;
  Value *V = ConstOnRHS ? I.getOperand(0) : I.getOperand(1);
  Constant *Other = findConstantFor(V);

  if (!Other)
    return nullptr;

  if (ConstOnRHS)
    std::swap(Const, Other);
  return ConstantFoldCompareInstOperands(I.getPredicate(), Const, Other, DL);
}

Constant *ArgSpecCostVisitor::visitBinaryOperator(BinaryOperator &I) {
  assert(LastVisited != KnownConstants.end() && "Invalid iterator!");

  bool ConstOnRHS = I.getOperand(1) == LastVisited->first;
  Value *V = ConstOnRHS ? I.getOperand(0) : I.getOperand(1);
  Constant *Other = findConstantFor(V);
  Value *OtherVal = Other ? Other : V;
  Value *ConstVal = LastVisited->second;

  if (ConstOnRHS)
    std::swap(ConstVal, OtherVal);

  return dyn_cast_or_null<Constant>(
      simplifyBinOp(I.getOpcode(), ConstVal, OtherVal, SimplifyQuery(DL)));
}

Constant *ArgSpecCostVisitor::visitGetElementPtrInst(GetElementPtrInst &I) {
  SmallVector<Constant *, 8> Operands;
  Operands.reserve(I.getNumOperands());

  for (unsigned Idx = 0, E = I.getNumOperands(); Idx != E; ++Idx) {
    Value *V = I.getOperand(Idx);
    Constant *C = findConstantFor(V);
    if (!C)
      return nullptr;
    Operands.push_back(C);
  }

  auto Ops = ArrayRef(Operands.begin(), Operands.end());
  return ConstantFoldInstOperands(&I, Ops, DL);
}

Constant *ArgSpecCostVisitor::visitSelectInst(SelectInst &I) {
  assert(LastVisited != KnownConstants.end() && "Invalid iterator!");

  if (I.getCondition() == LastVisited->first) {
    Value *V = LastVisited->second->isNullValue() ? I.getFalseValue()
                                                  : I.getTrueValue();
    return findConstantFor(V);
  }
  if (Constant *Condition = findConstantFor(I.getCondition()))
    if ((I.getTrueValue() == LastVisited->first && Condition->isOneValue()) ||
        (I.getFalseValue() == LastVisited->first && Condition->isNullValue()))
      return LastVisited->second;
  return nullptr;
}

Constant *ArgSpecCostVisitor::visitCastInst(CastInst &I) {
  return ConstantFoldCastOperand(I.getOpcode(), LastVisited->second,
                                 I.getType(), DL);
}

Constant *ArgSpecCostVisitor::visitUnaryOperator(UnaryOperator &I) {
  assert(LastVisited != KnownConstants.end() && "Invalid iterator!");

  return ConstantFoldUnaryOpOperand(I.getOpcode(), LastVisited->second, DL);
}

Constant *ArgSpecCostVisitor::visitCallBase(CallBase &I) {
  assert(LastVisited != KnownConstants.end() && "Invalid iterator!");

  Function *F = I.getCalledFunction();
  if (!F || !canConstantFoldCallTo(&I, F))
    return nullptr;

  SmallVector<Constant *, 8> Operands;
  Operands.reserve(I.getNumOperands());

  for (unsigned Idx = 0, E = I.getNumOperands() - 1; Idx != E; ++Idx) {
    Value *V = I.getOperand(Idx);
    if (isa<MetadataAsValue>(V))
      return nullptr;
    Constant *C = findConstantFor(V);
    if (!C)
      return nullptr;
    Operands.push_back(C);
  }

  auto Ops = ArrayRef(Operands.begin(), Operands.end());
  return ConstantFoldCall(&I, F, Ops);
}

Constant *ArgSpecCostVisitor::visitFreezeInst(FreezeInst &I) {
  assert(LastVisited != KnownConstants.end() && "Invalid iterator!");

  if (isGuaranteedNotToBeUndefOrPoison(LastVisited->second))
    return LastVisited->second;
  return nullptr;
}

Constant *ArgSpecCostVisitor::visitPHINode(PHINode &I) {
  if (I.getNumIncomingValues() > MaxIncomingPhiValues)
    return nullptr;

  bool Inserted = VisitedPHIs.insert(&I).second;
  Constant *Const = nullptr;
  bool HaveSeenIncomingPHI = false;

  for (unsigned Idx = 0, E = I.getNumIncomingValues(); Idx != E; ++Idx) {
    Value *V = I.getIncomingValue(Idx);

    // Disregard self-references and dead incoming values.
    if (auto *Inst = dyn_cast<Instruction>(V))
      if (Inst == &I || !isBlockExecutable(I.getIncomingBlock(Idx)))
        continue;

    if (Constant *C = findConstantFor(V)) {
      if (!Const)
        Const = C;
      // Not all incoming values are the same constant. Bail immediately.
      if (C != Const)
        return nullptr;
      continue;
    }

    if (Inserted) {
      // First time we are seeing this phi. We will retry later, after
      // all the constant arguments have been propagated. Bail for now.
      PendingPHIs.push_back(&I);
      return nullptr;
    }

    if (isa<PHINode>(V)) {
      // Perhaps it is a Transitive Phi. We will confirm later.
      HaveSeenIncomingPHI = true;
      continue;
    }

    // We can't reason about anything else.
    return nullptr;
  }

  if (!Const)
    return nullptr;

  if (!HaveSeenIncomingPHI)
    return Const;

  DenseSet<PHINode *> TransitivePHIs;
  if (!discoverTransitivelyIncomingValues(Const, &I, TransitivePHIs))
    return nullptr;

  return Const;
}

Cost ArgSpecCostVisitor::estimateSwitchInst(SwitchInst &I) {
  assert(LastVisited != KnownConstants.end() && "Invalid iterator!");

  if (I.getCondition() != LastVisited->first)
    return 0;

  auto *C = dyn_cast<ConstantInt>(LastVisited->second);
  if (!C)
    return 0;

  BasicBlock *Succ = I.findCaseValue(C)->getCaseSuccessor();
  // Initialize the worklist with the dead basic blocks. These are the
  // destination labels which are different from the one corresponding
  // to \p C. They should be executable and have a unique predecessor.
  SmallVector<BasicBlock *> WorkList;
  for (const auto &Case : I.cases()) {
    BasicBlock *BB = Case.getCaseSuccessor();
    if (BB != Succ && isBlockExecutable(BB) &&
        canEliminateSuccessor(I.getParent(), BB))
      WorkList.push_back(BB);
  }

  return estimateBasicBlocks(WorkList);
}

Cost ArgSpecCostVisitor::estimateBranchInst(BranchInst &I) {
  assert(LastVisited != KnownConstants.end() && "Invalid iterator!");
  assert(I.isConditional() && "Only meaningful for conditional branches");

  if (I.getCondition() != LastVisited->first)
    return 0;

  BasicBlock *Succ = I.getSuccessor(LastVisited->second->isOneValue());
  // Initialize the worklist with the dead successor as long as
  // it is executable and has a unique predecessor.
  SmallVector<BasicBlock *> WorkList;
  if (isBlockExecutable(Succ) && canEliminateSuccessor(I.getParent(), Succ))
    WorkList.push_back(Succ);

  return estimateBasicBlocks(WorkList);
}

bool ArgSpecCostVisitor::discoverTransitivelyIncomingValues(
    Constant *Const, PHINode *Root, DenseSet<PHINode *> &TransitivePHIs) {

  SmallVector<PHINode *, 64> WorkList;
  WorkList.push_back(Root);
  unsigned Iter = 0;

  while (!WorkList.empty()) {
    PHINode *PN = WorkList.pop_back_val();

    if (++Iter > MaxDiscoveryIterations ||
        PN->getNumIncomingValues() > MaxIncomingPhiValues)
      return false;

    if (!TransitivePHIs.insert(PN).second)
      continue;

    for (unsigned I = 0, E = PN->getNumIncomingValues(); I != E; ++I) {
      Value *V = PN->getIncomingValue(I);

      // Disregard self-references and dead incoming values.
      if (auto *Inst = dyn_cast<Instruction>(V))
        if (Inst == PN || !isBlockExecutable(PN->getIncomingBlock(I)))
          continue;

      if (Constant *C = findConstantFor(V)) {
        // Not all incoming values are the same constant. Bail immediately.
        if (C != Const)
          return false;
        continue;
      }

      if (auto *Phi = dyn_cast<PHINode>(V)) {
        WorkList.push_back(Phi);
        continue;
      }

      // We can't reason about anything else.
      return false;
    }
  }
  return true;
}

// Only relevant if a specialized argument is itself a pointer. That case is not currently handled
Constant *ArgSpecCostVisitor::visitLoadInst(LoadInst &I) {
  assert(LastVisited != KnownConstants.end() && "Invalid iterator!");

  if (isa<ConstantPointerNull>(LastVisited->second))
    return nullptr;
  return ConstantFoldLoadFromConstPtr(LastVisited->second, I.getType(), DL);
}