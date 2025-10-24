#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/TypeSize.h"
#include "llvm/IR/Type.h"
#include <nlohmann/json.hpp>
#include "llvm/IR/IRBuilder.h"

#include <fstream>
#include <set>
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>
#include <cctype>

using namespace llvm;
using json = nlohmann::json;

namespace {
struct BBInfoPass : public ModulePass {
  static char ID;
  BBInfoPass() : ModulePass(ID) {}

  bool runOnModule(Module &M) override {
    const DataLayout &DL = M.getDataLayout();
    json resultJson;
    int BB_ID = 1;
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
      if (!F.isDeclaration() && F.getName() != "roi_region_begin" && F.getName() != "roi_region_end" && F.getName().find("magic_op") == std::string::npos && F.getName() != "mcp_roi_begin") {
        json functionJson;
        functionJson["FunctionName"] = F.getName().str();
        json basicBlocksJson = json::array();
        for (auto &BB : F) {
          size_t totalInstCount = 0;
          for (auto &I : BB) {
            totalInstCount++;
            if ((isa<ReturnInst>(&I) || isa<BranchInst>(&I) || isa<SwitchInst>(&I)) && totalInstCount == 1) {
              continue;
            }
          }
          std::set<const Value*> usedVars;
          std::set<const Value*> incomingVars;
          // Gather used variables: all non-constant operands in instructions in this BB
          for (const Instruction &I : BB) {
            for (unsigned i = 0, e = I.getNumOperands(); i != e; ++i) {
              const Value *op = I.getOperand(i);
              if (!op) continue;
              // Ignore pure Constants (ConstantInt, ConstantFP, ConstantExpr) but not Globals
              if (isa<Constant>(op) && !isa<GlobalValue>(op)) continue;
              usedVars.insert(op);
            }
          }
          // Determine incoming (live-in): used but defined outside this BB
          for (const Value *V : usedVars) {
            if (isa<Argument>(V)) {
              incomingVars.insert(V);
              continue;
            }
            if (const Instruction *DefI = dyn_cast<Instruction>(V)) {
              const BasicBlock *defBB = DefI->getParent();
              if (defBB != &BB) incomingVars.insert(V);
              continue;
            }
            if (isa<GlobalValue>(V)) {
              incomingVars.insert(V);
              continue;
            }
            incomingVars.insert(V);
          }
          
          uint64_t usedCount = usedVars.size();
          uint64_t incomingCount = incomingVars.size();
          uint64_t usedTotalSize = 0;
          uint64_t incomingTotalSize = 0;
          for (const Value *V : usedVars) {
            const Type *T = V->getType();
            uint64_t sz = typeSizeBytes(DL, T);
            usedTotalSize += sz;
          }
          for (const Value *V : incomingVars) {
            const Type *T = V->getType();
            uint64_t sz = typeSizeBytes(DL, T);
            incomingTotalSize += sz;
          }
          json bbJson;
          bbJson["BasicBlockID"] = BB_ID;
          bbJson["BasicBlockName"] = BB.getName().str();
          bbJson["IncommingData"] = incomingTotalSize;
          bbJson["UsedData"] = usedTotalSize;
          basicBlocksJson.push_back(bbJson);

          IRBuilder<> Builder(&*BB.getFirstInsertionPt());
          Builder.CreateCall(BeginHook, {Builder.getInt32(BB_ID)});
          for (auto &I : BB) {
            if (isa<ReturnInst>(&I) || isa<BranchInst>(&I) || isa<SwitchInst>(&I)) {
              IRBuilder<> AfterBuilder(&I);
              AfterBuilder.CreateCall(EndHook, {AfterBuilder.getInt32(BB_ID)});
              break;
            }
          }
          ++BB_ID;
        }
        functionJson["BasicBlocks"] = basicBlocksJson;
        resultJson.push_back(functionJson);
      } 
    }
    // Write the JSON to a file.
    std::ofstream outFile("proc_{id}_bb_info.json");
    outFile << resultJson.dump(4); // Pretty-print with 4 spaces of indentation.
    outFile.close();

    return true;
  }

  // Compute size in bytes for a value's type using DataLayout.
  // If type is void or function or token or metadata, returns 0.
  static uint64_t typeSizeBytes(const DataLayout &DL, const Type *T) {
    if (!T) return 0;
    if (T->isVoidTy() || T->isFunctionTy() || T->isLabelTy() || T->isMetadataTy())
      return 0;

    // DataLayout::getTypeAllocSize expects non-const Type*
    TypeSize TS = DL.getTypeAllocSize(const_cast<Type*>(T));
    // For scalable types we return the known minimum; for fixed types return fixed size.
    if (TS.isScalable())
      return TS.getKnownMinSize();
    else
      return TS.getFixedSize();
  }

};
}

char BBInfoPass::ID = 0;
static RegisterPass<BBInfoPass> X("bb-info", "Basic Block Info Pass", false, false);
