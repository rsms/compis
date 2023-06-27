// LLVM routines that must be implemented in C++
// SPDX-License-Identifier: Apache-2.0
#include "llvm-includes.hh"
#include "llvmimpl.h"

using namespace llvm;


static const char* g_default_target_triple = "";


// err_llvm converts an llvm error to a co err_t
err_t err_llvm(Error e, char** nullable errmsg) {
  if (!e)
    return ErrOk;
  std::error_code c = errorToErrorCode(std::move(e));
  err_t err = err_errnox(c.value());
  if (errmsg)
    *errmsg = LLVMCreateMessage(c.message().c_str());
  return err ? err : ErrInvalid;
}


err_t llvm_init() {
  static std::once_flag once;
  std::call_once(once, [](){
    #if 1
      // Initialize ALL targets
      // (this causes a lot of llvm code to be included in this program)
      // Note: lld (liblldCOFF.a) requires all targets
      InitializeAllTargetInfos();
      InitializeAllTargets();
      InitializeAllTargetMCs();
      InitializeAllAsmPrinters();
      InitializeAllAsmParsers();
    #else
      // For JIT only, call InitializeNativeTarget()
      InitializeNativeTarget();

      // Initialize some targets (see llvm/Config/Targets.def)
      #define TARGETS(_) _(AArch64) _(WebAssembly) _(X86)
      #define _(TargetName) \
        LLVMInitialize##TargetName##TargetInfo(); \
        LLVMInitialize##TargetName##Target(); \
        LLVMInitialize##TargetName##TargetMC(); \
        LLVMInitialize##TargetName##AsmPrinter(); \
        /*LLVMInitialize##TargetName##AsmParser();*/
      TARGETS(_)
      #undef _
    #endif

    g_default_target_triple = LLVMGetDefaultTargetTriple();
    // Note: if we ever make this non-static, LLVMDisposeMessage(str) when done.
  });
  return 0;
}


const char* llvm_host_triple() {
  return g_default_target_triple;
}


// static bool llvm_error_to_errmsg(Error err, char** errmsg) {
//   assert(err);
//   std::string errstr = toString(std::move(err));
//   *errmsg = LLVMCreateMessage(errstr.c_str());
//   return false;
// }


void llvm_triple_info(const char* triplestr, CoLLVMTargetInfo* info) {
  Triple triple(Triple::normalize(triplestr));
  info->arch_type   = (CoLLVMArch)triple.getArch();
  info->vendor_type = (CoLLVMVendor)triple.getVendor();
  info->os_type     = (CoLLVMOS)triple.getOS();
  info->env_type    = (CoLLVMEnvironment)triple.getEnvironment();
  info->obj_format  = (CoLLVMObjectFormat)triple.getObjectFormat();
  info->is_little_endian = triple.isLittleEndian();
  info->ptr_size = 4;
  if (triple.isArch64Bit()) {
    info->ptr_size = 8;
  } else if (triple.isArch16Bit()) {
    info->ptr_size = 2;
  }
}


void llvm_triple_min_version(const char* triple, CoLLVMVersionTuple* r) {
  Triple t(Triple::normalize(triple));
  VersionTuple v = t.getMinimumSupportedOSVersion();
  if (v.empty()) {
    r->major = -1;
    r->minor = -1;
    r->subminor = -1;
    r->build = -1;
  } else {
    r->major    = (int)v.getMajor();
    r->minor    = v.getMinor() == None ? -1 : (int)v.getMinor().getValue();
    r->subminor = v.getSubminor() == None ? -1 : (int)v.getSubminor().getValue();
    r->build    = v.getBuild() == None ? -1 : (int)v.getBuild().getValue();
  }
}


// CoLLVMOS_name returns the canonical name for the OS
const char* CoLLVMOS_name(CoLLVMOS os) {
  return (const char*)Triple::getOSTypeName((Triple::OSType)os).bytes_begin();
}

// CoLLVMArch_name returns the canonical name for the arch
const char* CoLLVMArch_name(CoLLVMArch v) {
  return (const char*)Triple::getArchTypeName((Triple::ArchType)v).bytes_begin();
}

// CoLLVMVendor_name returns the canonical name for the vendor
const char* CoLLVMVendor_name(CoLLVMVendor v) {
  return (const char*)Triple::getVendorTypeName((Triple::VendorType)v).bytes_begin();
}

// CoLLVMEnvironment_name returns the canonical name for the environment
const char* CoLLVMEnvironment_name(CoLLVMEnvironment v) {
  return (const char*)Triple::getEnvironmentTypeName((Triple::EnvironmentType)v).bytes_begin();
}

