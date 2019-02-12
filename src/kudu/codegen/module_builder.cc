// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/codegen/module_builder.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>


// NOTE: among the headers below, the MCJIT.h header file is needed
//       for successful run-time operation of the code generator.
#include <glog/logging.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/MCJIT.h> // IWYU pragma: keep
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Constant.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Pass.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/IPO/AlwaysInliner.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include "kudu/codegen/precompiled.ll.h"
#include "kudu/gutil/basictypes.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/util/status.h"

#ifndef CODEGEN_MODULE_BUILDER_DO_OPTIMIZATIONS
#if NDEBUG
#define CODEGEN_MODULE_BUILDER_DO_OPTIMIZATIONS 1
#else
#define CODEGEN_MODULE_BUILDER_DO_OPTIMIZATIONS 0
#endif
#endif

using llvm::AttrBuilder;
using llvm::AttributeList;
using llvm::CodeGenOpt::Level;
using llvm::ConstantExpr;
using llvm::ConstantInt;
using llvm::EngineBuilder;
using llvm::ExecutionEngine;
using llvm::Function;
using llvm::FunctionType;
using llvm::GlobalValue;
using llvm::IntegerType;
using llvm::legacy::FunctionPassManager;
using llvm::legacy::PassManager;
using llvm::LLVMContext;
using llvm::Module;
using llvm::PassManagerBuilder;
using llvm::PointerType;
using llvm::raw_os_ostream;
using llvm::SMDiagnostic;
using llvm::TargetMachine;
using llvm::Type;
using llvm::Value;
using std::move;
using std::ostream;
using std::ostringstream;
using std::string;
using std::unique_ptr;
using std::unordered_set;
using std::vector;
using strings::Substitute;

