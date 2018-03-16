//===-- CanonicalizeTypeToI8Ptr.h - Canonicalize Type to i8*-----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This fold inttoptr(ptrtoint(p)) -> bitcast p.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_INTPTRFOLD_H
#define LLVM_TRANSFORMS_SCALAR_INTPTRFOLD_H

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

class InitialIntPtrFoldPass : public PassInfoMixin<InitialIntPtrFoldPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};
}

#endif // LLVM_TRANSFORMS_SCALAR_INTPTRFOLD_H
