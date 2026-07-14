#ifndef LLVM_TRANSFORMS_UTILS_ARGUMENTVALUESPECIALIZATION_H
#define LLVM_TRANSFORMS_UTILS_ARGUMENTVALUESPECIALIZATION_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class ArgumentValueSpecialization
    : public PassInfoMixin<ArgumentValueSpecialization> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_UTILS_ARGUMENTVALUESPECIALIZATION_H