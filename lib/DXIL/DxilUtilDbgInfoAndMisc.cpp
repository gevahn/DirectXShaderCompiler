///////////////////////////////////////////////////////////////////////////////
//                                                                           //
// DxilUtilDbgInfoAndMisc.cpp                                                //
// Copyright (C) Microsoft Corporation. All rights reserved.                 //
// This file is distributed under the University of Illinois Open Source     //
// License. See LICENSE.TXT for details.                                     //
//                                                                           //
// Dxil helper functions.                                                    //
//                                                                           //
///////////////////////////////////////////////////////////////////////////////


#include "dxc/DXIL/DxilTypeSystem.h"
#include "dxc/DXIL/DxilUtil.h"
#include "dxc/DXIL/DxilModule.h"
#include "dxc/DXIL/DxilOperations.h"
#include "dxc/HLSL/DxilConvergentName.h"
#include "dxc/Support/Global.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/IRBuilder.h"

using namespace llvm;
using namespace hlsl;

namespace hlsl {

namespace dxilutil {

std::unique_ptr<llvm::Module> LoadModuleFromBitcode(llvm::MemoryBuffer *MB,
                                                    llvm::LLVMContext &Ctx,
                                                    std::string &DiagStr) {
  // Note: the DiagStr is not used.
  auto pModule = llvm::parseBitcodeFile(MB->getMemBufferRef(), Ctx);
  if (!pModule) {
    return nullptr;
  }
  return std::unique_ptr<llvm::Module>(pModule.get().release());
}

std::unique_ptr<llvm::Module>
LoadModuleFromBitcodeLazy(std::unique_ptr<llvm::MemoryBuffer> &&MB,
                          llvm::LLVMContext &Ctx, std::string &DiagStr) {
  // Note: the DiagStr is not used.
  auto pModule = llvm::getLazyBitcodeModule(std::move(MB), Ctx, nullptr, true);
  if (!pModule) {
    return nullptr;
  }
  return std::unique_ptr<llvm::Module>(pModule.get().release());
}

std::unique_ptr<llvm::Module> LoadModuleFromBitcode(llvm::StringRef BC,
                                                    llvm::LLVMContext &Ctx,
                                                    std::string &DiagStr) {
  std::unique_ptr<llvm::MemoryBuffer> pBitcodeBuf(
      llvm::MemoryBuffer::getMemBuffer(BC, "", false));
  return LoadModuleFromBitcode(pBitcodeBuf.get(), Ctx, DiagStr);
}

DIGlobalVariable *FindGlobalVariableDebugInfo(GlobalVariable *GV,
                                              DebugInfoFinder &DbgInfoFinder) {
  struct GlobalFinder {
    GlobalVariable *GV;
    bool operator()(llvm::DIGlobalVariable *const arg) const {
      return arg->getVariable() == GV;
    }
  };
  GlobalFinder F = {GV};
  DebugInfoFinder::global_variable_iterator Found =
      std::find_if(DbgInfoFinder.global_variables().begin(),
                   DbgInfoFinder.global_variables().end(), F);
  if (Found != DbgInfoFinder.global_variables().end()) {
    return *Found;
  }
  return nullptr;
}

static void EmitWarningOrErrorOnInstruction(Instruction *I, Twine Msg,
                                            DiagnosticSeverity severity);

// If we don't have debug location and this is select/phi,
// try recursing users to find instruction with debug info.
// Only recurse phi/select and limit depth to prevent doing
// too much work if no debug location found.
static bool
EmitWarningOrErrorOnInstructionFollowPhiSelect(Instruction *I, Twine Msg,
                                               DiagnosticSeverity severity,
                                               unsigned depth = 0) {
  if (depth > 4)
    return false;
  if (I->getDebugLoc().get()) {
    EmitWarningOrErrorOnInstruction(I, Msg, severity);
    return true;
  }
  if (isa<PHINode>(I) || isa<SelectInst>(I)) {
    for (auto U : I->users())
      if (Instruction *UI = dyn_cast<Instruction>(U))
        if (EmitWarningOrErrorOnInstructionFollowPhiSelect(UI, Msg, severity,
                                                           depth + 1))
          return true;
  }
  return false;
}

static void EmitWarningOrErrorOnInstruction(Instruction *I, Twine Msg,
                                            DiagnosticSeverity severity) {
  const DebugLoc &DL = I->getDebugLoc();
  if (!DL.get() && (isa<PHINode>(I) || isa<SelectInst>(I))) {
    if (EmitWarningOrErrorOnInstructionFollowPhiSelect(I, Msg, severity))
      return;
  }

  I->getContext().diagnose(
      DiagnosticInfoDxil(I->getParent()->getParent(), DL.get(), Msg, severity));
}

void EmitErrorOnInstruction(Instruction *I, Twine Msg) {
  EmitWarningOrErrorOnInstruction(I, Msg, DiagnosticSeverity::DS_Error);
}

void EmitWarningOnInstruction(Instruction *I, Twine Msg) {
  EmitWarningOrErrorOnInstruction(I, Msg, DiagnosticSeverity::DS_Warning);
}

static void EmitWarningOrErrorOnFunction(llvm::LLVMContext &Ctx, Function *F,
                                         Twine Msg,
                                         DiagnosticSeverity severity) {
  DILocation *DLoc = nullptr;

  if (DISubprogram *DISP = getDISubprogram(F)) {
    DLoc = DILocation::get(F->getContext(), DISP->getLine(), 0, DISP,
                           nullptr /*InlinedAt*/);
  }
  Ctx.diagnose(DiagnosticInfoDxil(F, DLoc, Msg, severity));
}

void EmitErrorOnFunction(llvm::LLVMContext &Ctx, Function *F, Twine Msg) {
  EmitWarningOrErrorOnFunction(Ctx, F, Msg, DiagnosticSeverity::DS_Error);
}

void EmitWarningOnFunction(llvm::LLVMContext &Ctx, Function *F, Twine Msg) {
  EmitWarningOrErrorOnFunction(Ctx, F, Msg, DiagnosticSeverity::DS_Warning);
}

static void EmitWarningOrErrorOnGlobalVariable(llvm::LLVMContext &Ctx,
                                               GlobalVariable *GV, Twine Msg,
                                               DiagnosticSeverity severity) {
  DIGlobalVariable *DIV = nullptr;

  if (GV) {
    Module &M = *GV->getParent();
    if (hasDebugInfo(M)) {
      DebugInfoFinder FinderObj;
      DebugInfoFinder &Finder = FinderObj;
      // Debug modules have no dxil modules. Use it if you got it.
      if (M.HasDxilModule())
        Finder = M.GetDxilModule().GetOrCreateDebugInfoFinder();
      else
        Finder.processModule(M);
      DIV = FindGlobalVariableDebugInfo(GV, Finder);
    }
  }

  Ctx.diagnose(DiagnosticInfoDxil(nullptr /*Function*/, DIV, Msg, severity));
}

void EmitErrorOnGlobalVariable(llvm::LLVMContext &Ctx, GlobalVariable *GV,
                               Twine Msg) {
  EmitWarningOrErrorOnGlobalVariable(Ctx, GV, Msg,
                                     DiagnosticSeverity::DS_Error);
}

void EmitWarningOnGlobalVariable(llvm::LLVMContext &Ctx, GlobalVariable *GV,
                                 Twine Msg) {
  EmitWarningOrErrorOnGlobalVariable(Ctx, GV, Msg,
                                     DiagnosticSeverity::DS_Warning);
}

const char *kResourceMapErrorMsg =
    "local resource not guaranteed to map to unique global resource.";
void EmitResMappingError(Instruction *Res) {
  EmitErrorOnInstruction(Res, kResourceMapErrorMsg);
}

// Mostly just a locationless diagnostic output
static void EmitWarningOrErrorOnContext(LLVMContext &Ctx, Twine Msg,
                                        DiagnosticSeverity severity) {
  Ctx.diagnose(DiagnosticInfoDxil(nullptr /*Func*/, Msg, severity));
}

void EmitErrorOnContext(LLVMContext &Ctx, Twine Msg) {
  EmitWarningOrErrorOnContext(Ctx, Msg, DiagnosticSeverity::DS_Error);
}

void EmitWarningOnContext(LLVMContext &Ctx, Twine Msg) {
  EmitWarningOrErrorOnContext(Ctx, Msg, DiagnosticSeverity::DS_Warning);
}

void EmitNoteOnContext(LLVMContext &Ctx, Twine Msg) {
  EmitWarningOrErrorOnContext(Ctx, Msg, DiagnosticSeverity::DS_Note);
}

static DbgValueInst *FindDbgValueInst(Value *Val) {
  if (auto *ValAsMD = LocalAsMetadata::getIfExists(Val)) {
    if (auto *ValMDAsVal =
            MetadataAsValue::getIfExists(Val->getContext(), ValAsMD)) {
      for (User *ValMDUser : ValMDAsVal->users()) {
        if (DbgValueInst *DbgValInst = dyn_cast<DbgValueInst>(ValMDUser))
          return DbgValInst;
      }
    }
  }
  return nullptr;
}

void MigrateDebugValue(Value *Old, Value *New) {
  DbgValueInst *DbgValInst = FindDbgValueInst(Old);
  if (DbgValInst == nullptr)
    return;

  DbgValInst->setOperand(
      0, MetadataAsValue::get(New->getContext(), ValueAsMetadata::get(New)));

  // Move the dbg value after the new instruction
  if (Instruction *NewInst = dyn_cast<Instruction>(New)) {
    if (NewInst->getNextNode() != DbgValInst) {
      DbgValInst->removeFromParent();
      DbgValInst->insertAfter(NewInst);
    }
  }
}

// Propagates any llvm.dbg.value instruction for a given vector
// to the elements that were used to create it through a series
// of insertelement instructions.
//
// This is used after lowering a vector-returning intrinsic.
// If we just keep the debug info on the recomposed vector,
// we will lose it when we break it apart again during later
// optimization stages.
void TryScatterDebugValueToVectorElements(Value *Val) {
  if (!isa<InsertElementInst>(Val) || !Val->getType()->isVectorTy())
    return;

  DbgValueInst *VecDbgValInst = FindDbgValueInst(Val);
  if (VecDbgValInst == nullptr)
    return;

  Type *ElemTy = Val->getType()->getVectorElementType();
  DIBuilder DbgInfoBuilder(*VecDbgValInst->getModule());
  unsigned ElemSizeInBits =
      VecDbgValInst->getModule()->getDataLayout().getTypeSizeInBits(ElemTy);

  DIExpression *ParentBitPiece = VecDbgValInst->getExpression();
  if (ParentBitPiece != nullptr && !ParentBitPiece->isBitPiece())
    ParentBitPiece = nullptr;

  while (InsertElementInst *InsertElt = dyn_cast<InsertElementInst>(Val)) {
    Value *NewElt = InsertElt->getOperand(1);
    unsigned EltIdx = static_cast<unsigned>(
        cast<ConstantInt>(InsertElt->getOperand(2))->getLimitedValue());
    unsigned OffsetInBits = EltIdx * ElemSizeInBits;

    if (ParentBitPiece) {
      assert(OffsetInBits + ElemSizeInBits <=
                 ParentBitPiece->getBitPieceSize() &&
             "Nested bit piece expression exceeds bounds of its parent.");
      OffsetInBits += ParentBitPiece->getBitPieceOffset();
    }

    DIExpression *DIExpr =
        DbgInfoBuilder.createBitPieceExpression(OffsetInBits, ElemSizeInBits);
    // Offset is basically unused and deprecated in later LLVM versions.
    // Emit it as zero otherwise later versions of the bitcode reader will drop
    // the intrinsic.
    DbgInfoBuilder.insertDbgValueIntrinsic(
        NewElt, /* Offset */ 0, VecDbgValInst->getVariable(), DIExpr,
        VecDbgValInst->getDebugLoc(), InsertElt);
    Val = InsertElt->getOperand(0);
  }
}

} // namespace dxilutil
} // namespace hlsl

///////////////////////////////////////////////////////////////////////////////

namespace {
class DxilLoadMetadata : public ModulePass {
public:
  static char ID; // Pass identification, replacement for typeid
  explicit DxilLoadMetadata () : ModulePass(ID) {}

  StringRef getPassName() const override { return "HLSL load DxilModule from metadata"; }

  bool runOnModule(Module &M) override {
    if (!M.HasDxilModule()) {
      (void)M.GetOrCreateDxilModule();
      return true;
    }

    return false;
  }
};
}

char DxilLoadMetadata::ID = 0;

ModulePass *llvm::createDxilLoadMetadataPass() {
  return new DxilLoadMetadata();
}

INITIALIZE_PASS(DxilLoadMetadata, "hlsl-dxilload", "HLSL load DxilModule from metadata", false, false)
