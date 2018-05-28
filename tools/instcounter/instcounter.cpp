#include <iostream>
#include <string>
#include <set>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/ErrorOr.h>
#include <llvm/Pass.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/FileSystem.h>

using namespace llvm;

namespace{
class InstCountPass : public FunctionPass, public InstVisitor<InstCountPass> {
  friend class InstVisitor<InstCountPass>;

  void visitFunction(Function &F) { 
    ++TotalFuncs;
  }
  void visitBasicBlock(BasicBlock &BB) { 
    ++TotalBlocks; 
  }

#define HANDLE_INST(N, OPCODE, CLASS) \
  void visit##OPCODE(CLASS &I) { ++NumInst[""#OPCODE]; ++TotalInsts; countSpecialInsts(&I); visitOperands(&I); }
#include <llvm/IR/Instruction.def>
  
  void visitInstruction(Instruction &I) {
    errs() << "Instruction Count does not know about " << I;
    llvm_unreachable(nullptr);
  }
  void countSpecialInsts(Instruction *I) {
    if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(I)) {
      if (II->getIntrinsicID() == Intrinsic::psub)
        PSubCount++;
    } else if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(I)) {
      if (GEPI->isInBounds())
        GEPInboundsCount++;
    }
  }
  void visitOperands(User *U) {
    if (isa<ConstantExpr>(U)) {
      ConstantExpr *CE = dyn_cast<ConstantExpr>(U);
      if (Visited.find(CE) != Visited.end()) return;
      Visited.insert(CE);
      switch (CE->getOpcode()) {
      case Instruction::IntToPtr:
        NumConstExpr["inttoptr"]++;
        break;
      case Instruction::PtrToInt:
        NumConstExpr["ptrtoint"]++;
        break;
      case Instruction::GetElementPtr:
        NumConstExpr["getelementptr"]++;
        if (dyn_cast<GEPOperator>(CE)->isInBounds())
          ConstExprGEPInboundsCount++;
        break;
      }
    }
    for (auto I = U->op_begin(); I != U->op_end(); ++I) {
      Value *V = *I;
      if (!isa<ConstantExpr>(V)) continue;
      visitOperands(dyn_cast<ConstantExpr>(V));
    }
  }
public:
  static char ID;
  InstCountPass():FunctionPass(ID) { }
  
  virtual bool runOnFunction(Function &F);
  void finalize();

  int TotalInsts = 0;
  int TotalFuncs = 0;
  int TotalBlocks = 0;

  std::set<ConstantExpr *> Visited;
  std::map<std::string, int> NumInst;
  std::map<std::string, int> NumConstExpr;
  int PSubCount = 0;
  int GEPInboundsCount = 0;
  int ConstExprGEPInboundsCount = 0;
};

bool InstCountPass::runOnFunction(Function &F) {
  visit(F);
  return false;
}
void InstCountPass::finalize() {
  // lower keys.
  std::map<std::string, int> NumInstLowered;
  for (auto itr = NumInst.begin(); itr != NumInst.end(); itr++) {
    std::string str = itr->first;
    std::string str2 = str;
    std::transform(str.begin(), str.end(), str2.begin(), ::tolower);
    int val = itr->second;
    NumInstLowered[str2] = val;
  }
  NumInst = NumInstLowered;
}
}

char InstCountPass::ID = 0;
static RegisterPass<InstCountPass> X("hello", "Hello World Pass",
                             false /* Only looks at CFG */,
                             false /* Analysis Pass */);



class Stat {
public:
  int total_i;
  int ptrtoint_i;
  int ptrtoint_cexpr;
  int gep_i;
  int gep_cexpr;
  int gep_inb_i;
  int gep_inb_cexpr;
  int inttoptr_i;
  int inttoptr_cexpr;
  int psub;

  Stat(): total_i(0), ptrtoint_i(0), ptrtoint_cexpr(0), gep_i(0), gep_cexpr(0),
    gep_inb_i(0), gep_inb_cexpr(0), inttoptr_i(0), inttoptr_cexpr(0),
    psub(0) {}

  Stat(InstCountPass *ip) {
    total_i = ip->TotalInsts;
    inttoptr_i = ip->NumInst["inttoptr"];
    ptrtoint_i = ip->NumInst["ptrtoint"];
    gep_i = ip->NumInst["getelementptr"];
    gep_inb_i = ip->GEPInboundsCount;
    psub = ip->PSubCount;
    inttoptr_cexpr = ip->NumConstExpr["inttoptr"];
    ptrtoint_cexpr = ip->NumConstExpr["ptrtoint"];
    gep_cexpr = ip->NumConstExpr["getelementptr"];
    gep_inb_cexpr = ip->ConstExprGEPInboundsCount;
  }

