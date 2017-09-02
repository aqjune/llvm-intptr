//===-- LowerPCmpIntrinsic.h - Lower @llvm.pcmp -----------------*- C++ -*-===//
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
// uses of 'p' after if/while/for statement on condition 'p pred q' is met.
// However implementation of this is nontrivial because C syntax is not in
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

#ifndef LLVM_TRANSFORMS_SCALAR_LOWERPCMPINTRINSIC_H
#define LLVM_TRANSFORMS_SCALAR_LOWERPCMPINTRINSIC_H

#include "llvm/IR/Function.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

/// Move instructions into successor blocks when possible.
class LowerPCmpIntrinsicPass : public PassInfoMixin<LowerPCmpIntrinsicPass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};
}

#endif // LLVM_TRANSFORMS_SCALAR_LOWERPCMPINTRINSIC_H
