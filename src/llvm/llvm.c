// SPDX-License-Identifier: Apache-2.0
#include "llvmimpl.h"


#define HANDLE_LLVM_ERRMSG(err, errmsg) ({ \
  dlog("llvm error: %s", (errmsg)); \
  LLVMDisposeMessage((errmsg)); \
  err; \
})


static err_t select_target(const char* triple, LLVMTargetRef* targetp) {
  char* errmsg;
  if (LLVMGetTargetFromTriple(triple, targetp, &errmsg) != 0)
    return HANDLE_LLVM_ERRMSG(ErrInvalid, errmsg);

  #if DEBUG
    LLVMTargetRef target = *targetp;
    const char* name = LLVMGetTargetName(target);
    const char* description = LLVMGetTargetDescription(target);
    const char* jit = LLVMTargetHasJIT(target) ? " jit" : "";
    const char* mc = LLVMTargetHasTargetMachine(target) ? " mc" : "";
    const char* asmx = LLVMTargetHasAsmBackend(target) ? " asm" : "";
    dlog("selected target: %s (%s) [abilities:%s%s%s]", name, description, jit, mc, asmx);
  #endif

  return 0;
}


// select_target_machine is like -mtune
static err_t select_target_machine(
  LLVMTargetRef         target,
  const char*           triple,
  LLVMCodeGenOptLevel   optLevel,
  LLVMCodeModel         codeModel,
  LLVMTargetMachineRef* resultp)
{
  // select host CPU and features (NOT PORTABLE!) when optimizing
  const char* CPU = "";      // "" for generic
  const char* features = ""; // "" for none
  char* hostCPUName = NULL; // needs LLVMDisposeMessage
  char* hostFeatures = NULL; // needs LLVMDisposeMessage
  if (optLevel != LLVMCodeGenLevelNone && strcmp(triple, llvm_host_triple())) {
    hostCPUName = LLVMGetHostCPUName();
    hostFeatures = LLVMGetHostCPUFeatures();
    CPU = hostCPUName;
    features = hostFeatures;
  }

  LLVMTargetMachineRef tm = LLVMCreateTargetMachine(
    target, triple, CPU, features, optLevel, LLVMRelocStatic, codeModel);
  if (!tm) {
    dlog("LLVMCreateTargetMachine failed");
    return ErrNotSupported;
  }

  if (hostCPUName) {
    LLVMDisposeMessage(hostCPUName);
    LLVMDisposeMessage(hostFeatures);
  }

  *resultp = tm;
  return 0;
}


CoLLVMArchiveKind llvm_sys_archive_kind(sys_t sys) {
  switch ((enum target_sys)sys) {
    // note: llvm/lib/Object/ArchiveWriter.cpp switches to DARWIN64/GNU64 if needed
    case SYS_macos: return CoLLVMArchive_DARWIN;
    case SYS_linux: return CoLLVMArchive_GNU;
    // case SYS_openbsd: case SYS_freebsd: return CoLLVMArchive_BSD;
    case SYS_wasi: return CoLLVMArchive_GNU;
    case SYS_none:
      break;
  }
  safefail("invalid sys_t %u", sys);
  return CoLLVMArchive_GNU;
}


void llvm_module_init(CoLLVMModule* m, BuildCtx* build, const char* name) {
  m->build = build;
  m->M = LLVMModuleCreateWithNameInContext(name, CoLLVMContextCreate());
  m->TM = NULL;
}


void llvm_module_dispose(CoLLVMModule* m) {
  if (m->M == NULL)
    return;
  LLVMModuleRef mod = m->M;
  LLVMContextRef ctx = LLVMGetModuleContext(mod);
  LLVMDisposeModule(mod);
  LLVMContextDispose(ctx);
  memset(m, 0, sizeof(*m));
}


err_t llvm_module_set_target(CoLLVMModule* m, const char* triple) {
  m->TM = NULL;
  LLVMTargetRef target;
  err_t err = select_target(triple, &target);
  if (err)
    return err;

  LLVMCodeGenOptLevel optLevel;
  LLVMCodeModel codeModel = LLVMCodeModelDefault;
  switch (m->build->opt) {
    case '0':
      optLevel = LLVMCodeGenLevelNone;
      break;
    case '1':
      optLevel = LLVMCodeGenLevelLess;
      break;
    case '2':
      optLevel = LLVMCodeGenLevelDefault;
      break;
    case '3':
      optLevel = LLVMCodeGenLevelAggressive;
      break;
    case 's':
      optLevel = LLVMCodeGenLevelDefault;
      codeModel = LLVMCodeModelSmall;
      break;
  }
  LLVMTargetMachineRef targetm;
  err = select_target_machine(target, triple, optLevel, codeModel, &targetm);
  if (err)
    return err;

  LLVMModuleRef mod = m->M;
  LLVMSetTarget(mod, triple);
  LLVMTargetDataRef dataLayout = LLVMCreateTargetDataLayout(targetm);
  LLVMSetModuleDataLayout(mod, assertnotnull(dataLayout));

  m->TM = targetm;
  return 0;
}


err_t llvm_module_optimize(CoLLVMModule* m, const CoLLVMBuild* opt) {
  // optimize and target-fit module (also verifies the IR)
  return llvm_module_optimize1(m, opt, (char)m->build->opt);
}


void llvm_module_dump(CoLLVMModule* m) {
  LLVMDumpModule(m->M);
}
