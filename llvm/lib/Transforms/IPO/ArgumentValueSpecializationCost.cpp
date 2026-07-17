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