err_t llvm_module_optimize1(CoLLVMModule* m, const CoLLVMBuild* opt, char O) {
  Module& module = *unwrap((LLVMModuleRef)m->M);

  TargetMachine& targetMachine = *reinterpret_cast<TargetMachine*>(assertnotnull(m->TM));
  module.setTargetTriple(targetMachine.getTargetTriple().str());
  module.setDataLayout(targetMachine.createDataLayout());

  // pass performance debugging
  const bool enable_time_report = false;
  TimePassesIsEnabled = enable_time_report; // global llvm variable

  // Pipeline configurations
  // See {llvm}/lib/Passes/PassBuilderPipelines.cpp
  // and {llvm}/include/llvm/Passes/PassBuilder.h
  //
  // Note: CallGraphProfile enables the CGProfile pass which produces call graph
  // profile data used by Profile Guided Optimizations (PGO)
  // See https://llvm.org/devmtg/2018-10/slides/
  //     Spencer-Profile%20Guided%20Function%20Layout%20in%20LLVM%20and%20LLD.pdf
  //
  // MergeFunctions:   see https://llvm.org/docs/MergeFunctions.html
  // SLPVectorization: see https://llvm.org/docs/Vectorizers.html#slp-vectorizer
  //
  PipelineTuningOptions pipelineOpt; // start with defaults
  pipelineOpt.LoopInterleaving = O > '0';
  pipelineOpt.LoopVectorization = O > '0';
  pipelineOpt.SLPVectorization = O > '0';
  pipelineOpt.LoopUnrolling = O > '0';
  pipelineOpt.CallGraphProfile = O > '0';
  pipelineOpt.MergeFunctions = O > '2';
  pipelineOpt.EagerlyInvalidateAnalyses = O > '1';

  // Instrumentations
  PassInstrumentationCallbacks instrCallbacks;
  // StandardInstrumentations std_instrumentations(false);
  // std_instrumentations.registerCallbacks(instrCallbacks);

  // create analysis managers
  LoopAnalysisManager     LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager    CGAM;
  ModuleAnalysisManager   MAM;

  // create pass manager builder
  Optional<PGOOptions> pgo = None; // Profile Guided Optimizations (PGO) options
  PassBuilder PB(&targetMachine, pipelineOpt, pgo, &instrCallbacks);

  // Register the AA (Alias Analysis) manager first so that our version is the one used
  FAM.registerPass([&] { return PB.buildDefaultAAPipeline(); });

  // Register TargetLibraryAnalysis
  Triple targetTriple(module.getTargetTriple());
  auto tlii = std::make_unique<TargetLibraryInfoImpl>(targetTriple);
  FAM.registerPass([&] { return TargetLibraryAnalysis(*tlii); });

  // Register all the basic analyses with the managers
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  // IR verification
  #ifdef DEBUG
  // Verify the input
  PB.registerPipelineStartEPCallback(
    [](ModulePassManager& mpm, OptimizationLevel OL) {
      mpm.addPass(VerifierPass());
    });
  // Verify the output
  PB.registerOptimizerLastEPCallback(
    [](ModulePassManager& mpm, OptimizationLevel OL) {
      mpm.addPass(VerifierPass());
    });
  #endif

  // Passes specific for release build
  if (O != '0') {
    PB.registerPipelineStartEPCallback(
      [](ModulePassManager& mpm, OptimizationLevel OL) {
        mpm.addPass(createModuleToFunctionPassAdaptor(AddDiscriminatorsPass()));
      });
  }

  // Thread sanitizer
  if (opt->enable_tsan) {
    PB.registerOptimizerLastEPCallback(
      [](ModulePassManager& mpm, OptimizationLevel OL) {
        mpm.addPass(ModuleThreadSanitizerPass());
      });
  }

  // Select optimization level (See {llvm}/include/llvm/Passes/OptimizationLevel.h)
  OptimizationLevel optLevel;
  switch (O) { // buildctx/OptLevel
    case '0': optLevel = OptimizationLevel::O0; break;
    case '1': optLevel = OptimizationLevel::O1; break;
    case '2': optLevel = OptimizationLevel::O2; break;
    case '3': optLevel = OptimizationLevel::O3; break;
    case 's': optLevel = OptimizationLevel::Os; break;
    default:
      assertf(0,"invalid optlevel O='%c' (0x%02x)", O, O);
  }

  // Create pass manager
  ModulePassManager MPM;
  if (optLevel == OptimizationLevel::O0) {
    // note: buildO0DefaultPipeline requires optLevel == O0
    MPM = PB.buildO0DefaultPipeline(optLevel, opt->enable_lto);
    // include mem2reg; greatly reduces code size by using registers instead of stack memory
    // MPM.addPass(createModuleToFunctionPassAdaptor(PromotePass())); // mem2reg
  } else {
    // note: buildPerModuleDefaultPipeline requires optLevel != O0
    MPM = PB.buildPerModuleDefaultPipeline(optLevel, opt->enable_lto);
  }

  // run passes
  PreservedAnalyses pa = MPM.run(module, MAM);

  // print perf information
  if (enable_time_report)
    TimerGroup::printAll(errs());

  return 0;
}


