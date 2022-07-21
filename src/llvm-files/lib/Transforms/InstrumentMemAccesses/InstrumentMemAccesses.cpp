//===-- InstrumentMemAccesses.cpp - Instrument memory accesses ------------===//
///
/// \file
/// This pass instruments memory accesses (i.e., \p load and store instructions)
/// to discover their def site.
///
//===----------------------------------------------------------------------===//

#include <set>

#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/InstructionSimplify.h>
#include <llvm/Analysis/MemoryBuiltins.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/Utils/Local.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>

#include "Support/FuzzallocUtils.h"
#include "debug.h"     // from afl
#include "fuzzalloc/fuzzalloc.h"
#include "fuzzalloc/Metadata.h"

using namespace llvm;

#define DEBUG_TYPE "fuzzalloc-inst-mem-accesses"

enum Sensitivity {
  MemRead,
  MemWrite,
  MemAccess,

  MemReadOffset,
  MemWriteOffset,
  MemAccessOffset,
};

enum Fuzzer {
  DebugLog,
  AFL,
};

static cl::opt<Sensitivity> ClSensitivity(
    cl::desc("Sensitivity:"),
    cl::values(
        clEnumValN(Sensitivity::MemRead, "mem-read",
                   "Instrument read instructions"),
        clEnumValN(Sensitivity::MemWrite, "mem-write",
                   "Instrument write instructions"),
        clEnumValN(Sensitivity::MemAccess, "mem-access",
                   "Instrument read and write instructions"),
        clEnumValN(Sensitivity::MemReadOffset, "mem-read-offset",
                   "Instrument read instructions with offset"),
        clEnumValN(Sensitivity::MemWriteOffset, "mem-write-offset",
                   "Instrument write instructions with offset"),
        clEnumValN(Sensitivity::MemAccessOffset, "mem-access-offset",
                   "Instrument read and write instructions with offset")),
    cl::init(MemAccess));

static cl::opt<Fuzzer> ClFuzzerInstrument(
    cl::desc("Fuzzer instrumentation:"),
    cl::values(clEnumValN(Fuzzer::DebugLog, "debug-log", "Debug log"),
               clEnumValN(Fuzzer::AFL, "afl", "AFL")),
    cl::init(AFL));

STATISTIC(NumOfInstrumentedMemAccesses,
          "Number of memory accesses instrumented.");

// Debug logging
static const char *const DbgMemAccessName = "__mem_access";

// AFL-style fuzzing
static const char *const AFLMapName = "__afl_area_ptr";

namespace {

/// Keep track of what to instrument.
struct InstrumentFlags {
  bool InstrumentReads;
  bool InstrumentWrites;
  bool InstrumentAtomics;
  bool UseOffset;

  InstrumentFlags(Sensitivity S) {
    switch (S) {
    case MemRead:
      InstrumentReads = true;
      InstrumentWrites = false;
      InstrumentAtomics = false;
      UseOffset = false;
      break;
    case MemWrite:
      InstrumentReads = false;
      InstrumentWrites = true;
      InstrumentAtomics = false;
      UseOffset = false;
      break;
    case MemAccess:
      InstrumentReads = true;
      InstrumentWrites = true;
      InstrumentAtomics = false;
      UseOffset = false;
      break;
    case MemReadOffset:
      InstrumentReads = true;
      InstrumentWrites = false;
      InstrumentAtomics = false;
      UseOffset = true;
      break;
    case MemWriteOffset:
      InstrumentReads = false;
      InstrumentWrites = true;
      InstrumentAtomics = false;
      UseOffset = true;
      break;
    case MemAccessOffset:
      InstrumentReads = true;
      InstrumentWrites = true;
      InstrumentAtomics = false;
      UseOffset = true;
      break;
    }
  }
};

class InstrumentMemAccesses : public ModulePass {
private:
  DataLayout *DL;

  IntegerType *Int8Ty;
  IntegerType *Int64Ty;
  IntegerType *IntPtrTy;
  IntegerType *TagTy;

  ConstantInt *TagShiftSize;
  ConstantInt *TagMask;
  ConstantInt *DefaultTag;
  ConstantInt *MinTag;
  ConstantInt *MaxTag;
  ConstantInt *AFLInc;
  ConstantInt *HashMul;

  InstrumentFlags *InstFlags;

  Value *getDefSite(Value *, IRBuilder<> &) const;
  Value *isInterestingMemoryAccess(Instruction *, bool *, uint64_t *,
                                   unsigned *, Value **) const;

public:
  static char ID;
  InstrumentMemAccesses() : ModulePass(ID) {}

