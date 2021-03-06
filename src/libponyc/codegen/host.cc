#include "llvm_config_begin.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Target/TargetOptions.h>

#include "llvm_config_end.h"

#include <stdio.h>
#include "codegen.h"
#include "ponyassert.h"

using namespace llvm;

LLVMTargetMachineRef codegen_machine(LLVMTargetRef target, pass_opt_t* opt,
  bool jit)
{
  Optional<Reloc::Model> reloc;

  if(opt->pic || opt->library)
    reloc = Reloc::PIC_;

  CodeGenOpt::Level opt_level =
    opt->release ? CodeGenOpt::Aggressive : CodeGenOpt::None;

  TargetOptions options;
  options.TrapUnreachable = true;

  Target* t = reinterpret_cast<Target*>(target);

#if PONY_LLVM < 600
  CodeModel::Model model = jit ? CodeModel::JITDefault : CodeModel::Default;
  TargetMachine* m = t->createTargetMachine(opt->triple, opt->cpu,
    opt->features, options, reloc, model, opt_level);
#else
TargetMachine* m = t->createTargetMachine(opt->triple, opt->cpu,
  opt->features, options, reloc, llvm::None, opt_level, jit);
#endif

  return reinterpret_cast<LLVMTargetMachineRef>(m);
}

#if PONY_LLVM < 700
char* LLVMGetHostCPUName()
{
  return strdup(sys::getHostCPUName().str().c_str());
}

char* LLVMGetHostCPUFeatures()
{
  StringMap<bool> features;
  bool got_features = sys::getHostCPUFeatures(features);
#ifdef PLATFORM_IS_ARM
  // LLVM might not have CPU features support on e.g. FreeBSD/aarch64
  if (!got_features) {
    features["neon"] = true;
  }
#else
  pony_assert(got_features);
#endif
  (void)got_features;

  // Calculate the size of buffer that will be needed to return all features.
  size_t buf_size = 0;
  for(auto it = features.begin(); it != features.end(); it++)
    buf_size += (*it).getKey().str().length() + 2; // plus +/- char and ,/null

#if PONY_LLVM < 500 and defined(PLATFORM_IS_X86)
  // Add extra buffer space for llvm bug workaround
  buf_size += 9;
#endif

#ifdef PLATFORM_IS_ARMHF_WITHOUT_NEON
  buf_size += 6;
#endif

  char* buf = (char*)malloc(buf_size);
  pony_assert(buf != NULL);
  buf[0] = 0;

  for(auto it = features.begin(); it != features.end();)
  {
    if((*it).getValue())
      strcat(buf, "+");
    else
      strcat(buf, "-");

    strcat(buf, (*it).getKey().str().c_str());

    it++;
    if(it != features.end())
      strcat(buf, ",");
  }

#if PONY_LLVM < 500 and defined(PLATFORM_IS_X86)
  // Disable -avx512f on LLVM < 5.0.0 to avoid bug https://bugs.llvm.org/show_bug.cgi?id=30542
  strcat(buf, ",-avx512f");
#endif

#ifdef PLATFORM_IS_ARMHF_WITHOUT_NEON
  // Workaround for https://bugs.llvm.org/show_bug.cgi?id=30842
  strcat(buf, ",-neon");
#endif

  return buf;
}
#endif

void LLVMSetUnsafeAlgebra(LLVMValueRef inst)
{
#if PONY_LLVM < 600
  unwrap<Instruction>(inst)->setHasUnsafeAlgebra(true);
#else // See https://reviews.llvm.org/D39304 for this change
  unwrap<Instruction>(inst)->setHasAllowReassoc(true);
#endif
}

void LLVMSetNoUnsignedWrap(LLVMValueRef inst)
{
  unwrap<BinaryOperator>(inst)->setHasNoUnsignedWrap(true);
}

void LLVMSetNoSignedWrap(LLVMValueRef inst)
{
  unwrap<BinaryOperator>(inst)->setHasNoSignedWrap(true);
}

void LLVMSetIsExact(LLVMValueRef inst)
{
  unwrap<BinaryOperator>(inst)->setIsExact(true);
}