static void printDebugLoc(const DebugLoc &DL, formatted_raw_ostream &OS) {
  OS << DL.getLine() << ":" << DL.getCol();
  if (DILocation *IDL = DL.getInlinedAt()) {
    OS << "@";
    printDebugLoc(IDL, OS);
  }
}
class CommentWriter : public AssemblyAnnotationWriter {
public:
  void emitFunctionAnnot(const Function *F,
                         formatted_raw_ostream &OS) override {
    OS << "; [#uses=" << F->getNumUses() << ']';  // Output # uses
    OS << '\n';
  }
  void printInfoComment(const Value &V, formatted_raw_ostream &OS) override {
    bool Padded = false;
    if (!V.getType()->isVoidTy()) {
      OS.PadToColumn(50);
      Padded = true;
      // Output # uses and type
      OS << "; [#uses=" << V.getNumUses() << " type=" << *V.getType() << "]";
    }
    if (const Instruction *I = dyn_cast<Instruction>(&V)) {
      if (const DebugLoc &DL = I->getDebugLoc()) {
        if (!Padded) {
          OS.PadToColumn(50);
          Padded = true;
          OS << ";";
        }
        OS << " [debug line = ";
        printDebugLoc(DL,OS);
        OS << "]";
      }
      if (const DbgDeclareInst *DDI = dyn_cast<DbgDeclareInst>(I)) {
        if (!Padded) {
          OS.PadToColumn(50);
          OS << ";";
        }
        OS << " [debug variable = " << DDI->getVariable()->getName() << "]";
      }
      else if (const DbgValueInst *DVI = dyn_cast<DbgValueInst>(I)) {
        if (!Padded) {
          OS.PadToColumn(50);
          OS << ";";
        }
        OS << " [debug variable = " << DVI->getVariable()->getName() << "]";
      }
    }
  }
};


static err_t emit_mc(
  CoLLVMModule* m, raw_fd_ostream& OS, CoLLVMEmitType etype, CoLLVMEmitFlags fl)
{
  Module& module = *unwrap((LLVMModuleRef)m->M);
  TargetMachine& TM = *reinterpret_cast<TargetMachine*>(assertnotnull(m->TM));
  module.setDataLayout(TM.createDataLayout());

  CodeGenFileType ft = CGFT_ObjectFile;
  if (etype == CoLLVMEmit_asm) {
    ft = CGFT_AssemblyFile;
  } else {
    assert(etype == CoLLVMEmit_obj);
  }

  legacy::PassManager PM;
  PM.add(createTargetTransformInfoWrapperPass(TM.getTargetIRAnalysis()));
  if (TM.addPassesToEmitFile(PM, OS, nullptr, ft)) {
    // "TargetMachine can't emit a file of this type"
    return ErrNotSupported;
  }

  PM.run(module);
  OS.flush();
  return 0;
}


static err_t emit_llvm(
  CoLLVMModule* m, raw_fd_ostream& OS, CoLLVMEmitType etype, CoLLVMEmitFlags fl)
{
  Module& module = *unwrap((LLVMModuleRef)m->M);
  bool isdebug = (fl & CoLLVMEmit_debug) != 0;
  bool preserveUseListOrder = isdebug;
    // If preserveUseListOrder, then include uselistorder directives so that
    // use-lists can be recreated when reading the assembly.
  if (etype == CoLLVMEmit_bc) {
    const ModuleSummaryIndex* index = nullptr;
    bool genHash = false;
    ModuleHash* modHash = nullptr;
    WriteBitcodeToFile(module, OS, preserveUseListOrder, index, genHash, modHash);
  } else {
    assert(etype == CoLLVMEmit_ir);
    std::unique_ptr<AssemblyAnnotationWriter> annotator;
    if (isdebug)
      annotator.reset(new CommentWriter());
    module.print(OS, annotator.get(), preserveUseListOrder, isdebug);
  }
  OS.flush();
  return 0;
}