  void getAnalysisUsage(AnalysisUsage &) const override;
  bool doInitialization(Module &) override;
  bool runOnModule(Module &) override;

private:
  //
  // Debug logging
  //

  Function *DbgMemAccessFn;
  void doDebugInstrument(Instruction *, Value *) const;

  //
  // AFL-style fuzzing
  //

  FunctionCallee ReadPCAsm;
  GlobalVariable *AFLMapPtr;

  void doAFLInstrument(Instruction *, Value *) const;
};

} // anonymous namespace

char InstrumentMemAccesses::ID = 0;

// Adapted from llvm::checkSanitizerInterfaceFunction
static Function *checkInstrumentationFunc(FunctionCallee FuncOrBitcast) {
  assert(FuncOrBitcast && "Invalid function callee");
  if (isa<Function>(FuncOrBitcast.getCallee()->stripPointerCasts())) {
    return cast<Function>(FuncOrBitcast.getCallee()->stripPointerCasts());
  }

  FuncOrBitcast.getCallee()->print(errs());
  errs() << '\n';
  std::string Err;
  raw_string_ostream OS(Err);
  OS << "Instrumentation function redefined: " << *FuncOrBitcast.getCallee();
  OS.flush();
  report_fatal_error(Err);
}

// Adapted from llvm::AddressSanitizer::isSafeAccess
static bool isSafeAccess(ObjectSizeOffsetVisitor &ObjSizeVis, Value *Addr,
                         uint64_t TypeSize) {
  SizeOffsetType SizeOffset = ObjSizeVis.compute(Addr);
  if (!ObjSizeVis.bothKnown(SizeOffset)) {
    return false;
  }

  uint64_t Size = SizeOffset.first.getZExtValue();
  int64_t Offset = SizeOffset.second.getSExtValue();

  // Three checks are required to ensure safety:
  // - Offset >= 0 (since the offset is given from the base ptr)
  // - Size >= Offset (unsigned)
  // - Size - Offset >= NeededSize (unsigned)
  return Offset >= 0 && Size >= uint64_t(Offset) &&
         Size - uint64_t(Offset) >= TypeSize / CHAR_BIT;
}

// Adapted from llvm::AddressSanitizer::getAllocaSizeInBytes
static uint64_t getAllocaSizeInBytes(const AllocaInst &AI) {
  uint64_t ArraySize = 1;

  if (AI.isArrayAllocation()) {
    const ConstantInt *CI = dyn_cast<ConstantInt>(AI.getArraySize());
    assert(CI && "Non-constant array size");
    ArraySize = CI->getZExtValue();
  }

  Type *Ty = AI.getAllocatedType();
  uint64_t SizeInBytes = AI.getModule()->getDataLayout().getTypeAllocSize(Ty);

  return SizeInBytes * ArraySize;
}

// Adapted from llvm::AddressSanitizer::isInterestingAlloca
static bool isInterestingAlloca(const AllocaInst &AI) {
  return AI.getAllocatedType()->isSized() &&
         // alloca() may be called with 0 size, ignore it
         ((!AI.isStaticAlloca()) || getAllocaSizeInBytes(AI) > 0) &&
         // We are only interested in allocas not promotable to registers
         !isAllocaPromotable(&AI) &&
         // inalloca allocas are not treated as static, and we don't want
         // dynamic alloca instrumentation for them also
         !AI.isUsedWithInAlloca() &&
         // swifterror allocas are register promoted by ISel
         !AI.isSwiftError();
}

// Adapted from llvm::GetUnderlyingObject
static GEPOperator *getUseSiteGEP(Value *V, DataLayout &DL,
                                  unsigned MaxLookup = 6) {
  if (!V->getType()->isPointerTy()) {
    return nullptr;
  }

  for (unsigned Count = 0; MaxLookup == 0 || Count < MaxLookup; ++Count) {
    if (GEPOperator *GEP = dyn_cast<GEPOperator>(V)) {
      return GEP;
    } else if (Operator::getOpcode(V) == Instruction::BitCast ||
               Operator::getOpcode(V) == Instruction::AddrSpaceCast) {
      V = cast<Operator>(V)->getOperand(0);
    } else {
      if (auto *Call = dyn_cast<CallBase>(V)) {
        if (auto *RP = getArgumentAliasingToReturnedPointer(Call, false)) {
          V = RP;
          continue;
        }
      }

      if (Instruction *I = dyn_cast<Instruction>(V)) {
        if (Value *Simplified = SimplifyInstruction(I, {DL, I})) {
          V = Simplified;
          continue;
        }
      }

      return nullptr;
    }

    assert(V->getType()->isPointerTy() && "Unexpected operand type");
  }

  return nullptr;
}

