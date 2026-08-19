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
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "argument-value-specialization"
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
        if (!Callee || Callee->isIntrinsic())
          continue;

        FunctionType *FTy = Callee->getFunctionType();
        unsigned NumArgs = FTy->getNumParams();
        

        for (unsigned ArgIndex = 0; ArgIndex < NumArgs; ++ArgIndex) {
          Type *ValueTy = FTy->getParamType(ArgIndex);

          bool ArgIsConstant = ArgIndex < CB->arg_size() && isa<Constant>(CB->getArgOperand(ArgIndex));

          // Only collect candidates for integer or floating-point formal
          // parameter types. Other types (pointers, aggregates, vectors,
          // etc.) are unsupported by buildConstantFromBits and should be
          // skipped to avoid crashes.
          if (ValueTy->isIntegerTy()) {
            if (!IntMD)
              continue;

            unsigned NeededIdx = IntPos * 2 + 2;
            if (IntMD->getNumOperands() <= NeededIdx) {
              LLVM_DEBUG(dbgs() << "ArgumentValueSpecialization: metadata for callee "
                                << Callee->getName() << " does not cover int-formal "
                                << ArgIndex << ", skipping\n");
              continue;
            }

            if (ArgIsConstant) {
              ++IntPos;  
            continue;
            }
            
            auto *ValMeta = cast<ConstantAsMetadata>(IntMD->getOperand(IntPos * 2 + 1));
            auto *ValConst = cast<ConstantInt>(ValMeta->getValue());
            uint64_t Value = ValConst->getZExtValue();

            auto *PctMeta = cast<ConstantAsMetadata>(IntMD->getOperand(IntPos * 2 + 2));
            auto *PctConst = cast<ConstantInt>(PctMeta->getValue());
            uint8_t Percentage = (uint8_t)PctConst->getZExtValue();

            Candidates.push_back({Callee, ArgIndex, Value, std::nullopt, CB, Percentage});
            ++IntPos;

          } else if (ValueTy->isFloatingPointTy()) {
            if (!FpMD)
              continue;

            unsigned NeededIdx = FpPos * 3 + 3;
            if (FpMD->getNumOperands() <= NeededIdx) {
              LLVM_DEBUG(dbgs() << "ArgumentValueSpecialization: metadata for callee "
                                << Callee->getName() << " does not cover fp-formal "
                                << ArgIndex << ", skipping\n");
              continue;
            }

            if (ArgIsConstant) {
              ++FpPos;
              continue;
            }

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
          } else {
            // Unsupported formal parameter type: skip.
            LLVM_DEBUG(dbgs() << "ArgumentValueSpecialization: skipping unsupported formal type "
                              << *ValueTy << " for callee " << Callee->getName() << "\n");
            continue;
          }
        }
      }
    }
  }
  GrowthMap Growth; //accumulates how much code size each function has grown due to specializations so far
  std::map<CloneKey, Function *> ClonedFunctions; //dedup cache: different callsites, same clone key -> share clone

  // for every CB which single argument index gives the maximum specialization benefit
  constexpr double JointPercentFloor =50.0; //an arg only enters joint arg consideration if it's hot on its own
  constexpr unsigned MaxJointArity = 3; //combo size cap

  auto GetBFI = [&FAM](Function &F) -> BlockFrequencyInfo & {
    return FAM.getResult<BlockFrequencyAnalysis>(F);
  };

  MapVector<CallBase *, SmallVector<unsigned, 8>> CandidatesByCB; // regroup by CB
  for (unsigned I = 0, E = Candidates.size(); I != E; ++I)
    CandidatesByCB[Candidates[I].CB].push_back(I);

  //Per call site, try every legal subset of its hot args and keep only the best-scoring one.
  struct SubsetResult {
    SmallVector<unsigned, 4> CandIdxs;
    double JointPercentBound;
    Cost CodeSizeSavings;
  };

  DenseMap<CallBase *, SubsetResult> BestForCallSite; // needed to specialize 
  DenseMap<CallBase *, unsigned> BestScoreForCallSite; // running max

  for (auto &[CB, Idxs] : CandidatesByCB) {
    SmallVector<unsigned, 8> Hot;
    for (unsigned Idx : Idxs)
      if (Candidates[Idx].Percentage > JointPercentFloor)
        Hot.push_back(Idx);
    if (Hot.empty())
      continue;


    Function *Callee = Candidates[Hot[0]].Callee;
    TargetTransformInfo &TTI = FAM.getResult<TargetIRAnalysis>(*Callee);

    Function *Caller = CB->getFunction();
    BlockFrequencyInfo &BFI = GetBFI(*Caller);
    BasicBlock *BB = CB->getParent();

    double CallFreq = static_cast<double>(BFI.getBlockFreq(BB).getFrequency()) /
                    static_cast<double>(BFI.getEntryFreq().getFrequency());


    unsigned MaxArity = std::min<unsigned>(Hot.size(), MaxJointArity);
    unsigned FullMask = (1u << Hot.size()) - 1;

    for (unsigned Mask = 1; Mask <= FullMask; ++Mask) {
      if ((unsigned)llvm::popcount(Mask) > MaxArity) 
        continue;

      SmallVector<unsigned, 4> Combo;
      SmallVector<ArgSubstitution, 4> Substitutions;
      double JointPercentBound = 0.0;
      bool SkipCombo = false;
      for (unsigned Bit = 0; Bit < Hot.size(); ++Bit) {
        if (!(Mask & (1u << Bit)))
          continue;
        unsigned Idx = Hot[Bit];
        auto &Cand = Candidates[Idx];
        Combo.push_back(Idx);
        JointPercentBound += Cand.Percentage;

        Argument *Arg = Callee->getArg(Cand.ArgIndex);
        Constant *C = buildConstantFromBits(Arg->getType(), Cand.ValueLo, Cand.ValueHi);
        if (!C) {
          SkipCombo = true;
          break;
        }
        Substitutions.push_back({Arg, C});
      }
      if (SkipCombo)
        continue;
      // Bonferroni bound: valid lower bound on P(A∩B∩...) given per-arg P(A),P(B)...
      JointPercentBound = std::max(0.0, JointPercentBound - 100.0 * (Combo.size() - 1));

      Cost CodeSizeSavings = 0;
      unsigned Score = computeSpecializationScore(*Callee, Substitutions,
                                                  JointPercentBound, CallFreq,
                                                  GetBFI, TTI, Growth, &CodeSizeSavings);
      if (Score == 0)
        continue;

      auto It = BestScoreForCallSite.find(CB);
      if (It == BestScoreForCallSite.end() || Score > It->second) {
        BestScoreForCallSite[CB] = Score;
        BestForCallSite[CB] = {Combo, JointPercentBound, CodeSizeSavings};
      }
    }
  }

    // clone/guard only for the winning ArgIndex of each call site.
    for (auto &[CB, Best] : BestForCallSite) {
    Function *Callee = Candidates[Best.CandIdxs[0]].Callee;
    TargetTransformInfo &TTI = FAM.getResult<TargetIRAnalysis>(*Callee);

    CloneKey Key;
    Key.Callee = Callee;
    for (unsigned Idx : Best.CandIdxs) {
      auto &C = Candidates[Idx];
      Key.Args.push_back({C.ArgIndex, C.ValueLo, C.ValueHi});
    }
    llvm::sort(Key.Args, [](const ArgSpecValue &A, const ArgSpecValue &B) {
      return A.ArgIndex < B.ArgIndex;
    });

    Function *Clone = nullptr;
    auto CloneIt = ClonedFunctions.find(Key);
    if (CloneIt != ClonedFunctions.end()) {
      Clone = CloneIt->second;
    } else {
      Clone = cloneAndSpecialize(Key); // now RAUWs every Args[i], not just one
      if (!Clone)
        continue; // couldn't build a valid clone for this key
      ClonedFunctions[Key] = Clone;
      chargeGrowth(*Callee, TTI, Growth, Best.CodeSizeSavings);
      Changed = true;
    }
    insertGuard(CB, Clone, Key.Args); // generalized: AND-chain of compares
  }

    if (!Changed)
      return PreservedAnalyses::all();
    return PreservedAnalyses::none();

}