namespace kudu {
namespace codegen {

namespace {

string ToString(const SMDiagnostic& err) {
  ostringstream sstr;
  raw_os_ostream os(sstr);
  err.print("precompiled.ll", os);
  os.flush();
  return Substitute("line $0 col $1: $2",
                    err.getLineNo(), err.getColumnNo(),
                    sstr.str());
}

string ToString(const Module& m) {
  ostringstream sstr;
  raw_os_ostream os(sstr);
  os << m;
  return sstr.str();
}

// This method is needed for the implicit conversion from
// llvm::StringRef to std::string
string ToString(const Function* f) {
  return f->getName();
}

bool ModuleContains(const Module& m, const Function* fptr) {
  for (const auto& function : m) {
    if (&function == fptr) return true;
  }
  return false;
}

} // anonymous namespace

ModuleBuilder::ModuleBuilder()
  : state_(kUninitialized),
    context_(new LLVMContext()),
    builder_(*context_) {}

ModuleBuilder::~ModuleBuilder() {}

Status ModuleBuilder::Init() {
  CHECK_EQ(state_, kUninitialized) << "Cannot Init() twice";

  // Even though the LLVM API takes an explicit length for the input IR,
  // it appears to actually depend on NULL termination. We assert for it
  // here because otherwise we end up with very strange LLVM errors which
  // are tough to debug.
  CHECK_EQ('\0', precompiled_ll_data[precompiled_ll_len]) << "IR not properly NULL-terminated";

  // However, despite depending on the buffer being null terminated, it doesn't
  // expect the null terminator to be included in the length of the buffer.
  // Per http://llvm.org/docs/doxygen/html/classllvm_1_1MemoryBuffer.html :
  //   > In addition to basic access to the characters in the file, this interface
  //   > guarantees you can read one character past the end of the file, and that this
  //   > character will read as '\0'.
  llvm::StringRef ir_data(precompiled_ll_data, precompiled_ll_len);
  CHECK_GT(ir_data.size(), 0) << "IR not properly linked";

  // Parse IR.
  SMDiagnostic err;
  unique_ptr<llvm::MemoryBuffer> ir_buf(llvm::MemoryBuffer::getMemBuffer(ir_data));
  module_ = llvm::parseIR(ir_buf->getMemBufferRef(), err, *context_);
  if (!module_) {
    return Status::ConfigurationError("Could not parse IR", ToString(err));
  }
  VLOG(3) << "Successfully parsed IR:\n" << ToString(*module_);

  // TODO: consider parsing this module once instead of on each invocation.
  state_ = kBuilding;
  return Status::OK();
}

Function* ModuleBuilder::Create(FunctionType* fty, const string& name) {
  CHECK_EQ(state_, kBuilding);
  return Function::Create(fty, Function::ExternalLinkage, name, module_.get());
}

Function* ModuleBuilder::GetFunction(const string& name) {
  CHECK_EQ(state_, kBuilding);
  // All extern "C" functions are guaranteed to have the same
  // exact name as declared in the source file.
  return CHECK_NOTNULL(module_->getFunction(name));
}

Type* ModuleBuilder::GetType(const string& name) {
  CHECK_EQ(state_, kBuilding);
  // Technically clang is not obligated to name every
  // class as "class.kudu::ClassName" but so long as there
  // are no naming conflicts in the LLVM context it appears
  // to do so (naming conflicts are avoided by having 1 context
  // per module)
  return CHECK_NOTNULL(module_->getTypeByName(name));
}

Value* ModuleBuilder::GetPointerValue(void* ptr) const {
  CHECK_EQ(state_, kBuilding);
  // No direct way of creating constant pointer values in LLVM, so
  // first a constant int has to be created and then casted to a pointer
  IntegerType* llvm_uintptr_t = Type::getIntNTy(*context_, 8 * sizeof(ptr));
  uintptr_t int_value = reinterpret_cast<uintptr_t>(ptr);
  ConstantInt* llvm_int_value = ConstantInt::get(llvm_uintptr_t,
                                                 int_value, false);
  Type* llvm_ptr_t = Type::getInt8PtrTy(*context_);
  return ConstantExpr::getIntToPtr(llvm_int_value, llvm_ptr_t);
}


void ModuleBuilder::AddJITPromise(llvm::Function* llvm_f,
                                  FunctionAddress* actual_f) {
  CHECK_EQ(state_, kBuilding);
  DCHECK(ModuleContains(*module_, llvm_f))
    << "Function " << ToString(llvm_f) << " does not belong to ModuleBuilder.";
  JITFuture fut;
  fut.llvm_f_ = llvm_f;
  fut.actual_f_ = actual_f;
  futures_.push_back(fut);
}

namespace {

void DoOptimizations(Module* module,
                     const unordered_set<string>& external_functions) {
  PassManagerBuilder pass_builder;
  // Don't optimize for code size (this corresponds to -O2/-O3)
  pass_builder.SizeLevel = 0;
#if CODEGEN_MODULE_BUILDER_DO_OPTIMIZATIONS
  pass_builder.OptLevel = 2;
  pass_builder.Inliner = llvm::createFunctionInliningPass(
      pass_builder.OptLevel,
      pass_builder.SizeLevel,
      false); // don't disable inlining of hot call sites
#else
  // Even if we don't want to do optimizations, we have to run the "AlwaysInliner" pass.
  // This pass ensures that any functions marked 'always_inline' are inlined, but nothing
  // else.
  //
  // If we don't, the following happens:
  // - symbols in libc++ (eg _ZNKSt3__19basic_iosIcNS_11char_traitsIcEEE5rdbufEv) are
  //   marked as __attribute__((always_inline)) in the header.
  // - those symbols end up included with 'local' visibility in libc++.so, since the compiler
  //   knows that all call sites should inline them.
  // - if we don't run any inliner at all, then our generated code generates LLVM
  //   'invoke' instructions to try to call these external functions, despite them
  //   being marked 'always_inline'.
  // - these 'invoke' instructions fail to link at runtime since they can't find the
  //   dynamic symbol (due to its local visibility)
  pass_builder.OptLevel = 0;
  pass_builder.Inliner = llvm::createAlwaysInlinerLegacyPass();
#endif

  FunctionPassManager fpm(module);
  pass_builder.populateFunctionPassManager(fpm);
  fpm.doInitialization();

  // For each function in the module, optimize it
  for (Function& f : *module) {
    // The bool return value here just indicates whether the passes did anything.
    // We can safely expect that many functions are too small to do any optimization.
    ignore_result(fpm.run(f));
  }
  fpm.doFinalization();

  PassManager module_passes;

  // Internalize all functions that aren't explicitly specified with external linkage.
  module_passes.add(llvm::createInternalizePass([&](const GlobalValue& v) {
    return ContainsKey(external_functions, v.getGlobalIdentifier());
  }));

  // Run Global Dead Code Elimination.
  //
  // This is responsible for removing any unreferenced functions. This is
  // important to do even in -O0 to workaround an issue we see when our generated
  // functions are actually empty. In that case, for whatever reason (perhaps a bug in LLVM?)
  // the compiled module would try to include versions of functions with calls to
  // other functions marked "alwaysinline". The latter functions would not get linked
  // in our compiled module, and then the module would fail to load.
  module_passes.add(llvm::createGlobalDCEPass());
  pass_builder.populateModulePassManager(module_passes);

  // Same as above, the result here just indicates whether optimization made any changes.
  // Don't need to check it.
  ignore_result(module_passes.run(*module));
}

// Set LLVM attributes on all functions in 'module'.
// Modeled after 'setFunctionAttributes' in LLVM's 'include/llvm/CodeGen/CommandFlags.def'
void SetFunctionAttributes(Module* module) {
  for (auto& func : *module) {
    AttrBuilder new_attrs;
    new_attrs.addAttribute("no-frame-pointer-elim", "true");
    auto attrs = func.getAttributes();
    attrs = attrs.addAttributes(module->getContext(),
        AttributeList::FunctionIndex, new_attrs);
    func.setAttributes(attrs);
  }
}

vector<string> GetHostCPUAttrs() {
  // LLVM's ExecutionEngine expects features to be enabled or disabled with a list
  // of strings like ["+feature1", "-feature2"].
  vector<string> attrs;
  llvm::StringMap<bool> cpu_features;
  llvm::sys::getHostCPUFeatures(cpu_features);
  for (const auto& entry : cpu_features) {
    attrs.emplace_back(
        Substitute("$0$1", entry.second ? "+" : "-", entry.first().data()));
  }
  return attrs;
}

} // anonymous namespace

Status ModuleBuilder::Compile(unique_ptr<ExecutionEngine>* out) {
  CHECK_EQ(state_, kBuilding);

  // Attempt to generate the engine
  string str;
#ifdef NDEBUG
  Level opt_level = llvm::CodeGenOpt::Aggressive;
#else
  Level opt_level = llvm::CodeGenOpt::None;
#endif
  Module* module = module_.get();
  EngineBuilder ebuilder(move(module_));
  ebuilder.setErrorStr(&str);
  ebuilder.setOptLevel(opt_level);
  ebuilder.setMCPU(llvm::sys::getHostCPUName());
  ebuilder.setMAttrs(GetHostCPUAttrs());
  target_ = ebuilder.selectTarget();
  unique_ptr<ExecutionEngine> local_engine(ebuilder.create(target_));
  if (!local_engine) {
    return Status::ConfigurationError("Code generation for module failed. "
                                      "Could not start ExecutionEngine",
                                      str);
  }
  module->setDataLayout(target_->createDataLayout());

  DoOptimizations(module, GetFunctionNames());
  SetFunctionAttributes(module);

  // Compile the module
  local_engine->finalizeObject();

  // Satisfy the promises
  for (JITFuture& fut : futures_) {
    *fut.actual_f_ = local_engine->getPointerToFunction(fut.llvm_f_);
    if (*fut.actual_f_ == nullptr) {
      return Status::NotFound(
        "Code generation for module failed. Could not find function \""
        + ToString(fut.llvm_f_) + "\".");
    }
  }

  // For LLVM 3.7, generated code lasts exactly as long as the execution engine
  // that created it does. Furthermore, if the module is removed from the
  // engine's ownership, neither the context nor the module have to stick
  // around for the jitted code to run.
  CHECK(local_engine->removeModule(module)); // releases ownership
  module_.reset(module);

  // Upon success write to the output parameter
  out->swap(local_engine);
  state_ = kCompiled;
  return Status::OK();
}

TargetMachine* ModuleBuilder::GetTargetMachine() const {
  CHECK_EQ(state_, kCompiled);
  return CHECK_NOTNULL(target_);
}

unordered_set<string> ModuleBuilder::GetFunctionNames() const {
  unordered_set<string> ret;
  for (const JITFuture& fut : futures_) {
    ret.insert(CHECK_NOTNULL(fut.llvm_f_)->getName());
  }
  return ret;
}

} // namespace codegen
} // namespace kudu