Value *InstrumentMemAccesses::getDefSite(Value *Ptr, IRBuilder<> &IRB) const {
  // Cast the memory access pointer to an integer and mask out the mspace tag
  // from the pointer by right-shifting by 32 bits
  auto *PtrAsInt = IRB.CreatePtrToInt(Ptr, this->Int64Ty);
  if (auto PtrAsIntInst = dyn_cast<Instruction>(PtrAsInt)) {
    setNoSanitizeMetadata(PtrAsIntInst);
  }
  auto *MSpaceTag = IRB.CreateAnd(IRB.CreateLShr(PtrAsInt, this->TagShiftSize),
                                  this->TagMask);

  auto *DefSite =
      IRB.CreateIntCast(MSpaceTag, this->TagTy, /* isSigned */ false,
                        Ptr->getName() + ".def_site");

  // Ensure that the def site is within the valid range of allowed def sites.
  // Otherwise assign it the default tag
  auto *SelectComp = IRB.CreateAnd(IRB.CreateICmpUGE(DefSite, this->MinTag),
                                   IRB.CreateICmpULE(DefSite, this->MaxTag));
  return IRB.CreateSelect(SelectComp, DefSite, this->DefaultTag,
                          DefSite->getName() + "_in_range");
}

// Adapted from llvm::AddressSanitizer::isInterestingMemoryAccess
Value *InstrumentMemAccesses::isInterestingMemoryAccess(
    Instruction *I, bool *IsWrite, uint64_t *TypeSize, unsigned *Alignment,
    Value **MaybeMask = nullptr) const {
  Value *PtrOperand = nullptr;

  if (auto *LI = dyn_cast<LoadInst>(I)) {
    if (!this->InstFlags->InstrumentReads) {
      return nullptr;
    }

    *IsWrite = false;
    *TypeSize = this->DL->getTypeStoreSizeInBits(LI->getType());
    *Alignment = LI->getAlignment();
    PtrOperand = LI->getPointerOperand();
  } else if (auto *SI = dyn_cast<StoreInst>(I)) {
    if (!this->InstFlags->InstrumentWrites) {
      return nullptr;
    }

    *IsWrite = true;
    *TypeSize =
        this->DL->getTypeStoreSizeInBits(SI->getValueOperand()->getType());
    *Alignment = SI->getAlignment();
    PtrOperand = SI->getPointerOperand();
  } else if (auto *RMW = dyn_cast<AtomicRMWInst>(I)) {
    if (!this->InstFlags->InstrumentAtomics) {
      return nullptr;
    }

    *IsWrite = true;
    *TypeSize =
        this->DL->getTypeStoreSizeInBits(RMW->getValOperand()->getType());
    *Alignment = 0;
    PtrOperand = RMW->getPointerOperand();
  } else if (auto *XCHG = dyn_cast<AtomicCmpXchgInst>(I)) {
    if (!this->InstFlags->InstrumentAtomics) {
      return nullptr;
    }

    *IsWrite = true;
    *TypeSize =
        this->DL->getTypeStoreSizeInBits(XCHG->getCompareOperand()->getType());
    *Alignment = 0;
    PtrOperand = XCHG->getPointerOperand();
  } else if (auto *CI = dyn_cast<CallInst>(I)) {
    auto *F = CI->getCalledFunction();
    if (F && (F->getName().startswith("llvm.masked.load.") ||
              F->getName().startswith("llvm.masked.store."))) {
      unsigned OpOffset = 0;

      if (F->getName().startswith("llvm.masked.store.")) {
        if (!this->InstFlags->InstrumentWrites) {
          return nullptr;
        }

        // Masked store has an initial operand for the value
        OpOffset = 1;
        *IsWrite = true;
      } else {
        if (!this->InstFlags->InstrumentReads) {
          return nullptr;
        }

        *IsWrite = false;
      }

      auto *BasePtr = CI->getOperand(0 + OpOffset);
      auto *Ty = cast<PointerType>(BasePtr->getType())->getElementType();
      *TypeSize = this->DL->getTypeStoreSizeInBits(Ty);

      if (auto *AlignmentConstant =
              dyn_cast<ConstantInt>(CI->getOperand(1 + OpOffset))) {
        *Alignment = (unsigned)AlignmentConstant->getZExtValue();
      } else {
        *Alignment = 1; // No alignment guarantees
      }

      if (MaybeMask) {
        *MaybeMask = CI->getOperand(2 + OpOffset);
        PtrOperand = BasePtr;
      }
    }
  }

  if (PtrOperand) {
    // Do not instrument accesses from different address spaces; we cannot
    // deal with them
    Type *PtrTy = cast<PointerType>(PtrOperand->getType()->getScalarType());
    if (PtrTy->getPointerAddressSpace() != 0) {
      return nullptr;
    }

    // Ignore swifterror addresses
    if (PtrOperand->isSwiftError()) {
      return nullptr;
    }
  }

  // Treat memory accesses to promotable allocas as non-interesting since they
  // will not cause memory violations
  if (auto *AI = dyn_cast_or_null<AllocaInst>(PtrOperand)) {
    return isInterestingAlloca(*AI) ? AI : nullptr;
  }

  return PtrOperand;
}

