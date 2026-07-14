#include "llvm/Transforms/IPO/ArgumentValueSpecialization.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/TargetTransformInfo.h"
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
#include "llvm/Transforms/IPO/ArgumentValueSpecializationCost.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h" 
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
         std::tie(Other.Callee, Other.ArgIndex, Other.ValueLo, Other.ValueHi);
}
};

struct SpecializationCandidate {
  Function *Callee;
  unsigned ArgIndex;
  uint64_t ValueLo;
  std::optional<uint64_t> ValueHi; //only for Fp, not int
  CallBase *CB;
  uint8_t Percentage;
};


} // end namespace

PreservedAnalyses
ArgumentValueSpecialization::run(Module &M, ModuleAnalysisManager &AM) {
  std::vector<SpecializationCandidate> Candidates;


  bool Changed = false;

  FunctionAnalysisManager &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();
  for (Function &F : M) {
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        auto *CB = dyn_cast<CallBase>(&I);
        if (!CB)
          continue;

        MDNode *IntMD = CB->getMetadata("int-args");
        MDNode *FpMD = CB->getMetadata("fp-args");


        // Index for metadata
        unsigned IntPos = 0;
        unsigned FpPos = 0;

        Function *Callee = CB->getCalledFunction();
        if (!Callee)
          continue;

        FunctionType *FTy = Callee->getFunctionType();
        unsigned NumArgs = FTy->getNumParams();
        

        for (unsigned ArgIndex = 0; ArgIndex < NumArgs; ++ArgIndex) {
        
          Type *ValueTy = FTy->getParamType(ArgIndex);

          if (ValueTy->isIntegerTy() || ValueTy->isPointerTy()) { 
            if (!IntMD) continue;        
            
            auto *ValMeta = cast<ConstantAsMetadata>(IntMD->getOperand(IntPos * 2 + 1));
            auto *ValConst = cast<ConstantInt>(ValMeta->getValue());
            uint64_t Value = ValConst->getZExtValue();

            auto *PctMeta = cast<ConstantAsMetadata>(IntMD->getOperand(IntPos * 2 + 2));
            auto *PctConst = cast<ConstantInt>(PctMeta->getValue());
            uint8_t Percentage = (uint8_t)PctConst->getZExtValue();

            Candidates.push_back({Callee, ArgIndex, Value, std::nullopt, CB, Percentage});
            ++IntPos;

          } else if (ValueTy->isFloatingPointTy()) {
            if (!FpMD) continue;
            
            auto *LoMeta = cast<ConstantAsMetadata>(FpMD->getOperand(FpPos * 3 + 1));
            auto *LoConst = cast<ConstantInt>(LoMeta->getValue());
            uint64_t Lo = LoConst->getZExtValue();

            auto *HiMeta = cast<ConstantAsMetadata>(FpMD->getOperand(FpPos * 3 + 2));
            auto *HiConst = cast<ConstantInt>(HiMeta->getValue());
            uint64_t Hi = HiConst->getZExtValue();

            auto *PctMeta = cast<ConstantAsMetadata>(FpMD->getOperand(FpPos * 3 + 3));
            auto *PctConst = cast<ConstantInt>(PctMeta->getValue());
            uint8_t Percentage = (uint8_t)PctConst->getZExtValue();

            Candidates.push_back({Callee, ArgIndex, Lo, Hi, CB, Percentage}); 

            ++FpPos;
          }
        }
      }
    }
  }
  // TODO: Clone candidate with highest score
  GrowthMap Growth;
  std::map<CloneKey, Function *> ClonedFunctions; // dedup cache: reuse clone for same (Callee, ArgIndex, Value)

  for (auto &Cand : Candidates) {
    Function *Callee = Cand.Callee;
    TargetTransformInfo &TTI = FAM.getResult<TargetIRAnalysis>(*Callee);

    unsigned Score = computeSpecializationScore(*Callee, Cand.Percentage, TTI, Growth);
    if (Score == 0)
      continue;

    CloneKey Key{Cand.Callee, Cand.ArgIndex, Cand.ValueLo, Cand.ValueHi};

    Function *Clone = nullptr;
    if (auto It = ClonedFunctions.find(Key); It != ClonedFunctions.end()) {
      Clone = It->second; // reuse existing clone for this key
    } else {
      // Clone = cloneAndSpecialize(Callee, Cand.ArgIndex, Cand.ValueLo, Cand.ValueHi);
      ClonedFunctions[Key] = Clone;
      chargeGrowth(*Callee, TTI, Growth); // charge only once, on first creation of this key
    }

    // insertGuard(Callee, Clone, Cand.CB, Cand.ArgIndex, Cand.ValueLo);
    // Changed = true;
  }


  if (!Changed)
    return PreservedAnalyses::all();

  return PreservedAnalyses::none();
}