//===-- InitialIntPtrFold.cpp - Fold inttoptr(ptrtoint) ---------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This folds inttoptr(ptrtoint(p)) -> p.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/InitialIntPtrFold.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Transforms/Utils/Local.h"
using namespace llvm;

#define DEBUG_TYPE "initialintptrfold"

static bool runOnBasicBlock(BasicBlock &BB) {
  bool Changed = false;
  SmallVector<IntToPtrInst*, 32> Worklist;

  for (BasicBlock::iterator DI = BB.begin(), DE = BB.end(); DI != DE;) {
    Instruction *Inst = &*DI++;
    IntToPtrInst *II = dyn_cast<IntToPtrInst>(Inst);
    if (!II) continue;

    PtrToIntInst *PI = dyn_cast<PtrToIntInst>(II->getOperand(0));;
    if (!PI) continue;

    Value *P = PI->getOperand(0);
    if (P->getType()->getPointerAddressSpace() !=
        II->getType()->getPointerAddressSpace())
      continue;

    Worklist.push_back(II);
  }

  for (auto I = Worklist.begin(); I != Worklist.end(); ++I) {
    IntToPtrInst *II = *I;
    PtrToIntInst *PI = dyn_cast<PtrToIntInst>(II->getOperand(0));

    Value *P = PI->getOperand(0);
    if (P->getType() != II->getType()) {
      IRBuilder<> Bldr(II);
      P = Bldr.CreatePointerCast(P, II->getType());
    }

    II->replaceAllUsesWith(P);
    II->eraseFromParent();
    if (PI->use_begin() == PI->use_end())
      PI->eraseFromParent();

    Changed = true;
  }

  return Changed;
}

static bool foldFunction(Function &F) {
  bool Changed = false;
  for (BasicBlock &BB : F) {
    Changed |= runOnBasicBlock(BB);
  }
  return Changed;
}

PreservedAnalyses InitialIntPtrFoldPass::run(Function &M, FunctionAnalysisManager &AM) {
  if (!foldFunction(M))
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  PA.preserve<GlobalsAA>();
  return PA;
}

namespace {
class InitialIntPtrFold : public FunctionPass {
public:
  static char ID;
  InitialIntPtrFold() : FunctionPass(ID) {
    initializeInitialIntPtrFoldPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addPreserved<DominatorTreeWrapperPass>();
    AU.addPreserved<GlobalsAAWrapperPass>();
    FunctionPass::getAnalysisUsage(AU);
  }

  bool runOnFunction(Function &F) override {
    if (skipFunction(F))
      return false;
    return foldFunction(F);
  }
private:
  InitialIntPtrFoldPass Impl;
};

}

char InitialIntPtrFold::ID = 0;
INITIALIZE_PASS(InitialIntPtrFold, "initialintptrfold",
                "Folds inttoptr(ptrtoint) to bitcast", false, false)

FunctionPass *llvm::createInitialIntPtrFoldPass() {
  return new InitialIntPtrFold();
}