void InstrumentMemAccesses::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TargetLibraryInfoWrapperPass>();
}

bool InstrumentMemAccesses::doInitialization(Module &M) {
  this->DL = new DataLayout(M.getDataLayout());

  LLVMContext &C = M.getContext();
  IntegerType *SizeTTy = this->DL->getIntPtrType(C);

  this->Int8Ty = Type::getInt8Ty(C);
  this->Int64Ty = Type::getInt64Ty(C);
  this->IntPtrTy = Type::getIntNTy(C, this->DL->getPointerSizeInBits());
  this->TagTy = Type::getIntNTy(C, NUM_TAG_BITS);

  this->TagShiftSize = ConstantInt::get(SizeTTy, FUZZALLOC_TAG_SHIFT);
  this->TagMask = ConstantInt::get(this->TagTy, FUZZALLOC_TAG_MASK);
  this->DefaultTag = ConstantInt::get(this->TagTy, FUZZALLOC_DEFAULT_TAG);
  this->MinTag = ConstantInt::get(this->TagTy, ClDefSiteTagMin);
  this->MaxTag = ConstantInt::get(this->TagTy, ClDefSiteTagMax);
  this->AFLInc = ConstantInt::get(this->Int8Ty, 1);
  this->HashMul = ConstantInt::get(this->TagTy, 3);

  this->InstFlags = new InstrumentFlags(ClSensitivity);

  return false;
}

