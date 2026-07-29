#ifndef LLVM_TRANSFORMS_IPO_ARGUMENTVALUESPECIALIZATION_H
#define LLVM_TRANSFORMS_IPO_ARGUMENTVALUESPECIALIZATION_H

#include "llvm/IR/PassManager.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include <cstdint>
#include <optional>
#include <tuple>

namespace llvm {

struct ArgSpecValue {
  unsigned ArgIndex;
  uint64_t ValueLo;
  std::optional<uint64_t>  ValueHi = 0;
};

struct CloneKey {
  Function *Callee;
  SmallVector<ArgSpecValue, 4> Args;

  bool operator<(const CloneKey &Other) const {
    if (Callee != Other.Callee) return Callee < Other.Callee;
    if (Args.size() != Other.Args.size()) return Args.size() < Other.Args.size();
    for (unsigned I = 0, E = Args.size(); I != E; ++I) {
      if (Args[I].ArgIndex != Other.Args[I].ArgIndex) return Args[I].ArgIndex < Other.Args[I].ArgIndex;
      if (Args[I].ValueLo != Other.Args[I].ValueLo)   return Args[I].ValueLo   < Other.Args[I].ValueLo;
      if (Args[I].ValueHi != Other.Args[I].ValueHi)   return Args[I].ValueHi   < Other.Args[I].ValueHi;
    }
    return false;
  }
};


class ArgumentValueSpecialization
    : public PassInfoMixin<ArgumentValueSpecialization> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

  Function *cloneAndSpecialize(const CloneKey &Key);

  void insertGuard(CallBase *CB, Function *Clone, ArrayRef<ArgSpecValue> ArgSpecs);

  Constant *buildConstantFromBits(Type *ArgTy, uint64_t ValueLo, std::optional<uint64_t> ValueHi);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_IPO_ARGUMENTVALUESPECIALIZATION_H