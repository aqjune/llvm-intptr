//===- MergeFunctions.cpp - Merge identical functions ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass looks for equivalent functions that are mergable and folds them.
//
// Order relation is defined on set of functions. It was made through
// special function comparison procedure that returns
// 0 when functions are equal,
// -1 when Left function is less than right function, and
// 1 for opposite case. We need total-ordering, so we need to maintain
// four properties on the functions set:
// a <= a (reflexivity)
// if a <= b and b <= a then a = b (antisymmetry)
// if a <= b and b <= c then a <= c (transitivity).
// for all a and b: a <= b or b <= a (totality).
//
// Comparison iterates through each instruction in each basic block.
// Functions are kept on binary tree. For each new function F we perform
// lookup in binary tree.
// In practice it works the following way:
// -- We define Function* container class with custom "operator<" (FunctionPtr).
// -- "FunctionPtr" instances are stored in std::set collection, so every
//    std::set::insert operation will give you result in log(N) time.
// 
// As an optimization, a hash of the function structure is calculated first, and
// two functions are only compared if they have the same hash. This hash is
// cheap to compute, and has the property that if function F == G according to
// the comparison function, then hash(F) == hash(G). This consistency property
// is critical to ensuring all possible merging opportunities are exploited.
// Collisions in the hash affect the speed of the pass but not the correctness
// or determinism of the resulting transformation.
//
// When a match is found the functions are folded. If both functions are
// overridable, we move the functionality into a new internal function and
// leave two overridable thunks to it.
//
//===----------------------------------------------------------------------===//
//
// Future work:
//
// * virtual functions.
//
// Many functions have their address taken by the virtual function table for
// the object they belong to. However, as long as it's only used for a lookup
// and call, this is irrelevant, and we'd like to fold such functions.
//
// * be smarter about bitcasts.
//
// In order to fold functions, we will sometimes add either bitcast instructions
// or bitcast constant expressions. Unfortunately, this can confound further
// analysis since the two functions differ where one has a bitcast and the
// other doesn't. We should learn to look through bitcasts.
//
// * Compare complex types with pointer types inside.
// * Compare cross-reference cases.
// * Compare complex expressions.
//
// All the three issues above could be described as ability to prove that
// fA == fB == fC == fE == fF == fG in example below:
//
//  void fA() {
//    fB();
//  }
//  void fB() {
//    fA();
//  }
//
//  void fE() {
//    fF();
//  }
//  void fF() {
//    fG();
//  }
//  void fG() {
//    fE();
//  }
//
// Simplest cross-reference case (fA <--> fB) was implemented in previous
// versions of MergeFunctions, though it presented only in two function pairs
// in test-suite (that counts >50k functions)
// Though possibility to detect complex cross-referencing (e.g.: A->B->C->D->A)
// could cover much more cases.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Utils/FunctionComparator.h"
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "mergefunc"

STATISTIC(NumFunctionsMerged, "Number of functions merged");
STATISTIC(NumThunksWritten, "Number of thunks generated");
STATISTIC(NumAliasesWritten, "Number of aliases generated");
STATISTIC(NumDoubleWeak, "Number of new functions created");

static cl::opt<unsigned> NumFunctionsForSanityCheck(
    "mergefunc-sanity",
    cl::desc("How many functions in module could be used for "
             "MergeFunctions pass sanity check. "
             "'0' disables this check. Works only with '-debug' key."),
    cl::init(0), cl::Hidden);

namespace {

class FunctionNode {
  mutable AssertingVH<Function> F;
  FunctionComparator::FunctionHash Hash;
public:
  // Note the hash is recalculated potentially multiple times, but it is cheap.
  FunctionNode(Function *F)
    : F(F), Hash(FunctionComparator::functionHash(*F))  {}
  Function *getFunc() const { return F; }
  FunctionComparator::FunctionHash getHash() const { return Hash; }

  /// Replace the reference to the function F by the function G, assuming their
  /// implementations are equal.
  void replaceBy(Function *G) const {
    F = G;
  }

