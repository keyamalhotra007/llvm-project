#include "llvm/Transforms/Utils/ArgumentValueSpecialization.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h" // SplitBlockAndInsertIfThenElse
#include "llvm/Transforms/Utils/Cloning.h"

#include <cstdint>
#include <map>
#include <optional>
#include <tuple>

using namespace llvm;

namespace {

struct CloneKey {
  Function *Callee;
  unsigned ArgIndex;
  uint64_t ValueLo;
  std::optional<uint64_t> ValueHi;

  bool operator<(const CloneKey &Other) const {
    return std::tie(Callee, ArgIndex, ValueLo, ValueHi) <
           std::tie(Other.Callee, Other.ArgIndex, Other.ValueLo,
                    Other.ValueHi);
  }
};

struct CallSiteObservation {
  CallBase *CB;
  uint8_t Percentage;
};

} // end namespace

PreservedAnalyses
ArgumentValueSpecialization::run(Module &M, ModuleAnalysisManager &AM) {
  std::map<CloneKey, SmallVector<CallSiteObservation, 4>> Candidates;

  bool Changed = false;

  for (Function &F : M) {
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        auto *CB = dyn_cast<CallBase>(&I);
        if (!CB)
          continue;

        MDNode *IntMD = CB->getMetadata("int-args");
        MDNode *FpMD = CB->getMetadata("fp-args");

        unsigned IntPos = 0;
        unsigned FpPos = 0;

        Function *Callee = CB->getCalledFunction();
        if (!Callee)
          continue;

        FunctionType *FTy = Callee->getFunctionType();
        unsigned NumArgs = FTy->getNumParams();

        for (unsigned ArgIndex = 0; ArgIndex < NumArgs; ++ArgIndex) {
          Type *ValueTy = FTy->getParamType(ArgIndex);

          if (ValueTy->isIntegerTy()) {
            // handle integer arg
          } else if (ValueTy->isFloatingPointTy()) {
            // handle fp arg
          }
        }
      }
    }
  }

  if (!Changed)
    return PreservedAnalyses::all();

  return PreservedAnalyses::none();
}