#ifndef LLVM_TRANSFORMS_IPO_ARGUMENTVALUESPECIALIZATION_H
#define LLVM_TRANSFORMS_IPO_ARGUMENTVALUESPECIALIZATION_H

#include "llvm/IR/PassManager.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include <cstdint>
#include <optional>
#include <tuple>

namespace llvm {

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


class ArgumentValueSpecialization
    : public PassInfoMixin<ArgumentValueSpecialization> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

  Function *cloneAndSpecialize(const CloneKey &Key);

  void insertGuard(CallBase *CB, Function *Clone, unsigned ArgIndex,
                    uint64_t ValueLo, std::optional<uint64_t> ValueHi);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_IPO_ARGUMENTVALUESPECIALIZATION_H