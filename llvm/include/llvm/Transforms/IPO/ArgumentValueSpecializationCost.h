//===- ArgumentValueSpecializationCost.h -----------------------*- C++ -*-===//
//
// Cost model for PEBS-guided argument value specialization.
//
// Adapted from the profitability heuristics in
// llvm/lib/Transforms/IPO/FunctionSpecialization.cpp, simplified to whole-
// function codesize costing (no per-argument use-def walking yet -- that's
// deferred until the clone+guard transform itself is validated).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_IPO_ARGUMENTVALUESPECIALIZATIONCOST_H
#define LLVM_TRANSFORMS_IPO_ARGUMENTVALUESPECIALIZATIONCOST_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Function.h"
#include <cstdint>

namespace llvm {

struct ArgCandidate {
  uint64_t Value;
  unsigned Percentage;
};

/// Tracks accumulated clone-codesize growth per original callee, so that
/// repeated specializations of the same function (for different call sites) are 
/// charged against a shared budget rather than each being evaluated in isolation.
using GrowthMap = DenseMap<Function *, unsigned>;

/// Whole-function codesize cost, summed via TargetTransformInfo.
unsigned estimateFunctionCodeSize(Function &F, TargetTransformInfo &TTI);

/// Structural eligibility checks, adapted from FunctionSpecializer::isCandidateFunction()
bool isEligibleForSpecialization(Function &F);

/// Call after committing to a specialization, to charge its size against the
/// callee's growth budget for subsequent candidates.
void chargeGrowth(Function &Callee, TargetTransformInfo &TTI,
                  GrowthMap &Growth);

/// Score a candidate specialization: either a single hot argument, or a
/// combination of several hot arguments at the same call site. Higher is
/// better. Returns 0 if the hard size/growth gates are violated (i.e. this
/// candidate must not be specialized at all)
unsigned computeSpecializationScore(Function &Callee,
                                    uint8_t Percentage,
                                    TargetTransformInfo &TTI,
                                    GrowthMap &Growth);

} // namespace llvm

#endif // LLVM_TRANSFORMS_IPO_ARGUMENTVALUESPECIALIZATIONCOST_H