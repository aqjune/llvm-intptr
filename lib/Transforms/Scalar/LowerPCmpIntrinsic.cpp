//===-- LowerPCmpIntrinsic.cpp - Lower @llvm.pcmp -------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// It is valid for Clang to translate C code
//    if (p pred q) { // pred is either ==, !=, <, <=.
//      use(p);
//    }
// into IR
//      rp = restrict (p, q)
//      rq = restrict (q, p)
//      c = icmp pred rp, rq
//      br c, ifblock, exitblock
//    ifblock:
//      use(rp)
// and this enables GVN propagateEquality on pointer equality.
//
// One possible way to implement this is let _clang_ insert 'restrict' on all
// uses of 'p' when if/while/for statement on condition 'p pred q' is met.
// However implementing this is nontrivial because C syntax is not in
// SSA-form. For example,
//    if (p == q) { // "p == q" is translated into IR
//                  // rp = restrict p, q; rq = restrict q, p;
//                  // c = icmp eq rp, rq 
//      *p = 10; // Here, it is valid for clang to translate *p into IR
//               // "store rp, 10",
//               // , but
//      p = another_pointer; // The value of p has changed!
//      *p = 10; // Now it is invalid to translate *p into IR
//               // "store rp, 10".
//    }
// (a proficient clang developer may know how to consider this case -
//  there might exist some utility function in clang which returns true if
//  the value of 'p' is no more alive. However I don't
//  know such utility function, and there are so many types of statements
//  in clang that I have no idea what it is (for example, 'Expr' class in clang
//  is a subclass of 'Stmt' class, and 'Expr' itself has so many subclasses
//  as well.) so it would be hard for me to exhaustively consider all cases.
//
// This pass is an alternative way to implement insertion of 'restrict's.
// Now, clang does not insert restrict directly.
// Instead, clang translates C code
//    if (p pred q) {
//      use (p);
//    }
// into IR
//      c = call llvm.pcmp (pred, p, q)
//      br c, ifblock, exitblock
//    ifblock:
//      use (p)
// 
// and this pass (LowerPCmpIntrinsic) converts IR
//      c = call llvm.pcmp (pred, p, q)
//      br c, ifblock, exitblock
//    ifblock:
//      use p1
// into IR
//      rp = restrict (p, q)
//      rq = restrict (q, p)
//      c = icmp pred rp, rq
//      br c, ifblock, exitblock
//    ifblock:
//      use (rp) // changed!
//
// Note that this pass is, actually, wrong. 
// Assume that p = p0 + n (where
// n is size of p0's block), and q = (int*)(int)p.
// In our memory model, q is a physical pointer, but p may be a
// logical pointer. However, rp is always a physical pointer by definition.
// Therefore, replacing `use (p)` with `use (rp)` may change behavior.
// To validate this replacement, `c` should be poison, so branch on poison can 
// happen. However, by C standard, c must not be poison because (p == q) is true
// in C, so `call llvm.pcmp (==, p, q)` in IR must be true as well.
//
// This pass is just a temporary one (to implement insertion of restrict before
// PLDI due) which is designed to run just after the first -mem2reg.
// This issue is discussed with Gil and must be written to the draft file.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/LowerPCmpIntrinsic.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Scalar.h"
using namespace llvm;

#define DEBUG_TYPE "lowerpcmp"

static bool LowerPCmpCmpXchgInst(AtomicCmpXchgInst *CXI) {
  IRBuilder<> Builder(CXI);
  Value *Ptr = CXI->getPointerOperand();
  Value *Cmp = CXI->getCompareOperand();
  Value *Val = CXI->getNewValOperand();

  LoadInst *Orig = Builder.CreateLoad(Ptr);
  Value *Equal = Builder.CreateICmpEQ(Orig, Cmp);
  Value *Res = Builder.CreateSelect(Equal, Val, Orig);
  Builder.CreateStore(Res, Ptr);

  Res = Builder.CreateInsertValue(UndefValue::get(CXI->getType()), Orig, 0);
  Res = Builder.CreateInsertValue(Res, Equal, 1);

  CXI->replaceAllUsesWith(Res);
  CXI->eraseFromParent();
  return true;
}