err_t llvm_module_emit(
  CoLLVMModule* m, const char* filename, CoLLVMEmitType etype, CoLLVMEmitFlags fl)
{
  // open file output stream
  sys::fs::OpenFlags oflags = sys::fs::OF_None;
  if (etype & (CoLLVMEmit_asm | CoLLVMEmit_ir))
    oflags = sys::fs::OF_TextWithCRLF;
  std::error_code errcode;
  raw_fd_ostream dest(filename, errcode, oflags);
  if (errcode)
    return err_errnox(errcode.value());

  // call type-specific emitter
  err_t err = ErrInvalid;
  switch (etype) {
    case CoLLVMEmit_obj:
    case CoLLVMEmit_asm:
      err = emit_mc(m, dest, etype, fl);
      break;
    case CoLLVMEmit_ir:
    case CoLLVMEmit_bc:
      err = emit_llvm(m, dest, etype, fl);
      break;
  }

  // flush & close output
  dest.close();
  if (!err && dest.has_error())
    return err_errnox(dest.error().value());
  return err;
}


err_t llvm_write_archive(
  CoLLVMArchiveKind kind,
  const char* outfile, const char*const* infilev, u32 infilec, char** errmsg)
{
  // see llvm/tools/llvm-ar/llvm-ar.cpp
  object::Archive::Kind llvmkind;
  switch (kind) {
    case CoLLVMArchive_GNU:      llvmkind = object::Archive::K_GNU; break;
    case CoLLVMArchive_GNU64:    llvmkind = object::Archive::K_GNU64; break;
    case CoLLVMArchive_BSD:      llvmkind = object::Archive::K_BSD; break;
    case CoLLVMArchive_DARWIN:   llvmkind = object::Archive::K_DARWIN; break;
    case CoLLVMArchive_DARWIN64: llvmkind = object::Archive::K_DARWIN64; break;
    case CoLLVMArchive_COFF:     llvmkind = object::Archive::K_COFF; break;
    case CoLLVMArchive_AIXBIG:   llvmkind = object::Archive::K_AIXBIG; break;
    case CoLLVMArchive_none:     return ErrInvalid;
  }

  bool deterministic = true;
  Error err = Error::success();
  SmallVector<NewArchiveMember> inputs;

  for (u32 i = 0; i < infilec; i += 1) {
    Expected<NewArchiveMember> m = NewArchiveMember::getFile(infilev[i], deterministic);
    if (( err = m.takeError() )) {
      dlog("NewArchiveMember::getFile failed");
      return err_llvm(std::move(err), errmsg);
    }
    inputs.push_back(std::move(*m));
  }

  bool writeSymtab = true; // "ranlib"
  bool thin = false;
  err = writeArchive(outfile, inputs, writeSymtab, llvmkind, deterministic, thin);
  if (err)
    return err_llvm(std::move(err), errmsg);
  return 0;
}


LLVMContextRef CoLLVMContextCreate() {
  auto ctx = new LLVMContext();
  // Enable opaque pointers. This will be the default in future LLVM releases.
  ctx->setOpaquePointers(true);
  return wrap(ctx);
}


LLVMValueRef CoLLVMBuildGlobalString(
  LLVMBuilderRef B, const char* data, usize len, const char* vname)
{
  using namespace llvm;

  IRBuilder<>* builder = unwrap(B);
  auto& ctx = builder->getContext();

  bool addnull = false;
  Constant* strconst = ConstantDataArray::getString(ctx, StringRef(data, len), addnull);

  Module* M = builder->GetInsertBlock()->getParent()->getParent();

  auto* GV = new GlobalVariable(
    *M, strconst->getType(),
    true,
    GlobalValue::PrivateLinkage,
    strconst,
    vname,
    nullptr,
    GlobalVariable::NotThreadLocal,
    /*AddressSpace*/0);
  GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
  GV->setAlignment(Align(1));
  return wrap(GV);
}


u64 CoLLVMArrayTypeLength(LLVMTypeRef array_ty) {
  return unwrap<ArrayType>(array_ty)->getNumElements();
}


LLVMTypeRef CoLLVMOpaquePointerType(LLVMContextRef C, unsigned AddressSpace) {
  return wrap(PointerType::get(*unwrap(C), AddressSpace));
}


// DEPRECATED
CoLLVMOS LLVMGetHostOS() {
  return (CoLLVMOS)Triple(sys::getProcessTriple()).getOS();
}


char* LLVMGetMainExecutable(const char* argv0) {
  // This just needs to be some symbol in the binary; C++ doesn't
  // allow taking the address of ::main however.
  void* P = (void*)(intptr_t)LLVMGetMainExecutable;
  return strdup(sys::fs::getMainExecutable(argv0, P).c_str());
}
