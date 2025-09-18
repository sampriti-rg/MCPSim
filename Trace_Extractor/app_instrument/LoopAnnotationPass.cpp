// clang++ -shared -fPIC LoopAnnotationPass.cpp -o LoopAnnotationPass.so `llvm-config --cxxflags --ldflags --libs`
// opt -S -load ./LoopAnnotationPass.so -loop-annotation < ir.ll  > inst.ll

#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/LoopInfoImpl.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"


using namespace llvm;

namespace {
struct LoopAnnotationPass : public ModulePass {
    static char ID;
    LoopAnnotationPass() : ModulePass(ID) {}

    struct RegionData {
        Loop *L;
        BasicBlock *PreHeader;
        BasicBlock *Header;
        SmallVector<BasicBlock *, 100> Latches;
        SmallVector<Instruction *, 100> Terminators;
        SmallVector<BasicBlock *, 100> ExitBlocks;
    };

    std::vector<RegionData> StoredRegions;

    bool runOnModule(Module &M) override {
        FunctionCallee BeginHook = M.getOrInsertFunction(
            "roi_region_begin",
            FunctionType::get(Type::getVoidTy(M.getContext()),
                                {Type::getInt32Ty(M.getContext())},
                                false));

        FunctionCallee EndHook = M.getOrInsertFunction(
            "roi_region_end",
            FunctionType::get(Type::getVoidTy(M.getContext()),
                                {Type::getInt32Ty(M.getContext())},
                                false));

        for (Function &F : M) {
            if (F.isDeclaration()) continue;
            auto &LI = getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();
            for (Loop *L : LI) {
                if (L->getParentLoop() == nullptr) {
                    BasicBlock *LatchTest = L->getLoopLatch();
                    if (L->getLoopPreheader() && L->getHeader() && LatchTest) {
                        RegionData LD;
                        LD.L = L;
                        LD.PreHeader = L->getLoopPreheader();
                        LD.Header = L->getHeader();
                        L->getLoopLatches(LD.Latches);
                        for (BasicBlock *BB : L->getBlocks()) {
                            if (Instruction *Term = BB->getTerminator()) {
                                LD.Terminators.push_back(Term);
                            }
                        }
                        L->getExitBlocks(LD.ExitBlocks);
                        StoredRegions.push_back(LD);
                    }
                }
            }
        }

        int LOOP_ID = 1;
        for (auto it = StoredRegions.rbegin(); it != StoredRegions.rend(); ++it) {
            auto &LD = *it;
            if (!LD.L->getParentLoop()) { 
                IRBuilder<> Builder(LD.PreHeader->getTerminator());
                Builder.CreateCall(BeginHook, {Builder.getInt32(LOOP_ID)});
                for (auto *ExitBlock : LD.ExitBlocks) {
                    IRBuilder<> AfterBuilder(ExitBlock->getTerminator());
                    AfterBuilder.CreateCall(EndHook, {AfterBuilder.getInt32(LOOP_ID)});
                }
            }
            ++LOOP_ID;
        }
        return true;
    }

    void getAnalysisUsage(AnalysisUsage &AU) const override {
        AU.addRequired<LoopInfoWrapperPass>();
        AU.setPreservesAll();
    }
};
} // namespace

char LoopAnnotationPass::ID = 0;
static RegisterPass<LoopAnnotationPass> X("loop-annotation", "Loop Annotation Module Pass", false, false);
