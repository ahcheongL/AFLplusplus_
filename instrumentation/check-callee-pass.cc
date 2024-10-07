#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <list>
#include <string>
#include <fstream>
#include <sys/time.h>

#include "config.h"

#include "llvm/Config/llvm-config.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/IRBuilder.h"
#if LLVM_VERSION_MAJOR >= 11 /* use new pass manager */
  #include "llvm/Passes/PassPlugin.h"
  #include "llvm/Passes/PassBuilder.h"
  #include "llvm/IR/PassManager.h"
#else
  #include "llvm/IR/LegacyPassManager.h"
  #include "llvm/Transforms/IPO/PassManagerBuilder.h"
#endif
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Pass.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#if LLVM_VERSION_MAJOR >= 14 /* how about stable interfaces? */
  #include "llvm/Passes/OptimizationLevel.h"
#endif
#include "llvm/Support/CommandLine.h"

#include "llvm/IR/IRBuilder.h"
#if LLVM_VERSION_MAJOR >= 4 || \
    (LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR > 4)
  #include "llvm/IR/Verifier.h"
  #include "llvm/IR/DebugInfo.h"
#else
  #include "llvm/Analysis/Verifier.h"
  #include "llvm/DebugInfo.h"
  #define nullptr 0
#endif

#include <set>
#include "afl-llvm-common.h"

using namespace llvm;

namespace {

static cl::opt<std::string> callgraph_fn("callgraph",
                                         cl::desc("Callgraph file name"),
                                         cl::init(""));

static cl::opt<std::string> caller_name("caller",
                                        cl::desc("Target caller name"),
                                        cl::init(""));

static llvm::cl::opt<bool> dump_ir(
    "dump-ir", llvm::cl::desc("Dump the final IR to stdout"));

class CheckCalleePass : public PassInfoMixin<CheckCalleePass> {
 public:
  CheckCalleePass() {
  }

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);

 private:
  bool init_pass_context(Module &);

  llvm::Function *get_target_func();

  // Read callgraph.txt and get direct callees of target function
  bool read_call_graph(llvm::Function *);

  void instrument_callees();

  void verify_dump_module();

  Module           *Mod;
  LLVMContext      *Context;
  const DataLayout *DL;
  IRBuilder<>      *IRB;

  llvm::Function *main_func = NULL;
  bool            is_target_main = false;

  std::set<llvm::Function *> target_callees;

  Type *VoidTy;
  Type *Int32Ty;
  Type *Int8Ty;
  Type *Int8PtrTy;
  Type *Int8PtrPtrTy;
  Type *Int32PtrTy;
  Type *FilePtrTy;
};

}  // namespace

extern "C" ::llvm::PassPluginLibraryInfo LLVM_ATTRIBUTE_WEAK
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "CheckCalleePass", "v0.1",
          /* lambda to insert our pass into the pass pipeline. */
          [](PassBuilder &PB) {

#if LLVM_VERSION_MAJOR <= 13
            using OptimizationLevel = typename PassBuilder::OptimizationLevel;
#endif
            PB.registerOptimizerLastEPCallback(
                [](ModulePassManager &MPM, OptimizationLevel OL) {
                  MPM.addPass(CheckCalleePass());
                });
          }};
}

PreservedAnalyses CheckCalleePass::run(Module &M, ModuleAnalysisManager &MAM) {
  auto PA = PreservedAnalyses::all();

  if (!init_pass_context(M)) { return PA; }

  for (auto &F : M) {
    if (F.getName() == "main") {
      main_func = &F;
      break;
    }
  }

  if (main_func == NULL) {
    errs() << "No main function found, skipping CheckCalleePass\n";
    return PA;
  }

  Function *target_func = get_target_func();
  if (target_func == NULL) {
    errs() << "No target function found, skipping CheckCalleePass\n";
    return PA;
  }

  if (target_func == main_func) {
    outs() << "Target function is main, skipping CheckCalleePass\n";
    return PA;
  }

  if (!read_call_graph(target_func)) {
    errs() << "Failed to read callgraph, skipping CheckCalleePass\n";
    return PA;
  }

  instrument_callees();

  verify_dump_module();
  delete IRB;

  return PA;
}

bool CheckCalleePass::init_pass_context(llvm::Module &M) {
  Mod = &M;
  Context = &M.getContext();
  DL = &M.getDataLayout();
  IRB = new IRBuilder<>(*Context);

  VoidTy = Type::getVoidTy(*Context);
  Int32Ty = Type::getInt32Ty(*Context);
  Int8Ty = Type::getInt8Ty(*Context);
  Int8PtrTy = PointerType::get(Int8Ty, 0);
  Int8PtrPtrTy = PointerType::get(Int8PtrTy, 0);
  Int32PtrTy = PointerType::get(Int32Ty, 0);

  FilePtrTy = NULL;
  for (auto &type : M.getIdentifiedStructTypes()) {
    if (type->getName().startswith("struct._IO_FILE")) {
      FilePtrTy = PointerType::get(type, 0);
      break;
    }
  }

  if (FilePtrTy == nullptr) {
    errs() << "Can't find IO_FILE type! Abort.\n";
    return false;
  }

  return true;
}