bool InstrumentMemAccesses::runOnModule(Module &M) {
  LLVMContext &C = M.getContext();
  Type *VoidTy = Type::getVoidTy(C);

  if (ClFuzzerInstrument == Fuzzer::DebugLog) {
    //
    // Debug logging
    //

    this->DbgMemAccessFn = checkInstrumentationFunc(M.getOrInsertFunction(
        DbgMemAccessName, VoidTy, this->TagTy, this->Int64Ty));
    this->DbgMemAccessFn->addParamAttr(0, Attribute::ZExt);
    this->DbgMemAccessFn->addParamAttr(1, Attribute::SExt);
  } else if (ClFuzzerInstrument == Fuzzer::AFL) {
    //
    // AFL-style fuzzing
    //

    auto *ReadPCAsmTy = FunctionType::get(this->Int64Ty, /*isVarArg=*/false);
    this->ReadPCAsm = FunctionCallee(
        ReadPCAsmTy,
        InlineAsm::get(ReadPCAsmTy, "leaq (%rip), $0",
                       /* Constraints */ "=r", /* hasSideEffects */ false));
    this->AFLMapPtr = new GlobalVariable(
        M, PointerType::getUnqual(this->Int8Ty), /* isConstant */ false,
        GlobalValue::ExternalLinkage, /* Initializer */ nullptr, AFLMapName);
  }

  // For determining whether to instrument a memory dereference
  ObjectSizeOpts ObjSizeOpts;
  ObjSizeOpts.RoundToAlign = true;

  for (auto &F : M.functions()) {
    // Don't instrument our own constructors/destructors
    if (F.getName().startswith("fuzzalloc.init_") ||
        F.getName().startswith("fuzzalloc.alloc_") ||
        F.getName().startswith("fuzzalloc.free_")) {
      continue;
    }

    // We want to instrument every address only once per basic block (unless
    // there are calls between uses that access memory)
    SmallPtrSet<Value *, 16> TempsToInstrument;
    SmallVector<Instruction *, 16> ToInstrument;
    bool IsWrite;
    unsigned Alignment;
    uint64_t TypeSize;

    // For determining whether to instrument a memory dereference
    const TargetLibraryInfo *TLI =
        &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
    ObjectSizeOffsetVisitor ObjSizeVis(*this->DL, TLI, C, ObjSizeOpts);

    for (auto &BB : F) {
      TempsToInstrument.clear();

      for (auto &Inst : BB) {
        Value *MaybeMask = nullptr;

        if (Value *Addr = isInterestingMemoryAccess(&Inst, &IsWrite, &TypeSize,
                                                    &Alignment, &MaybeMask)) {
          Value *Obj = getUnderlyingObject(Addr);

          // If we have a mask, skip instrumentation if we've already
          // instrumented the full object. But don't add to TempsToInstrument
          // because we might get another load/store with a different mask
          if (MaybeMask) {
            if (TempsToInstrument.count(Obj)) {
              // We've seen this (whole) temp in the current BB
              continue;
            }
          } else {
            if (!TempsToInstrument.insert(Obj).second) {
              // We've seen this temp in the current BB
              continue;
            }
          }
        }
        // TODO pointer comparisons?
        else if (const auto *CB = dyn_cast<CallBase>(&Inst)) {
          // A call that accesses memory inside the basic block. If the call
          // is indirect (getCalledFunction returns null) then we don't know
          // so we just have to assume that it accesses memory
          auto *CalledF = CB->getCalledFunction();
          bool MaybeAccessMemory =
              isa_and_nonnull<Function>(CalledF)
                  ? !cast<Function>(CalledF)->doesNotAccessMemory()
                  : true;
          if (MaybeAccessMemory) {
            TempsToInstrument.clear();
          }

          continue;
        }

        // Finally, check if the instruction has the "noinstrument" metadata
        // attached to it (from the array heapify pass)
        if (!Inst.getMetadata(M.getMDKindID(kFuzzallocNoInstrumentMD))) {
          ToInstrument.push_back(&Inst);
        }
      }
    }

    // Nothing to instrument in this function
    if (ToInstrument.empty()) {
      continue;
    }

    // Instrument memory operations
    for (auto *I : ToInstrument) {
      if (Value *Addr =
              isInterestingMemoryAccess(I, &IsWrite, &TypeSize, &Alignment)) {
        Value *Obj = getUnderlyingObject(Addr);
        // A direct inbounds access to a stack variable is always valid
        if (isa<AllocaInst>(Obj) && isSafeAccess(ObjSizeVis, Addr, TypeSize)) {
          continue;
        }

        // Don't instrument vtable/typeinfo loads/stores
        if (isVTableOrTypeInfo(Obj)) {
          continue;
        }

        if (ClFuzzerInstrument == Fuzzer::DebugLog) {
          doDebugInstrument(I, Addr);
        } else if (ClFuzzerInstrument == Fuzzer::AFL) {
          doAFLInstrument(I, Addr);
        }
      }
    }
  }

  printStatistic(M, NumOfInstrumentedMemAccesses);

  return NumOfInstrumentedMemAccesses > 0;
}

//===----------------------------------------------------------------------===//
//
// Debug logging
//
//===----------------------------------------------------------------------===//

/// Instrument the Instruction `I` that accesses the memory at `Ptr`.
void InstrumentMemAccesses::doDebugInstrument(Instruction *I,
                                              Value *Ptr) const {
  LLVM_DEBUG(dbgs() << "instrumenting " << *Ptr << " in " << *I << '\n');

  auto *M = I->getModule();
  IRBuilder<> IRB(I);
  LLVMContext &C = IRB.getContext();

  // This metadata can be used by the static pointer analysis
  I->setMetadata(M->getMDKindID(kFuzzallocInstrumentedDerefMD),
                 MDNode::get(C, None));

  // Get the def site
  auto *DefSite = getDefSite(Ptr, IRB);

  // Get the use site offset. Default to zero if we can't determine the offset
  Value *UseSiteOffset = Constant::getNullValue(this->Int64Ty);
  if (this->InstFlags->UseOffset) {
    auto *UseSiteGEP = getUseSiteGEP(Ptr, *this->DL);
    if (UseSiteGEP) {
      UseSiteOffset = EmitGEPOffset(&IRB, *this->DL, UseSiteGEP);
    }
  }
  UseSiteOffset->setName(Ptr->getName() + ".offset");
  auto *UseSiteOffsetInt64 =
      IRB.CreateSExtOrTrunc(UseSiteOffset, this->Int64Ty);

  // Call the debub logging function
  IRB.CreateCall(this->DbgMemAccessFn, {DefSite, UseSiteOffsetInt64});

  NumOfInstrumentedMemAccesses++;
}

