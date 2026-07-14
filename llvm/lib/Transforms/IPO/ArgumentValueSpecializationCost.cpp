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
    "argspec-max-codesize-growth", cl::init(3), cl::Hidden,
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
                                          ArrayRef<ArgCandidate> Args,
                                          TargetTransformInfo &TTI,
                                          GrowthMap &Growth) {
  if (Args.empty())
    return 0;
 
  if (!isEligibleForSpecialization(Callee))
    return 0;
 
  unsigned FuncSize = estimateFunctionCodeSize(Callee, TTI);
  if (FuncSize == 0 || FuncSize > ArgSpecMaxAbsoluteSize)
    return 0;
 
  unsigned PriorGrowth = Growth.lookup(&Callee);
  if ((PriorGrowth + FuncSize) / FuncSize > ArgSpecMaxCodeSizeGrowth)
    return 0;
 
  // --- Combine hotness across every argument in this candidate ---
  //
  // Each argument's Percentage is treated as an independent probability that
  // this call site presents that particular constant. For a multi-argument
  // specialization, the guard we'd insert only fires when ALL of the hot
  // values are present simultaneously, so we combine via product rather than
  // average: P(all N co-occur) = product of P(each), under an independence
  // assumption. This likely understates true combined hotness for correlated
  // arguments, so it's a conservative (lower-bound) estimate, not a precise
  // one.
  //
  // Any single argument below the hotness threshold disqualifies the whole
  // combination -- one cold argument means the guard rarely fires no matter
  // how hot the others are.
  unsigned CombinedPercent = 100; // 100%
  for (const ArgCandidate &A : Args) {
    if (A.Percentage < 97)
      return 0;
    CombinedPercent = (CombinedPercent * A.Percentage) / 100;
    if (CombinedPercent < 90)
      return 0;
  }
 
  // --- Score: higher combined hotness and smaller (less-already-grown)
  // callee both increase the score. ---
  //
  // Dividing by (FuncSize + PriorGrowth) rather than just FuncSize means a
  // callee that has already had other specializations charged against it
  // scores lower for further specialization - all else equal, prefer
  // spending your growth budget on callees you haven't already spent it on.
  uint64_t SizePenalty = FuncSize + PriorGrowth;
  uint64_t Score = (CombinedPercent * 100) / SizePenalty;
 
  // A candidate that passed every gate above must never score exactly 0 --
  // 0 is reserved to mean "rejected." Integer division can legitimately
  // truncate a small-but-real score down to 0, so floor it at 1.
  return Score > 0 ? static_cast<unsigned>(Score) : 1;
}