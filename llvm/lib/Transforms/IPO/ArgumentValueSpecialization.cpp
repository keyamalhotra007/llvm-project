#include "llvm/Transforms/IPO/ArgumentValueSpecialization.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstrTypes.h" 
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
  GrowthMap Growth; //accumulates how much code size each function has grown due to specializations so far
  std::map<CloneKey, Function *> ClonedFunctions; //dedup cache: different callsites, same clone key -> share clone

  // Pass 1: score everything, track best ArgIndex per call site.
  DenseMap<CallBase *, std::pair<unsigned, unsigned>> BestArgForCallSite; //map CB to the best {ArgIndex, Score} found so fare
  SmallVector<unsigned, 0> Scores(Candidates.size()); //store score for every candidate by index

  // for every CB which single argument index gives the maximum specialization benefit
  for (unsigned I = 0, size = Candidates.size(); I != size; ++I) {
    auto &Cand = Candidates[I];
    Function *Callee = Cand.Callee;
    TargetTransformInfo &TTI = FAM.getResult<TargetIRAnalysis>(*Callee);

    unsigned Score = computeSpecializationScore(*Callee, Cand.Percentage, TTI, Growth);
    Scores[I] = Score;
    if (Score == 0) //reject
      continue;

    CallBase *CB = Cand.CB;
    auto It = BestArgForCallSite.find(CB); //Have we already recorded a best arg?
    if (It == BestArgForCallSite.end() || Score > It->second.second)
      BestArgForCallSite[CB] = {Cand.ArgIndex, Score};
  }

  // clone/guard only for the winning ArgIndex of each call site.
  for (unsigned I = 0, size = Candidates.size(); I != size; ++I) {
    auto &Cand = Candidates[I];
    unsigned Score = Scores[I];
    if (Score == 0)
      continue;

    CallBase *CB = Cand.CB;
    auto BestIt = BestArgForCallSite.find(CB);
    if (BestIt == BestArgForCallSite.end() || BestIt->second.first != Cand.ArgIndex)
      continue;

    Function *Callee = Cand.Callee;
    TargetTransformInfo &TTI = FAM.getResult<TargetIRAnalysis>(*Callee); //needed for chargeGrowth
    CloneKey Key{Cand.Callee, Cand.ArgIndex, Cand.ValueLo, Cand.ValueHi};

    Function *Clone = nullptr;
    if (auto CloneIt = ClonedFunctions.find(Key); CloneIt != ClonedFunctions.end()) {
      Clone = CloneIt->second; //if some other call site already created a clone for this exact (Callee, ArgIndex, Value) combination, reuse it 
    } else {
      Clone = cloneAndSpecialize(Key);
      ClonedFunctions[Key] = Clone;
      chargeGrowth(*Callee, TTI, Growth);
      Changed = true;
      insertGuard(CB, Clone, Key.ArgIndex, Key.ValueLo, Key.ValueHi);
    }
  }

  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();

}

Constant *ArgumentValueSpecialization::buildConstantFromBits(Type *ArgTy, uint64_t ValueLo, std::optional<uint64_t> ValueHi) {


  Constant *ReplacementConst;

  if (ArgTy->isIntegerTy()) {
  ReplacementConst = ConstantInt::get(ArgTy, ValueLo);
  } else if (ArgTy->isFloatingPointTy()) {
    unsigned Bits = ArgTy->getPrimitiveSizeInBits();
    APInt Raw(128, {ValueLo, *ValueHi});
    Raw = Raw.trunc(Bits);
    ReplacementConst = ConstantFP::get(ArgTy->getContext(), APFloat(ArgTy->getFltSemantics(), Raw));
  } else {
    llvm_unreachable("buildConstantFromBits: unsupported argument type reached specialization");
  }
    return ReplacementConst;
  }

Function *ArgumentValueSpecialization::cloneAndSpecialize(const CloneKey &Key) {
  ValueToValueMapTy Mappings;
  Function *Clone = CloneFunction(Key.Callee, Mappings);
  Clone->setName(Key.Callee->getName() + ".argspec." +
                 Twine(Key.ArgIndex) + "." + Twine(Key.ValueLo)); //TODO: Can remove ValueLo later after debugging done


  Argument *SpecArg = Clone->getArg(Key.ArgIndex);
  Type *ArgTy = SpecArg->getType();
  Constant *ReplacementConst;

  ReplacementConst = buildConstantFromBits(ArgTy, Key.ValueLo, Key.ValueHi.value_or(0));

  SpecArg->replaceAllUsesWith(ReplacementConst);
  return Clone;
}

void ArgumentValueSpecialization::insertGuard(CallBase *CB, Function *Clone,
                                               unsigned ArgIndex,
                                               uint64_t ValueLo,
                                               std::optional<uint64_t> ValueHi) {
  // TODO: handle InvokeInst
  if (isa<InvokeInst>(CB))
    return;
                                              
  Value *ActualArg = CB->getArgOperand(ArgIndex);
  Type *ArgTy = ActualArg->getType();

  Constant *SpecConst = buildConstantFromBits(ArgTy, ValueLo, ValueHi); //constant to specialize function for 

  IRBuilder<> Builder(CB); //insertion point immediately before the CB

  Value *Cond = ArgTy->isIntegerTy() 
                    ? Builder.CreateICmpEQ(ActualArg, SpecConst, "spec.cmp")
                    : Builder.CreateFCmpOEQ(ActualArg, SpecConst, "spec.cmp");

  // Then = hot path (call the specialized clone), Else = cold path (original call).
  Instruction *ThenTerm = nullptr; // cond true
  Instruction *ElseTerm = nullptr; // cond false
  SplitBlockAndInsertIfThenElse(Cond, CB, &ThenTerm, &ElseTerm);

  CB->moveBefore(ElseTerm->getIterator());

  // CB stays where it is: that's the Else (cold) block.
  CallBase *HotCall = cast<CallInst>(CB->clone()); //copies call instruction
  HotCall->setCalledFunction(Clone); // point hot call at specialized clone
  HotCall->insertBefore(ThenTerm->getIterator()); // place as last instruction in then block

  // If the result is used, join the two call results with a PHI in the merge block.
  if (!CB->use_empty()) { //use_empty: nothing uses return value
    BasicBlock *ThenBB = ThenTerm->getParent();
    BasicBlock *ElseBB = ElseTerm->getParent();
    BasicBlock *MergeBB = ThenTerm->getSuccessor(0); // common successor of then and else

    IRBuilder<> MergeBuilder(&*MergeBB->getFirstInsertionPt());
    PHINode *PN = MergeBuilder.CreatePHI(CB->getType(), 2, "spec.retval"); //an empty PHI node with 2 incoming-value slots at the top of Merge

    // Find every instruction in the function that currently has %call as one of its operands
    // rewrite that operand to be %spec.retval (the PHI) instead.
    CB->replaceAllUsesWith(PN);

    // incoming edges 
    PN->addIncoming(HotCall, ThenBB);
    PN->addIncoming(CB, ElseBB);
  }
}