Constant *ArgumentValueSpecialization::buildConstantFromBits(Type *ArgTy, uint64_t ValueLo, std::optional<uint64_t> ValueHi) {

  Constant *ReplacementConst = nullptr;

  if (ArgTy->isIntegerTy()) {
    ReplacementConst = ConstantInt::get(ArgTy, ValueLo);
  } else if (ArgTy->isFloatTy() || ArgTy->isDoubleTy()) {
    if (!ValueHi.has_value()) {
      LLVM_DEBUG(dbgs() << "buildConstantFromBits: missing ValueHi for FP type " << *ArgTy
                        << ", skipping candidate\n");
      return nullptr;
    }
    unsigned Bits = ArgTy->getPrimitiveSizeInBits();
    APInt Raw(128, {ValueLo, *ValueHi});
    Raw = Raw.trunc(Bits);
    ReplacementConst = ConstantFP::get(ArgTy->getContext(), APFloat(ArgTy->getFltSemantics(), Raw));
  } else if (ArgTy->isFloatingPointTy()) {
    LLVM_DEBUG(dbgs() << "buildConstantFromBits: unsupported floating-point type " << *ArgTy
                      << ", skipping candidate\n");
    return nullptr;
  } else {
    LLVM_DEBUG(dbgs() << "buildConstantFromBits: unsupported argument type " << *ArgTy
                      << ", skipping candidate\n");
    return nullptr;
  }

  return ReplacementConst;
}

