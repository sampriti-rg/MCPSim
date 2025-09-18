#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Support/raw_ostream.h"
#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <cstddef>
#include <fstream>

using namespace llvm;
using json = nlohmann::json;

namespace {
  struct BBAnnotationPass : public ModulePass {
    static char ID;
    BBAnnotationPass() : ModulePass(ID) {}

    bool runOnModule(Module &M) override {
      const DataLayout &DL = M.getDataLayout(); // Get the data layout of the module.
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
            size_t memoryInstCount = 0;
            size_t nonMemoryInstCount = 0;
            size_t totalMemoryConsumption = 0;
            size_t arithmeticInstCount = 0;
            bool shouldAnnotate = true;
            bool splitInstrumentation = false;

            for (auto &I : BB) {
              totalInstCount++;
              if ((isa<ReturnInst>(&I) || isa<BranchInst>(&I) || isa<SwitchInst>(&I)) && totalInstCount == 1) {
                shouldAnnotate = false;
                continue;
              }

              if (auto *LI = dyn_cast<LoadInst>(&I)) {
                ++memoryInstCount;
                Type *LoadType = LI->getType();
                totalMemoryConsumption += DL.getTypeStoreSize(LoadType);
              }
              else if (auto *SI = dyn_cast<StoreInst>(&I)) {
                ++memoryInstCount;
                Type *StoredType = SI->getValueOperand()->getType();
                totalMemoryConsumption += DL.getTypeStoreSize(StoredType);
              }
              else if (isa<BinaryOperator>(&I) || isa<ICmpInst>(&I) || isa<FCmpInst>(&I)) {
                ++arithmeticInstCount;
              }
              else {
                ++nonMemoryInstCount;
              }
            }

            json bbJson;
            bbJson["BasicBlockID"] = BB_ID;
            bbJson["BasicBlockName"] = BB.getName().str();
            bbJson["MemoryInstructions"] = memoryInstCount;
            bbJson["TotalMemoryConsumption"] = totalMemoryConsumption;
            bbJson["ArithmeticInstructions"] = arithmeticInstCount;
            bbJson["NonMemoryInstructions"] = nonMemoryInstCount;
            bbJson["TotalInstructions"] = totalInstCount;
            basicBlocksJson.push_back(bbJson);

            if (shouldAnnotate) {
              IRBuilder<> Builder(&*BB.getFirstInsertionPt());
              Builder.CreateCall(BeginHook, {Builder.getInt32(BB_ID)});
              for (auto &I : BB) {
                if (isa<ReturnInst>(&I) || isa<BranchInst>(&I) || isa<SwitchInst>(&I)) {
                  IRBuilder<> AfterBuilder(&I);
                  AfterBuilder.CreateCall(EndHook, {AfterBuilder.getInt32(BB_ID)});
                  break;
                }
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
  };
}

char BBAnnotationPass::ID = 0;
static RegisterPass<BBAnnotationPass> X("bb-annotation",
                                          "Instrument BBs with MCP Hook Functions After Extarcting Info into a JSON",
                                          false, // Does not only look at CFG
                                          false  // Is not an analysis pass
);