//===----------------------------------------------------------------------===//
//
// AFL-style fuzzing
//
//===----------------------------------------------------------------------===//

/// Instrument the Instruction `I` that accesses the memory at `Ptr`.
void InstrumentMemAccesses::doAFLInstrument(Instruction *I, Value *Ptr) const {
  LLVM_DEBUG(dbgs() << "instrumenting " << *Ptr << " in " << *I << '\n');

  auto *M = I->getModule();
  IRBuilder<> IRB(I);
  LLVMContext &C = IRB.getContext();

  // This metadata can be used by the static pointer analysis
  I->setMetadata(M->getMDKindID(kFuzzallocInstrumentedDerefMD),
                 MDNode::get(C, None));

  // Get the def site
  auto *DefSite = getDefSite(Ptr, IRB);

  // Get the use site offset. Default to zero if we can't determine the offset
  Value *UseSiteOffset = Constant::getNullValue(this->Int64Ty);
  if (this->InstFlags->UseOffset) {
    auto *UseSiteGEP = getUseSiteGEP(Ptr, *this->DL);
    if (UseSiteGEP) {
      UseSiteOffset = EmitGEPOffset(&IRB, *this->DL, UseSiteGEP);
    }
  }
  UseSiteOffset->setName(Ptr->getName() + ".offset");
  auto *UseSiteOffsetInt64 =
      IRB.CreateSExtOrTrunc(UseSiteOffset, this->Int64Ty);

  // Use the PC as the use site identifier
  auto *UseSite =
      IRB.CreateIntCast(IRB.CreateCall(this->ReadPCAsm), this->TagTy,
                        /* isSigned */ false, Ptr->getName() + ".use_site");

  // Incorporate the memory access offset into the use site
  if (this->InstFlags->UseOffset) {
    UseSite = IRB.CreateAdd(UseSite,
                            IRB.CreateIntCast(UseSiteOffsetInt64, this->TagTy,
                                              /* isSigned */ true));
  }

  // Load the AFL bitmap
  auto *AFLMap = IRB.CreateLoad(this->AFLMapPtr);

  // Hash the allocation site and use site to index into the bitmap
  //
  // zext is necessary otherwise we end up using signed indices
  //
  // Hash algorithm: ((3 * (def_site - DEFAULT_TAG)) ^ use_site) - use_site
  auto *Hash = IRB.CreateSub(
      IRB.CreateXor(IRB.CreateMul(this->HashMul,
                                  IRB.CreateSub(DefSite, this->DefaultTag)),
                    UseSite),
      UseSite, Ptr->getName() + ".def_use_hash");
  auto *AFLMapIdx = IRB.CreateGEP(
      AFLMap, IRB.CreateZExt(Hash, IRB.getInt32Ty()), "__afl_area_ptr_idx");

  // Update the bitmap only if the def site is not the default tag
  auto *CounterLoad = IRB.CreateLoad(AFLMapIdx);
  auto *Incr = IRB.CreateAdd(CounterLoad, this->AFLInc);
  auto *CounterStore = IRB.CreateStore(Incr, AFLMapIdx);

  setNoSanitizeMetadata(AFLMap);
  setNoSanitizeMetadata(CounterLoad);
  setNoSanitizeMetadata(CounterStore);

  NumOfInstrumentedMemAccesses++;
}

//===----------------------------------------------------------------------===//

static RegisterPass<InstrumentMemAccesses>
    X("fuzzalloc-inst-mem-accesses",
      "Instrument memory accesses to find their def site", false, false);

static void registerInstrumentMemAccessesPass(const PassManagerBuilder &,
                                              legacy::PassManagerBase &PM) {
  PM.add(new InstrumentMemAccesses());
}

static RegisterStandardPasses
    RegisterInstrumentMemAccessesPass(PassManagerBuilder::EP_OptimizerLast,
                                      registerInstrumentMemAccessesPass);

static RegisterStandardPasses RegisterInstrumentMemAccessesPass0(
    PassManagerBuilder::EP_EnabledOnOptLevel0,
    registerInstrumentMemAccessesPass);