Function *ArgumentValueSpecialization::cloneAndSpecialize(const CloneKey &Key) {
  ValueToValueMapTy Mappings;
  Function *Clone = CloneFunction(Key.Callee, Mappings);

  std::string Name = (Key.Callee->getName() + ".argspec").str();
  for (const ArgSpecValue &AV : Key.Args)
    Name += ("." + Twine(AV.ArgIndex) + "." + Twine(AV.ValueLo)).str();
  Clone->setName(Name); 

  for (const ArgSpecValue &AV : Key.Args) {
    Argument *SpecArg = Clone->getArg(AV.ArgIndex);
    Type *ArgTy = SpecArg->getType();
    Constant *ReplacementConst = buildConstantFromBits(ArgTy, AV.ValueLo, AV.ValueHi);
    if (!ReplacementConst) {
      LLVM_DEBUG(dbgs() << "cloneAndSpecialize: failed to build constant for arg " << AV.ArgIndex
                        << " in clone " << Clone->getName() << ", aborting clone\n");
      return nullptr;
    }
    SpecArg->replaceAllUsesWith(ReplacementConst);
  }

  return Clone;
}

void ArgumentValueSpecialization::insertGuard(CallBase *CB, Function *Clone,
                                               ArrayRef<ArgSpecValue> ArgSpecs) {
  if (isa<InvokeInst>(CB))
    return; // bailing on invoke

  IRBuilder<> Builder(CB);
  Value *Cond = nullptr;
  for (auto &AV : ArgSpecs) {
    if (AV.ArgIndex >= CB->arg_size()) {
      LLVM_DEBUG(dbgs() << "insertGuard: callsite has fewer args (" << CB->arg_size()
                        << ") than specialization expects index " << AV.ArgIndex
                        << ", skipping guard for callsite\n");
      return;
    }
    Value *ArgVal = CB->getArgOperand(AV.ArgIndex);
    Value *ConstVal = buildConstantFromBits(ArgVal->getType(), AV.ValueLo, AV.ValueHi);
    if (!ConstVal) {
      LLVM_DEBUG(dbgs() << "insertGuard: could not build constant for guard on arg " << AV.ArgIndex
                        << ", skipping guard for callsite\n");
      return;
    }
    Value *Cmp = ArgVal->getType()->isFloatingPointTy()
                     ? Builder.CreateFCmpOEQ(ArgVal, ConstVal)
                     : Builder.CreateICmpEQ(ArgVal, ConstVal);
    Cond = Cond ? Builder.CreateAnd(Cond, Cmp) : Cmp;
  }


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