  Stat operator+(const Stat &s) {
    Stat ns;
#define UPDATE_FIELD(FNAME) ns.FNAME = FNAME + s.FNAME
    UPDATE_FIELD(total_i);
    UPDATE_FIELD(ptrtoint_i);
    UPDATE_FIELD(ptrtoint_cexpr);
    UPDATE_FIELD(gep_i);
    UPDATE_FIELD(gep_cexpr);
    UPDATE_FIELD(gep_inb_i);
    UPDATE_FIELD(gep_inb_cexpr);
    UPDATE_FIELD(inttoptr_i);
    UPDATE_FIELD(inttoptr_cexpr);
    UPDATE_FIELD(psub);
#undef UPDATE_FIELD
    return ns;
  }

  void print(bool distinguishConstAndInst) {
#define PRINT(LABEL, VALUE) std::cout << LABEL << " " << (VALUE) << std::endl
    PRINT("inst total", total_i);
    if (distinguishConstAndInst) {
      // Distinguish constexpr and inst.
      PRINT("inst inttoptr", inttoptr_i);
      PRINT("inst ptrtoint", ptrtoint_i);
      PRINT("inst getelementptr_all", gep_i);
      PRINT("inst getelementptr_inbounds", gep_inb_i);
      PRINT("inst psub", psub);
      PRINT("constexpr inttoptr", inttoptr_cexpr);
      PRINT("constexpr ptrtoint", ptrtoint_cexpr);
      PRINT("constexpr getelementptr_all", gep_cexpr);
      PRINT("constexpr getelementptr_inbounds", gep_inb_cexpr);
    } else {
      PRINT("inttoptr", inttoptr_i + inttoptr_cexpr);
      PRINT("ptrtoint", ptrtoint_i + ptrtoint_cexpr);
      PRINT("getelementptr_all", gep_i + gep_cexpr);
      PRINT("getelementptr_inbounds", gep_inb_i + gep_inb_cexpr);
      PRINT("psub", psub);
    }
#undef PRINT
  }
};

Stat totalstat;

void processModule(StringRef filename, LLVMContext &context, bool distinguishConstAndInst) {
  ErrorOr<std::unique_ptr<MemoryBuffer>> fileOrErr = 
    MemoryBuffer::getFileOrSTDIN(filename);
  if (std::error_code ec = fileOrErr.getError()) {
    errs() << "Error opening input file: " << ec.message() << "\n";
    exit(2);
  }
  ErrorOr<Expected<std::unique_ptr<llvm::Module>>> moduleOrErr = 
    parseBitcodeFile(fileOrErr.get()->getMemBufferRef(), context);
  if (std::error_code ec = moduleOrErr.getError()) {
    errs() << "Error reading module : " << ec.message() << "\n";
    exit(3);
  }

  Expected<std::unique_ptr<llvm::Module>> moduleExpct = std::move(moduleOrErr.get());
  std::unique_ptr<Module> m;
  if (moduleExpct) {
    m = std::move(moduleExpct.get());
  } else {
    errs() << "Error reading module\n";
    exit(3);
  }
  
  InstCountPass *ip = new InstCountPass();
  for (auto fitr = m->getFunctionList().begin(); 
      fitr != m->getFunctionList().end(); fitr++) {
    Function &f = *fitr;
    ip->runOnFunction(f);
  }
  ip->finalize();
  Stat news(ip);
  std::cout << "---- " << filename.str() << " -----\n";
  news.print(distinguishConstAndInst);
  totalstat = totalstat + news;
  delete ip;
}

int main(int argc, char *argv[]){
  if (argc != 3) {
    std::cerr << "Usage : " << argv[0] << " <distinguish-const-and-inst(y/n)> <dir>" << std::endl;
    return 1;
  }

  LLVMContext context;
  bool distinguishConstAndInst = argv[1][0] == 'y';
  
  std::string rootdir = argv[2];
  std::error_code ec;
  int cnt = 0;
  for (sys::fs::recursive_directory_iterator itr(rootdir, ec), iend;
       itr != iend && !ec; itr.increment(ec)) {
    std::string path = itr->path();
    int len = path.length();
    if (sys::fs::is_regular_file(path) && path.length() > 3 &&
        path.substr(len - 3, 3) == ".bc") {
      processModule(itr->path(), context, distinguishConstAndInst);
      cnt++;
    }
  }

  std::cout << "--- TOTAL " << cnt << " FILES ---" << "\n";
  totalstat.print(distinguishConstAndInst);

  return 0;
}
