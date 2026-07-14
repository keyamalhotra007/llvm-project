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

bool llvm::shouldSpecialize(Function &Callee, uint8_t HotnessPercentage,
                            TargetTransformInfo &TTI, GrowthMap &Growth) {
  if (!isEligibleForSpecialization(Callee))
    return false;

  if (HotnessPercentage < ArgSpecHotnessThreshold)
    return false;

  unsigned FuncSize = estimateFunctionCodeSize(Callee, TTI);

  // never specialize past this size, no matter how hot.
  if (FuncSize == 0 || FuncSize > ArgSpecMaxAbsoluteSize)
    return false;

  // if the growth already charged to this callee,
  // plus what this new clone would add, 
  // exceeds N times the original function's size: reject.
  unsigned PriorGrowth = Growth.lookup(&Callee);
  if ((PriorGrowth + FuncSize) / FuncSize > ArgSpecMaxCodeSizeGrowth)
    return false;

  return true;
}

void llvm::chargeGrowth(Function &Callee, TargetTransformInfo &TTI,
                        GrowthMap &Growth) {
  Growth[&Callee] += estimateFunctionCodeSize(Callee, TTI);
}