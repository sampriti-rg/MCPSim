// LoopBoundsPass.cpp
// LLVM 10 legacy pass to instrument loops with printLoopBounds(lower, upper)

#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/IR/Dominators.h"

using namespace llvm;

namespace {

struct LoopBoundsInstrumentation : public FunctionPass {
  static char ID;
  LoopBoundsInstrumentation() : FunctionPass(ID) {}

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // We require LoopInfo and DominatorTree. We will modify CFG by inserting preheaders.
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
    // We don't claim to preserve analyses because we change CFG (preheader insertion).
  }

private:
  // Cast a Value* to i32 using sign-extend/trunc when integer; otherwise return i32(0)
  Value *castToI32(Value *V, IRBuilder<> &B) {
    LLVMContext &Ctx = B.getContext();
    Type *I32Ty = Type::getInt32Ty(Ctx);

    if (V->getType()->isIntegerTy()) {
      unsigned srcBits = V->getType()->getIntegerBitWidth();
      if (srcBits == 32) {
        return V;
      } else if (srcBits < 32) {
        return B.CreateSExt(V, I32Ty, "to_i32_sext");
      } else {
        return B.CreateTrunc(V, I32Ty, "to_i32_trunc");
      }
    }

    // If it's a pointer or float or other non-int, fallback to constant 0
    return ConstantInt::get(I32Ty, 0);
  }

  // Process one loop: ensure preheader exists, find induction PHI and icmp, then insert call
  bool processLoop(Loop *L, Module &M, LoopInfo &LI, DominatorTree &DT) {
    BasicBlock *header = L->getHeader();
    if (!header) return false;

    // Ensure preheader exists; if not, insert one. Use LLVM 10 signature:
    // BasicBlock *InsertPreheaderForLoop(Loop *L, DominatorTree *DT, LoopInfo *LI,
    //                                    MemorySSAUpdater *MSSAU, bool PreserveLCSSA);
    BasicBlock *preheader = L->getLoopPreheader();
    if (!preheader) {
      errs() << "LoopBoundsPass: creating preheader for loop header '"
             << header->getName() << "' in function '"
             << header->getParent()->getName() << "'\n";
      preheader = InsertPreheaderForLoop(L, &DT, &LI, /*MSSAU*/ nullptr, /*PreserveLCSSA*/ false);
      if (!preheader) {
        errs() << "LoopBoundsPass: failed to create preheader\n";
        return false;
      }
    }

    // Robustly find an induction PHI: one incoming from outside loop and one incoming from inside
    PHINode *indPhi = nullptr;
    for (PHINode &PN : header->phis()) {
      BasicBlock *incomingOut = nullptr;
      BasicBlock *incomingIn = nullptr;

      for (unsigned i = 0; i < PN.getNumIncomingValues(); ++i) {
        BasicBlock *incBB = PN.getIncomingBlock(i);
        if (!incBB) continue;
        if (L->contains(incBB))
          incomingIn = incBB;
        else
          incomingOut = incBB;
      }

      if (incomingOut && incomingIn) {
        indPhi = &PN;
        break;
      }
    }

    if (!indPhi) {
      // Nothing to instrument if we can't find a suitable PHI
      errs() << "LoopBoundsPass: no suitable PHI found in header '" << header->getName()
             << "' of function '" << header->getParent()->getName() << "'\n";
      return false;
    }

    // Find the lower bound value: incoming value from the preheader block
    Value *lowerVal = nullptr;
    for (unsigned i = 0; i < indPhi->getNumIncomingValues(); ++i) {
      BasicBlock *incBB = indPhi->getIncomingBlock(i);
      if (incBB == preheader) {
        lowerVal = indPhi->getIncomingValue(i);
        break;
      }
    }

    if (!lowerVal) {
      // If incoming from preheader not found (shouldn't happen if we created preheader),
      // try to find any incoming value outside the loop as fallback.
      for (unsigned i = 0; i < indPhi->getNumIncomingValues(); ++i) {
        BasicBlock *incBB = indPhi->getIncomingBlock(i);
        if (!L->contains(incBB)) {
          lowerVal = indPhi->getIncomingValue(i);
          break;
        }
      }
    }

    if (!lowerVal) {
      errs() << "LoopBoundsPass: couldn't determine lower bound for PHI in header '"
             << header->getName() << "'\n";
      return false;
    }

    // Find an ICmp in the header that uses the induction PHI to determine the upper bound.
    ICmpInst *icmp = nullptr;
    for (User *U : indPhi->users()) {
      if (ICmpInst *IC = dyn_cast<ICmpInst>(U)) {
        if (IC->getParent() == header) {
          icmp = IC;
          break;
        }
      }
    }

    Value *upperVal = nullptr;
    if (icmp) {
      // choose the operand that is not the phi
      if (icmp->getOperand(0) == indPhi)
        upperVal = icmp->getOperand(1);
      else if (icmp->getOperand(1) == indPhi)
        upperVal = icmp->getOperand(0);
    } else {
      // If no direct icmp found, try to find a compare in the header (fallback)
      for (Instruction &I : *header) {
        if (ICmpInst *IC = dyn_cast<ICmpInst>(&I)) {
          // if compare uses the PHI (possibly through a bitcast/other op), try direct operands
          if (IC->getOperand(0) == indPhi)
            upperVal = IC->getOperand(1);
          else if (IC->getOperand(1) == indPhi)
            upperVal = IC->getOperand(0);

          if (upperVal)
            break;
        }
      }
    }

    if (!upperVal) {
      errs() << "LoopBoundsPass: couldn't find upper bound (icmp) for loop header '"
             << header->getName() << "'. Instrumenting with upper=0 fallback.\n";
      // fallback to zero
      LLVMContext &Ctx = M.getContext();
      upperVal = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
    }

    // Prepare the print function declaration (extern "C" void printLoopBounds(int,int))
    LLVMContext &Ctx = M.getContext();
    FunctionCallee printFn = M.getOrInsertFunction(
        "printLoopBounds",
        FunctionType::get(Type::getVoidTy(Ctx),
                          {Type::getInt32Ty(Ctx), Type::getInt32Ty(Ctx)},
                          false));

    // Insert call in preheader before its terminator
    Instruction *insertPt = preheader->getTerminator();
    IRBuilder<> B(insertPt);

    Value *lowerI32 = castToI32(lowerVal, B);
    Value *upperI32 = castToI32(upperVal, B);

    SmallVector<Value *, 2> args;
    args.push_back(lowerI32);
    args.push_back(upperI32);

    B.CreateCall(printFn, args);

    return true;
  }

public:
  bool runOnFunction(Function &F) override {
    Module &M = *F.getParent();
    LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    DominatorTree &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();

    bool changed = false;

    // iterate top-level loops and include nested loops
    SmallVector<Loop *, 8> worklist;
    for (Loop *L : LI)
      worklist.push_back(L);

    while (!worklist.empty()) {
      Loop *L = worklist.pop_back_val();
      for (Loop *Sub : L->getSubLoops())
        worklist.push_back(Sub);

      changed |= processLoop(L, M, LI, DT);
    }

    return changed;
  }
};

} // end anonymous namespace

char LoopBoundsInstrumentation::ID = 0;
static RegisterPass<LoopBoundsInstrumentation> X(
    "loop-bounds-inst",
    "Instrument loops by printing lower and upper bounds (preheader call)",
    false, // only looks at CFG? (we modify) but keep false (not pure analysis)
    false  // not a pure analysis
);