  void release() { F = nullptr; }
};

/// MergeFunctions finds functions which will generate identical machine code,
/// by considering all pointer types to be equivalent. Once identified,
/// MergeFunctions will fold them by replacing a call to one to a call to a
/// bitcast of the other.
///
class MergeFunctions : public ModulePass {
public:
  static char ID;
  MergeFunctions()
    : ModulePass(ID), FnTree(FunctionNodeCmp(&GlobalNumbers)), FNodesInTree(),
      HasGlobalAliases(false) {
    initializeMergeFunctionsPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;

private:
  // The function comparison operator is provided here so that FunctionNodes do
  // not need to become larger with another pointer.
  class FunctionNodeCmp {
    GlobalNumberState* GlobalNumbers;
  public:
    FunctionNodeCmp(GlobalNumberState* GN) : GlobalNumbers(GN) {}
    bool operator()(const FunctionNode &LHS, const FunctionNode &RHS) const {
      // Order first by hashes, then full function comparison.
      if (LHS.getHash() != RHS.getHash())
        return LHS.getHash() < RHS.getHash();
      FunctionComparator FCmp(LHS.getFunc(), RHS.getFunc(), GlobalNumbers);
      return FCmp.compare() == -1;
    }
  };
  typedef std::set<FunctionNode, FunctionNodeCmp> FnTreeType;

  GlobalNumberState GlobalNumbers;

  /// A work queue of functions that may have been modified and should be
  /// analyzed again.
  std::vector<WeakVH> Deferred;

  /// Checks the rules of order relation introduced among functions set.
  /// Returns true, if sanity check has been passed, and false if failed.
  bool doSanityCheck(std::vector<WeakVH> &Worklist);

  /// Insert a ComparableFunction into the FnTree, or merge it away if it's
  /// equal to one that's already present.
  bool insert(Function *NewFunction);

  /// Remove a Function from the FnTree and queue it up for a second sweep of
  /// analysis.
  void remove(Function *F);

  /// Find the functions that use this Value and remove them from FnTree and
  /// queue the functions.
  void removeUsers(Value *V);

  /// Replace all direct calls of Old with calls of New. Will bitcast New if
  /// necessary to make types match.
  void replaceDirectCallers(Function *Old, Function *New);

  /// Merge two equivalent functions. Upon completion, G may be deleted, or may
  /// be converted into a thunk. In either case, it should never be visited
  /// again.
  void mergeTwoFunctions(Function *F, Function *G);

  /// Replace G with a thunk or an alias to F. Deletes G.
  void writeThunkOrAlias(Function *F, Function *G);

  /// Replace G with a simple tail call to bitcast(F). Also replace direct uses
  /// of G with bitcast(F). Deletes G.
  void writeThunk(Function *F, Function *G);

  /// Replace G with an alias to F. Deletes G.
  void writeAlias(Function *F, Function *G);

  /// Replace function F with function G in the function tree.
  void replaceFunctionInTree(const FunctionNode &FN, Function *G);

  /// The set of all distinct functions. Use the insert() and remove() methods
  /// to modify it. The map allows efficient lookup and deferring of Functions.
  FnTreeType FnTree;
  // Map functions to the iterators of the FunctionNode which contains them
  // in the FnTree. This must be updated carefully whenever the FnTree is
  // modified, i.e. in insert(), remove(), and replaceFunctionInTree(), to avoid
  // dangling iterators into FnTree. The invariant that preserves this is that
  // there is exactly one mapping F -> FN for each FunctionNode FN in FnTree.
  ValueMap<Function*, FnTreeType::iterator> FNodesInTree;

  /// Whether or not the target supports global aliases.
  bool HasGlobalAliases;
};

} // end anonymous namespace

char MergeFunctions::ID = 0;
INITIALIZE_PASS(MergeFunctions, "mergefunc", "Merge Functions", false, false)

ModulePass *llvm::createMergeFunctionsPass() {
  return new MergeFunctions();
}

bool MergeFunctions::doSanityCheck(std::vector<WeakVH> &Worklist) {
  if (const unsigned Max = NumFunctionsForSanityCheck) {
    unsigned TripleNumber = 0;
    bool Valid = true;

    dbgs() << "MERGEFUNC-SANITY: Started for first " << Max << " functions.\n";

    unsigned i = 0;
    for (std::vector<WeakVH>::iterator I = Worklist.begin(), E = Worklist.end();
         I != E && i < Max; ++I, ++i) {
      unsigned j = i;
      for (std::vector<WeakVH>::iterator J = I; J != E && j < Max; ++J, ++j) {
        Function *F1 = cast<Function>(*I);
        Function *F2 = cast<Function>(*J);
        int Res1 = FunctionComparator(F1, F2, &GlobalNumbers).compare();
        int Res2 = FunctionComparator(F2, F1, &GlobalNumbers).compare();

        // If F1 <= F2, then F2 >= F1, otherwise report failure.
        if (Res1 != -Res2) {
          dbgs() << "MERGEFUNC-SANITY: Non-symmetric; triple: " << TripleNumber
                 << "\n";
          F1->dump();
          F2->dump();
          Valid = false;
        }

        if (Res1 == 0)
          continue;

        unsigned k = j;
        for (std::vector<WeakVH>::iterator K = J; K != E && k < Max;
             ++k, ++K, ++TripleNumber) {
          if (K == J)
            continue;

          Function *F3 = cast<Function>(*K);
          int Res3 = FunctionComparator(F1, F3, &GlobalNumbers).compare();
          int Res4 = FunctionComparator(F2, F3, &GlobalNumbers).compare();

          bool Transitive = true;

          if (Res1 != 0 && Res1 == Res4) {
            // F1 > F2, F2 > F3 => F1 > F3
            Transitive = Res3 == Res1;
          } else if (Res3 != 0 && Res3 == -Res4) {
            // F1 > F3, F3 > F2 => F1 > F2
            Transitive = Res3 == Res1;
          } else if (Res4 != 0 && -Res3 == Res4) {
            // F2 > F3, F3 > F1 => F2 > F1
            Transitive = Res4 == -Res1;
          }

          if (!Transitive) {
            dbgs() << "MERGEFUNC-SANITY: Non-transitive; triple: "
                   << TripleNumber << "\n";
            dbgs() << "Res1, Res3, Res4: " << Res1 << ", " << Res3 << ", "
                   << Res4 << "\n";
            F1->dump();
            F2->dump();
            F3->dump();
            Valid = false;
          }
        }
      }
    }

    dbgs() << "MERGEFUNC-SANITY: " << (Valid ? "Passed." : "Failed.") << "\n";
    return Valid;
  }
  return true;
}

bool MergeFunctions::runOnModule(Module &M) {
  if (skipModule(M))
    return false;

  bool Changed = false;

  // All functions in the module, ordered by hash. Functions with a unique
  // hash value are easily eliminated.
  std::vector<std::pair<FunctionComparator::FunctionHash, Function *>>
    HashedFuncs;
  for (Function &Func : M) {
    if (!Func.isDeclaration() && !Func.hasAvailableExternallyLinkage()) {
      HashedFuncs.push_back({FunctionComparator::functionHash(Func), &Func});
    } 
  }

  std::stable_sort(
      HashedFuncs.begin(), HashedFuncs.end(),
      [](const std::pair<FunctionComparator::FunctionHash, Function *> &a,
         const std::pair<FunctionComparator::FunctionHash, Function *> &b) {
        return a.first < b.first;
      });

  auto S = HashedFuncs.begin();
  for (auto I = HashedFuncs.begin(), IE = HashedFuncs.end(); I != IE; ++I) {
    // If the hash value matches the previous value or the next one, we must
    // consider merging it. Otherwise it is dropped and never considered again.
    if ((I != S && std::prev(I)->first == I->first) ||
        (std::next(I) != IE && std::next(I)->first == I->first) ) {
      Deferred.push_back(WeakVH(I->second));
    }
  }
  
  do {
    std::vector<WeakVH> Worklist;
    Deferred.swap(Worklist);

    DEBUG(doSanityCheck(Worklist));

    DEBUG(dbgs() << "size of module: " << M.size() << '\n');
    DEBUG(dbgs() << "size of worklist: " << Worklist.size() << '\n');

    // Insert functions and merge them.
    for (WeakVH &I : Worklist) {
      if (!I)
        continue;
      Function *F = cast<Function>(I);
      if (!F->isDeclaration() && !F->hasAvailableExternallyLinkage()) {
        Changed |= insert(F);
      }
    }
    DEBUG(dbgs() << "size of FnTree: " << FnTree.size() << '\n');
  } while (!Deferred.empty());

  FnTree.clear();
  GlobalNumbers.clear();

  return Changed;
}

// Replace direct callers of Old with New.
void MergeFunctions::replaceDirectCallers(Function *Old, Function *New) {
  Constant *BitcastNew = ConstantExpr::getBitCast(New, Old->getType());
  for (auto UI = Old->use_begin(), UE = Old->use_end(); UI != UE;) {
    Use *U = &*UI;
    ++UI;
    CallSite CS(U->getUser());
    if (CS && CS.isCallee(U)) {
      // Transfer the called function's attributes to the call site. Due to the
      // bitcast we will 'lose' ABI changing attributes because the 'called
      // function' is no longer a Function* but the bitcast. Code that looks up
      // the attributes from the called function will fail.

      // FIXME: This is not actually true, at least not anymore. The callsite
      // will always have the same ABI affecting attributes as the callee,
      // because otherwise the original input has UB. Note that Old and New
      // always have matching ABI, so no attributes need to be changed.
      // Transferring other attributes may help other optimizations, but that
      // should be done uniformly and not in this ad-hoc way.
      auto &Context = New->getContext();
      auto NewFuncAttrs = New->getAttributes();
      auto CallSiteAttrs = CS.getAttributes();

      CallSiteAttrs = CallSiteAttrs.addAttributes(
          Context, AttributeSet::ReturnIndex, NewFuncAttrs.getRetAttributes());

      for (unsigned argIdx = 0; argIdx < CS.arg_size(); argIdx++) {
        AttributeSet Attrs = NewFuncAttrs.getParamAttributes(argIdx);
        if (Attrs.getNumSlots())
          CallSiteAttrs = CallSiteAttrs.addAttributes(Context, argIdx, Attrs);
      }

      CS.setAttributes(CallSiteAttrs);

      remove(CS.getInstruction()->getParent()->getParent());
      U->set(BitcastNew);
    }
  }
}

// Replace G with an alias to F if possible, or else a thunk to F. Deletes G.
void MergeFunctions::writeThunkOrAlias(Function *F, Function *G) {
  if (HasGlobalAliases && G->hasGlobalUnnamedAddr()) {
    if (G->hasExternalLinkage() || G->hasLocalLinkage() ||
        G->hasWeakLinkage()) {
      writeAlias(F, G);
      return;
    }
  }

  writeThunk(F, G);
}

// Helper for writeThunk,
// Selects proper bitcast operation,
// but a bit simpler then CastInst::getCastOpcode.
static Value *createCast(IRBuilder<> &Builder, Value *V, Type *DestTy) {
  Type *SrcTy = V->getType();
  if (SrcTy->isStructTy()) {
    assert(DestTy->isStructTy());
    assert(SrcTy->getStructNumElements() == DestTy->getStructNumElements());
    Value *Result = UndefValue::get(DestTy);
    for (unsigned int I = 0, E = SrcTy->getStructNumElements(); I < E; ++I) {
      Value *Element = createCast(
          Builder, Builder.CreateExtractValue(V, makeArrayRef(I)),
          DestTy->getStructElementType(I));

      Result =
          Builder.CreateInsertValue(Result, Element, makeArrayRef(I));
    }
    return Result;
  }
  assert(!DestTy->isStructTy());
  if (SrcTy->isIntegerTy() && DestTy->isPointerTy())
    return Builder.CreateNewIntToPtr(V, DestTy);
  else if (SrcTy->isPointerTy() && DestTy->isIntegerTy()) {
    return Builder.CreateNewPtrToInt(V, DestTy);
  } else
    return Builder.CreateBitCast(V, DestTy);
}

// Replace G with a simple tail call to bitcast(F). Also replace direct uses
// of G with bitcast(F). Deletes G.
void MergeFunctions::writeThunk(Function *F, Function *G) {
  if (!G->isInterposable()) {
    // Redirect direct callers of G to F.
    replaceDirectCallers(G, F);
  }

  // If G was internal then we may have replaced all uses of G with F. If so,
  // stop here and delete G. There's no need for a thunk.
  if (G->hasLocalLinkage() && G->use_empty()) {
    G->eraseFromParent();
    return;
  }

  Function *NewG = Function::Create(G->getFunctionType(), G->getLinkage(), "",
                                    G->getParent());
  BasicBlock *BB = BasicBlock::Create(F->getContext(), "", NewG);
  IRBuilder<> Builder(BB);

  SmallVector<Value *, 16> Args;
  unsigned i = 0;
  FunctionType *FFTy = F->getFunctionType();
  for (Argument & AI : NewG->args()) {
    Args.push_back(createCast(Builder, &AI, FFTy->getParamType(i)));
    ++i;
  }

  CallInst *CI = Builder.CreateCall(F, Args);
  CI->setTailCall();
  CI->setCallingConv(F->getCallingConv());
  CI->setAttributes(F->getAttributes());
  if (NewG->getReturnType()->isVoidTy()) {
    Builder.CreateRetVoid();
  } else {
    Builder.CreateRet(createCast(Builder, CI, NewG->getReturnType()));
  }

  NewG->copyAttributesFrom(G);
  NewG->takeName(G);
  removeUsers(G);
  G->replaceAllUsesWith(NewG);
  G->eraseFromParent();

  DEBUG(dbgs() << "writeThunk: " << NewG->getName() << '\n');
  ++NumThunksWritten;
}

// Replace G with an alias to F and delete G.
void MergeFunctions::writeAlias(Function *F, Function *G) {
  auto *GA = GlobalAlias::create(G->getLinkage(), "", F);
  F->setAlignment(std::max(F->getAlignment(), G->getAlignment()));
  GA->takeName(G);
  GA->setVisibility(G->getVisibility());
  removeUsers(G);
  G->replaceAllUsesWith(GA);
  G->eraseFromParent();

  DEBUG(dbgs() << "writeAlias: " << GA->getName() << '\n');
  ++NumAliasesWritten;
}

// Merge two equivalent functions. Upon completion, Function G is deleted.
void MergeFunctions::mergeTwoFunctions(Function *F, Function *G) {
  if (F->isInterposable()) {
    assert(G->isInterposable());

    // Make them both thunks to the same internal function.
    Function *H = Function::Create(F->getFunctionType(), F->getLinkage(), "",
                                   F->getParent());
    H->copyAttributesFrom(F);
    H->takeName(F);
    removeUsers(F);
    F->replaceAllUsesWith(H);

    unsigned MaxAlignment = std::max(G->getAlignment(), H->getAlignment());

    if (HasGlobalAliases) {
      writeAlias(F, G);
      writeAlias(F, H);
    } else {
      writeThunk(F, G);
      writeThunk(F, H);
    }

    F->setAlignment(MaxAlignment);
    F->setLinkage(GlobalValue::PrivateLinkage);
    ++NumDoubleWeak;
  } else {
    writeThunkOrAlias(F, G);
  }

  ++NumFunctionsMerged;
}

/// Replace function F by function G.
void MergeFunctions::replaceFunctionInTree(const FunctionNode &FN,
                                           Function *G) {
  Function *F = FN.getFunc();
  assert(FunctionComparator(F, G, &GlobalNumbers).compare() == 0 &&
         "The two functions must be equal");
  
  auto I = FNodesInTree.find(F);
  assert(I != FNodesInTree.end() && "F should be in FNodesInTree");
  assert(FNodesInTree.count(G) == 0 && "FNodesInTree should not contain G");
  
  FnTreeType::iterator IterToFNInFnTree = I->second;
  assert(&(*IterToFNInFnTree) == &FN && "F should map to FN in FNodesInTree.");
  // Remove F -> FN and insert G -> FN
  FNodesInTree.erase(I);
  FNodesInTree.insert({G, IterToFNInFnTree});
  // Replace F with G in FN, which is stored inside the FnTree.
  FN.replaceBy(G);
}

// Insert a ComparableFunction into the FnTree, or merge it away if equal to one
// that was already inserted.
bool MergeFunctions::insert(Function *NewFunction) {
  std::pair<FnTreeType::iterator, bool> Result =
      FnTree.insert(FunctionNode(NewFunction));

  if (Result.second) {
    assert(FNodesInTree.count(NewFunction) == 0);
    FNodesInTree.insert({NewFunction, Result.first});
    DEBUG(dbgs() << "Inserting as unique: " << NewFunction->getName() << '\n');
    return false;
  }

  const FunctionNode &OldF = *Result.first;

  // Don't merge tiny functions, since it can just end up making the function
  // larger.
  // FIXME: Should still merge them if they are unnamed_addr and produce an
  // alias.
  if (NewFunction->size() == 1) {
    if (NewFunction->front().size() <= 2) {
      DEBUG(dbgs() << NewFunction->getName()
                   << " is to small to bother merging\n");
      return false;
    }
  }

  // Impose a total order (by name) on the replacement of functions. This is
  // important when operating on more than one module independently to prevent
  // cycles of thunks calling each other when the modules are linked together.
  //
  // First of all, we process strong functions before weak functions.
  if ((OldF.getFunc()->isInterposable() && !NewFunction->isInterposable()) ||
     (OldF.getFunc()->isInterposable() == NewFunction->isInterposable() &&
       OldF.getFunc()->getName() > NewFunction->getName())) {
    // Swap the two functions.
    Function *F = OldF.getFunc();
    replaceFunctionInTree(*Result.first, NewFunction);
    NewFunction = F;
    assert(OldF.getFunc() != F && "Must have swapped the functions.");
  }

  DEBUG(dbgs() << "  " << OldF.getFunc()->getName()
               << " == " << NewFunction->getName() << '\n');

  Function *DeleteF = NewFunction;
  mergeTwoFunctions(OldF.getFunc(), DeleteF);
  return true;
}

// Remove a function from FnTree. If it was already in FnTree, add
// it to Deferred so that we'll look at it in the next round.
void MergeFunctions::remove(Function *F) {
  auto I = FNodesInTree.find(F);
  if (I != FNodesInTree.end()) {
    DEBUG(dbgs() << "Deferred " << F->getName()<< ".\n");
    FnTree.erase(I->second);
    // I->second has been invalidated, remove it from the FNodesInTree map to
    // preserve the invariant.
    FNodesInTree.erase(I);
    Deferred.emplace_back(F);
  }
}

// For each instruction used by the value, remove() the function that contains
// the instruction. This should happen right before a call to RAUW.
void MergeFunctions::removeUsers(Value *V) {
  std::vector<Value *> Worklist;
  Worklist.push_back(V);
  SmallSet<Value*, 8> Visited;
  Visited.insert(V);
  while (!Worklist.empty()) {
    Value *V = Worklist.back();
    Worklist.pop_back();

    for (User *U : V->users()) {
      if (Instruction *I = dyn_cast<Instruction>(U)) {
        remove(I->getParent()->getParent());
      } else if (isa<GlobalValue>(U)) {
        // do nothing
      } else if (Constant *C = dyn_cast<Constant>(U)) {
        for (User *UU : C->users()) {
          if (!Visited.insert(UU).second)
            Worklist.push_back(UU);
        }
      }
    }
  }
}