LLVMValueRef LLVMConstNaN(LLVMTypeRef type)
{
  Type* t = unwrap<Type>(type);

  Value* nan = ConstantFP::getNaN(t);
  return wrap(nan);
}

LLVMValueRef LLVMConstInf(LLVMTypeRef type, bool negative)
{
  Type* t = unwrap<Type>(type);

  Value* inf = ConstantFP::getInfinity(t, negative);
  return wrap(inf);
}

LLVMModuleRef LLVMParseIRFileInContext(LLVMContextRef ctx, const char* file)
{
  SMDiagnostic diag;

  return wrap(parseIRFile(file, diag, *unwrap(ctx)).release());
}

// From LLVM internals.
static MDNode* extractMDNode(MetadataAsValue* mdv)
{
  Metadata* md = mdv->getMetadata();
  pony_assert(isa<MDNode>(md) || isa<ConstantAsMetadata>(md));

  MDNode* n = dyn_cast<MDNode>(md);
  if(n != NULL)
    return n;

  return MDNode::get(mdv->getContext(), md);
}

bool LLVMHasMetadataStr(LLVMValueRef val, const char* str)
{
  Value* v = unwrap<Value>(val);
  Instruction* i = dyn_cast<Instruction>(v);

  if(i != NULL)
    return i->getMetadata(str) != NULL;

  return cast<Function>(v)->getMetadata(str) != NULL;
}

void LLVMSetMetadataStr(LLVMValueRef val, const char* str, LLVMValueRef node)
{
  pony_assert(node != NULL);

  MDNode* n = extractMDNode(unwrap<MetadataAsValue>(node));
  Value* v = unwrap<Value>(val);
  Instruction* i = dyn_cast<Instruction>(v);

  if(i != NULL)
    i->setMetadata(str, n);
  else
    cast<Function>(v)->setMetadata(str, n);
}

void LLVMMDNodeReplaceOperand(LLVMValueRef parent, unsigned i,
  LLVMValueRef node)
{
  pony_assert(parent != NULL);
  pony_assert(node != NULL);

  MDNode *pn = extractMDNode(unwrap<MetadataAsValue>(parent));
  MDNode *cn = extractMDNode(unwrap<MetadataAsValue>(node));
  pn->replaceOperandWith(i, cn);
}

LLVMValueRef LLVMMemcpy(LLVMModuleRef module, bool ilp32)
{
  Module* m = unwrap(module);

  Type* params[3];
  params[0] = Type::getInt8PtrTy(m->getContext());
  params[1] = params[0];
  params[2] = Type::getIntNTy(m->getContext(), ilp32 ? 32 : 64);

  return wrap(Intrinsic::getDeclaration(m, Intrinsic::memcpy, {params, 3}));
}

LLVMValueRef LLVMMemmove(LLVMModuleRef module, bool ilp32)
{
  Module* m = unwrap(module);

  Type* params[3];
  params[0] = Type::getInt8PtrTy(m->getContext());
  params[1] = params[0];
  params[2] = Type::getIntNTy(m->getContext(), ilp32 ? 32 : 64);

  return wrap(Intrinsic::getDeclaration(m, Intrinsic::memmove, {params, 3}));
}

LLVMValueRef LLVMLifetimeStart(LLVMModuleRef module, LLVMTypeRef type)
{
  Module* m = unwrap(module);

#if PONY_LLVM < 500
  (void)type;
  return wrap(Intrinsic::getDeclaration(m, Intrinsic::lifetime_start, {}));
#else
  Type* t[1] = { unwrap(type) };
  return wrap(Intrinsic::getDeclaration(m, Intrinsic::lifetime_start, t));
#endif
}

LLVMValueRef LLVMLifetimeEnd(LLVMModuleRef module, LLVMTypeRef type)
{
  Module* m = unwrap(module);

#if PONY_LLVM < 500
  (void)type;
  return wrap(Intrinsic::getDeclaration(m, Intrinsic::lifetime_end, {}));
#else
  Type* t[1] = { unwrap(type) };
  return wrap(Intrinsic::getDeclaration(m, Intrinsic::lifetime_end, t));
#endif
}