static bool runOnBasicBlock(BasicBlock &BB, DominatorTree &DT) {
  bool Changed = false;
  for (BasicBlock::iterator DI = BB.begin(), DE = BB.end(); DI != DE;) {
    Instruction *Inst = &*DI++;
    IntrinsicInst *II = dyn_cast<IntrinsicInst>(Inst);
    if (!II) continue;
    if (II->getIntrinsicID() != Intrinsic::pcmp) continue;

    IRBuilder<> Builder(II);
    uint64_t Pred = dyn_cast<ConstantInt>(II->getArgOperand(0))->
                    getUniqueInteger().getLimitedValue();
    Value *P = II->getArgOperand(1);
    Value *Q = II->getArgOperand(2);
    Module *M = II->getModule();

    auto createRestrictCall = [M, &Builder](Value *P, Value *Q) {
      Type *Tys[3] = { P->getType(), P->getType(), Q->getType() };
      Value *Args[2] = { P, Q };
      Value *F = Intrinsic::getDeclaration(M, Intrinsic::restrict, Tys);
      Value *RP = Builder.CreateCall(F, Args, P->getName() + ".restrict");
      return RP;
    };

    // Restrict arguments.
    Value *RP = createRestrictCall(P, Q);
    Value *RQ = createRestrictCall(Q, P);
    
    // Now create ICmp.
    Value *Cmp = Builder.CreateICmp((CmpInst::Predicate)Pred, RP, RQ);
    Cmp->takeName(II);
    // Replace all uses of llvm.pcmp(p,q) with icmp(restrict(p,q), restrict(q,p))!
    II->replaceAllUsesWith(Cmp);

    auto replaceDominatedUsesOnly = [&DT](Value *V, Value *ReplaceTo, Instruction *Dominator) {
      for (auto itr = V->user_begin(), iend = V->user_end(); itr != iend; itr++) {
        Value *U = *itr;
        Instruction *IU = dyn_cast<Instruction>(U);
        if (!IU)
          // Is this plausible?
          continue;
        if (DT.dominates(Dominator, IU)) {
          // replace!
          IU->replaceUsesOfWith(V, ReplaceTo);
        }
      }
    };
    if (ICmpInst *CmpI = dyn_cast<ICmpInst>(Cmp)) {
      replaceDominatedUsesOnly(P, RP, CmpI);
      replaceDominatedUsesOnly(Q, RQ, CmpI);
    }
  }
  return Changed;
}

static bool lowerPCmp(Function &F, DominatorTree &DT) {
  bool Changed = false;
  for (BasicBlock &BB : F) {
    Changed |= runOnBasicBlock(BB, DT);
  }
  return Changed;
}

PreservedAnalyses LowerPCmpIntrinsicPass::run(Function &F, FunctionAnalysisManager &FAM) {
  auto &DT = FAM.getResult<DominatorTreeAnalysis>(F);
  if (lowerPCmp(F, DT))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

namespace {
class LowerPCmpIntrinsic : public FunctionPass {
public:
  static char ID;
  LowerPCmpIntrinsic() : FunctionPass(ID) {
    initializeLowerPCmpIntrinsicPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override {
    if (skipFunction(F))
      return false;
    FunctionAnalysisManager DummyFAM;
    auto PA = Impl.run(F, DummyFAM);
    return !PA.areAllPreserved();
  }
private:
  LowerPCmpIntrinsicPass Impl;
};

}

char LowerPCmpIntrinsic::ID = 0;
INITIALIZE_PASS(LowerPCmpIntrinsic, "lower-pcmp",
                "Lower pcmp intrinsics to icmp+restrict", false, false)

FunctionPass *llvm::createLowerPCmpIntrinsicPass() {
  return new LowerPCmpIntrinsic();
}