Function *CheckCalleePass::get_target_func() {
  for (auto &BB : *main_func) {
    bool            has_ret = false;
    llvm::Function *callee = NULL;

    for (auto &I : BB) {
      if (llvm::isa<llvm::ReturnInst>(&I)) {
        has_ret = true;
        break;
      }

      llvm::CallInst *call_inst = llvm::dyn_cast<llvm::CallInst>(&I);
      if (call_inst == NULL) { continue; }

      llvm::Function *cur_callee = call_inst->getCalledFunction();
      if (cur_callee == NULL) { continue; }

      const std::string callee_name = cur_callee->getName().str();
      if (callee_name.find("__unit_driver") != std::string::npos) { continue; }

      if (callee_name.find("__extract") != std::string::npos) { continue; }

      callee = cur_callee;
    }

    if (has_ret && callee != NULL) {
      const std::string callee_name = callee->getName().str();
      is_target_main = callee_name == "__old_main";
      return callee;
    }
  }

  return NULL;
}

bool CheckCalleePass::read_call_graph(llvm::Function *target_func) {
  const std::string module_bc_name = Mod->getName().str();
  std::string       module_parent_dir = "./";

  if (module_bc_name.find("/") != std::string::npos) {
    module_parent_dir =
        module_bc_name.substr(0, module_bc_name.rfind("/")) + "/";
  }

  const std::string callgraph_fn = module_parent_dir + "/callgraph.txt";
  std::ifstream     callgraph_f(callgraph_fn);

  if (!callgraph_f.is_open()) {
    errs() << "Failed to open callgraph file: " << callgraph_fn << "\n";
    return false;
  }

  std::string line;

  std::string target_func_name = target_func->getName().str();

  while (std::getline(callgraph_f, line)) {
    if (line.empty()) { continue; }
    if (line[0] != '[') { continue; }

    size_t bracket_pos = line.find(']');
    if (bracket_pos == std::string::npos) { continue; }

    line = line.substr(bracket_pos + 1);

    if (line.compare(0, target_func_name.size(), target_func_name) != 0) {
      continue;
    }

    break;
  }

  std::vector<std::string> callee_names;
  while (std::getline(callgraph_f, line)) {
    if (line.empty()) { break; }
    if (line.size() < 7) { break; }

    size_t bracket_pos = line.find(']');
    if (bracket_pos == std::string::npos) { break; }
    line = line.substr(bracket_pos + 1);

    callee_names.push_back(line);
  }

  callgraph_f.close();

  auto       callee_name_iter = callee_names.begin();
  const auto callee_name_end = callee_names.end();

  for (; callee_name_iter != callee_name_end; ++callee_name_iter) {
    const std::string &callee_name = *callee_name_iter;
    llvm::Function    *callee = Mod->getFunction(callee_name);

    if (callee == NULL) {
      errs() << "Can't find callee: " << callee_name << "\n";
      continue;
    }

    target_callees.insert(callee);
  }

  return true;
}

void CheckCalleePass::instrument_callees() {
  unsigned int bb_index = 2;

  llvm::Constant *callee_map_ptr_var =
      Mod->getOrInsertGlobal("__afl_callee_map_ptr", Int8PtrTy);

  llvm::Constant *const_one = llvm::ConstantInt::get(Int8Ty, 1);

  for (llvm::Function *callee : target_callees) {
    if (callee->isDeclaration()) { continue; }

    llvm::BasicBlock  &entry_bb = callee->getEntryBlock();
    llvm::Instruction *first_inst = entry_bb.getFirstNonPHIOrDbgOrLifetime();
    IRB->SetInsertPoint(first_inst);

    for (auto &BB : *callee) {
      IRB->SetInsertPoint(BB.getFirstNonPHIOrDbgOrLifetime());
      llvm::Constant *bb_index_val = llvm::ConstantInt::get(Int32Ty, bb_index);

      llvm::Value *callee_map_ptr =
          IRB->CreateLoad(Int8PtrTy, callee_map_ptr_var);

      llvm::Value *bb_index_ptr =
          IRB->CreateGEP(Int8Ty, callee_map_ptr, bb_index_val);

      llvm::LoadInst *bb_index_load = IRB->CreateLoad(Int8Ty, bb_index_ptr);
      llvm::Value *bb_index_load_inc = IRB->CreateAdd(bb_index_load, const_one);

      IRB->CreateStore(bb_index_load_inc, bb_index_ptr);

      bb_index++;
    }
  }

  if (bb_index >= CALLEE_MAP_SIZE) {
    errs() << "Too many basic blocks (" << bb_index << ", > " << CALLEE_MAP_SIZE
           << ") in target function.\n";
    errs() << "Consider increasing CALLEE_MAP_SIZE.\n";
    return;
  }

  return;
}

void CheckCalleePass::verify_dump_module() {
  std::string              out;
  llvm::raw_string_ostream output(out);
  bool                     has_error = verifyModule(*Mod, &output);

  if (has_error > 0) {
    llvm::errs() << "The final IR has the following errors : \n";
    llvm::errs() << output.str();
  } else {
    llvm::errs() << "The final IR is valid\n";
  }

  if (!dump_ir) { return; }

  llvm::outs() << "Dumping IR...\n";

  Mod->print(llvm::outs(), nullptr);
  llvm::outs() << "\n";
}
