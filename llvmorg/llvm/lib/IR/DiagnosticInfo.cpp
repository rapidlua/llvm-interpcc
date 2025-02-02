//===- llvm/Support/DiagnosticInfo.cpp - Diagnostic Definitions -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the different classes involved in low level diagnostics.
//
// Diagnostics reporting is still done as part of the LLVMContext.
//===----------------------------------------------------------------------===//

#include "llvm/IR/DiagnosticInfo.h"
#include "LLVMContextImpl.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Support/raw_ostream.h"
#include <atomic>
#include <cassert>
#include <memory>
#include <string>

using namespace llvm;

int llvm::getNextAvailablePluginDiagnosticKind() {
  static std::atomic<int> PluginKindID(DK_FirstPluginKind);
  return ++PluginKindID;
}

const char *OptimizationRemarkAnalysis::AlwaysPrint = "";

DiagnosticInfoInlineAsm::DiagnosticInfoInlineAsm(const Instruction &I,
                                                 const Twine &MsgStr,
                                                 DiagnosticSeverity Severity)
    : DiagnosticInfo(DK_InlineAsm, Severity), MsgStr(MsgStr), Instr(&I) {
  if (const MDNode *SrcLoc = I.getMetadata("srcloc")) {
    if (SrcLoc->getNumOperands() != 0)
      if (const auto *CI =
              mdconst::dyn_extract<ConstantInt>(SrcLoc->getOperand(0)))
        LocCookie = CI->getZExtValue();
  }
}

void DiagnosticInfoInlineAsm::print(DiagnosticPrinter &DP) const {
  DP << getMsgStr();
  if (getLocCookie())
    DP << " at line " << getLocCookie();
}

void DiagnosticInfoResourceLimit::print(DiagnosticPrinter &DP) const {
  DP << getResourceName() << " limit";

  if (getResourceLimit() != 0)
    DP << " of " << getResourceLimit();

  DP << " exceeded (" <<  getResourceSize() << ") in " << getFunction();
}

void DiagnosticInfoDebugMetadataVersion::print(DiagnosticPrinter &DP) const {
  DP << "ignoring debug info with an invalid version (" << getMetadataVersion()
     << ") in " << getModule();
}

void DiagnosticInfoIgnoringInvalidDebugMetadata::print(
    DiagnosticPrinter &DP) const {
  DP << "ignoring invalid debug info in " << getModule().getModuleIdentifier();
}

void DiagnosticInfoSampleProfile::print(DiagnosticPrinter &DP) const {
  if (!FileName.empty()) {
    DP << getFileName();
    if (LineNum > 0)
      DP << ":" << getLineNum();
    DP << ": ";
  }
  DP << getMsg();
}

void DiagnosticInfoPGOProfile::print(DiagnosticPrinter &DP) const {
  if (getFileName())
    DP << getFileName() << ": ";
  DP << getMsg();
}

void DiagnosticInfo::anchor() {}
void DiagnosticInfoStackSize::anchor() {}
void DiagnosticInfoWithLocationBase::anchor() {}
void DiagnosticInfoIROptimization::anchor() {}

DiagnosticLocation::DiagnosticLocation(const DebugLoc &DL) {
  if (!DL)
    return;
  File = DL->getFile();
  Line = DL->getLine();
  Column = DL->getColumn();
}

DiagnosticLocation::DiagnosticLocation(const DISubprogram *SP) {
  if (!SP)
    return;

  File = SP->getFile();
  Line = SP->getScopeLine();
  Column = 0;
}

StringRef DiagnosticLocation::getRelativePath() const {
  return File->getFilename();
}

std::string DiagnosticLocation::getAbsolutePath() const {
  StringRef Name = File->getFilename();
  if (sys::path::is_absolute(Name))
    return std::string(Name);

  SmallString<128> Path;
  sys::path::append(Path, File->getDirectory(), Name);
  return sys::path::remove_leading_dotslash(Path).str();
}

std::string DiagnosticInfoWithLocationBase::getAbsolutePath() const {
  return Loc.getAbsolutePath();
}

void DiagnosticInfoWithLocationBase::getLocation(StringRef &RelativePath,
                                                 unsigned &Line,
                                                 unsigned &Column) const {
  RelativePath = Loc.getRelativePath();
  Line = Loc.getLine();
  Column = Loc.getColumn();
}

const std::string DiagnosticInfoWithLocationBase::getLocationStr() const {
  StringRef Filename("<unknown>");
  unsigned Line = 0;
  unsigned Column = 0;
  if (isLocationAvailable())
    getLocation(Filename, Line, Column);
  return (Filename + ":" + Twine(Line) + ":" + Twine(Column)).str();
}

DiagnosticInfoOptimizationBase::Argument::Argument(StringRef Key,
                                                   const Value *V)
    : Key(std::string(Key)) {
  if (auto *F = dyn_cast<Function>(V)) {
    if (DISubprogram *SP = F->getSubprogram())
      Loc = SP;
  }
  else if (auto *I = dyn_cast<Instruction>(V))
    Loc = I->getDebugLoc();

  // Only include names that correspond to user variables.  FIXME: We should use
  // debug info if available to get the name of the user variable.
  if (isa<llvm::Argument>(V) || isa<GlobalValue>(V))
    Val = std::string(GlobalValue::dropLLVMManglingEscape(V->getName()));
  else if (isa<Constant>(V)) {
    raw_string_ostream OS(Val);
    V->printAsOperand(OS, /*PrintType=*/false);
  } else if (auto *I = dyn_cast<Instruction>(V))
    Val = I->getOpcodeName();
}

DiagnosticInfoOptimizationBase::Argument::Argument(StringRef Key, const Type *T)
    : Key(std::string(Key)) {
  raw_string_ostream OS(Val);
  OS << *T;
}

DiagnosticInfoOptimizationBase::Argument::Argument(StringRef Key, StringRef S)
    : Key(std::string(Key)), Val(S.str()) {}

DiagnosticInfoOptimizationBase::Argument::Argument(StringRef Key, int N)
    : Key(std::string(Key)), Val(itostr(N)) {}

DiagnosticInfoOptimizationBase::Argument::Argument(StringRef Key, float N)
    : Key(std::string(Key)), Val(llvm::to_string(N)) {}

DiagnosticInfoOptimizationBase::Argument::Argument(StringRef Key, long N)
    : Key(std::string(Key)), Val(itostr(N)) {}

DiagnosticInfoOptimizationBase::Argument::Argument(StringRef Key, long long N)
    : Key(std::string(Key)), Val(itostr(N)) {}

DiagnosticInfoOptimizationBase::Argument::Argument(StringRef Key, unsigned N)
    : Key(std::string(Key)), Val(utostr(N)) {}

DiagnosticInfoOptimizationBase::Argument::Argument(StringRef Key,
                                                   unsigned long N)
    : Key(std::string(Key)), Val(utostr(N)) {}

DiagnosticInfoOptimizationBase::Argument::Argument(StringRef Key,
                                                   unsigned long long N)
    : Key(std::string(Key)), Val(utostr(N)) {}

DiagnosticInfoOptimizationBase::Argument::Argument(StringRef Key, DebugLoc Loc)
    : Key(std::string(Key)), Loc(Loc) {
  if (Loc) {
    Val = (Loc->getFilename() + ":" + Twine(Loc.getLine()) + ":" +
           Twine(Loc.getCol())).str();
  } else {
    Val = "<UNKNOWN LOCATION>";
  }
}

void DiagnosticInfoOptimizationBase::print(DiagnosticPrinter &DP) const {
  DP << getLocationStr() << ": " << getMsg();
  if (Hotness)
    DP << " (hotness: " << *Hotness << ")";
}

OptimizationRemark::OptimizationRemark(const char *PassName,
                                       StringRef RemarkName,
                                       const DiagnosticLocation &Loc,
                                       const Value *CodeRegion)
    : DiagnosticInfoIROptimization(
          DK_OptimizationRemark, DS_Remark, PassName, RemarkName,
          *cast<BasicBlock>(CodeRegion)->getParent(), Loc, CodeRegion) {}

OptimizationRemark::OptimizationRemark(const char *PassName,
                                       StringRef RemarkName,
                                       const Instruction *Inst)
    : DiagnosticInfoIROptimization(DK_OptimizationRemark, DS_Remark, PassName,
                                   RemarkName, *Inst->getParent()->getParent(),
                                   Inst->getDebugLoc(), Inst->getParent()) {}

static const BasicBlock *getFirstFunctionBlock(const Function *Func) {
  return Func->empty() ? nullptr : &Func->front();
}

OptimizationRemark::OptimizationRemark(const char *PassName,
                                       StringRef RemarkName,
                                       const Function *Func)
    : DiagnosticInfoIROptimization(DK_OptimizationRemark, DS_Remark, PassName,
                                   RemarkName, *Func, Func->getSubprogram(),
                                   getFirstFunctionBlock(Func)) {}

bool OptimizationRemark::isEnabled() const {
  const Function &Fn = getFunction();
  LLVMContext &Ctx = Fn.getContext();
  return Ctx.getDiagHandlerPtr()->isPassedOptRemarkEnabled(getPassName());
}

OptimizationRemarkMissed::OptimizationRemarkMissed(
    const char *PassName, StringRef RemarkName, const DiagnosticLocation &Loc,
    const Value *CodeRegion)
    : DiagnosticInfoIROptimization(
          DK_OptimizationRemarkMissed, DS_Remark, PassName, RemarkName,
          *cast<BasicBlock>(CodeRegion)->getParent(), Loc, CodeRegion) {}

OptimizationRemarkMissed::OptimizationRemarkMissed(const char *PassName,
                                                   StringRef RemarkName,
                                                   const Instruction *Inst)
    : DiagnosticInfoIROptimization(DK_OptimizationRemarkMissed, DS_Remark,
                                   PassName, RemarkName,
                                   *Inst->getParent()->getParent(),
                                   Inst->getDebugLoc(), Inst->getParent()) {}

bool OptimizationRemarkMissed::isEnabled() const {
  const Function &Fn = getFunction();
  LLVMContext &Ctx = Fn.getContext();
  return Ctx.getDiagHandlerPtr()->isMissedOptRemarkEnabled(getPassName());
}

OptimizationRemarkAnalysis::OptimizationRemarkAnalysis(
    const char *PassName, StringRef RemarkName, const DiagnosticLocation &Loc,
    const Value *CodeRegion)
    : DiagnosticInfoIROptimization(
          DK_OptimizationRemarkAnalysis, DS_Remark, PassName, RemarkName,
          *cast<BasicBlock>(CodeRegion)->getParent(), Loc, CodeRegion) {}

OptimizationRemarkAnalysis::OptimizationRemarkAnalysis(const char *PassName,
                                                       StringRef RemarkName,
                                                       const Instruction *Inst)
    : DiagnosticInfoIROptimization(DK_OptimizationRemarkAnalysis, DS_Remark,
                                   PassName, RemarkName,
                                   *Inst->getParent()->getParent(),
                                   Inst->getDebugLoc(), Inst->getParent()) {}

OptimizationRemarkAnalysis::OptimizationRemarkAnalysis(
    enum DiagnosticKind Kind, const char *PassName, StringRef RemarkName,
    const DiagnosticLocation &Loc, const Value *CodeRegion)
    : DiagnosticInfoIROptimization(Kind, DS_Remark, PassName, RemarkName,
                                   *cast<BasicBlock>(CodeRegion)->getParent(),
                                   Loc, CodeRegion) {}

bool OptimizationRemarkAnalysis::isEnabled() const {
  const Function &Fn = getFunction();
  LLVMContext &Ctx = Fn.getContext();
  return Ctx.getDiagHandlerPtr()->isAnalysisRemarkEnabled(getPassName()) ||
         shouldAlwaysPrint();
}

void DiagnosticInfoMIRParser::print(DiagnosticPrinter &DP) const {
  DP << Diagnostic;
}

DiagnosticInfoOptimizationFailure::DiagnosticInfoOptimizationFailure(
    const char *PassName, StringRef RemarkName, const DiagnosticLocation &Loc,
    const Value *CodeRegion)
    : DiagnosticInfoIROptimization(
          DK_OptimizationFailure, DS_Warning, PassName, RemarkName,
          *cast<BasicBlock>(CodeRegion)->getParent(), Loc, CodeRegion) {}

bool DiagnosticInfoOptimizationFailure::isEnabled() const {
  // Only print warnings.
  return getSeverity() == DS_Warning;
}

void DiagnosticInfoUnsupported::print(DiagnosticPrinter &DP) const {
  std::string Str;
  raw_string_ostream OS(Str);

  OS << getLocationStr() << ": in function " << getFunction().getName() << ' '
     << *getFunction().getFunctionType() << ": " << Msg << '\n';
  OS.flush();
  DP << Str;
}

void DiagnosticInfoISelFallback::print(DiagnosticPrinter &DP) const {
  DP << "Instruction selection used fallback path for " << getFunction();
}

void DiagnosticInfoOptimizationBase::insert(StringRef S) {
  Args.emplace_back(S);
}

void DiagnosticInfoOptimizationBase::insert(Argument A) {
  Args.push_back(std::move(A));
}

void DiagnosticInfoOptimizationBase::insert(setIsVerbose V) {
  IsVerbose = true;
}

void DiagnosticInfoOptimizationBase::insert(setExtraArgs EA) {
  FirstExtraArgIndex = Args.size();
}

std::string DiagnosticInfoOptimizationBase::getMsg() const {
  std::string Str;
  raw_string_ostream OS(Str);
  for (const DiagnosticInfoOptimizationBase::Argument &Arg :
       make_range(Args.begin(), FirstExtraArgIndex == -1
                                    ? Args.end()
                                    : Args.begin() + FirstExtraArgIndex))
    OS << Arg.Val;
  return OS.str();
}

DiagnosticInfoMisExpect::DiagnosticInfoMisExpect(const Instruction *Inst,
                                                 Twine &Msg)
    : DiagnosticInfoWithLocationBase(DK_MisExpect, DS_Warning,
                                     *Inst->getParent()->getParent(),
                                     Inst->getDebugLoc()),
      Msg(Msg) {}

void DiagnosticInfoMisExpect::print(DiagnosticPrinter &DP) const {
  DP << getLocationStr() << ": " << getMsg();
}

void OptimizationRemarkAnalysisFPCommute::anchor() {}
void OptimizationRemarkAnalysisAliasing::anchor() {}

DiagnosticInfoBareboneCC::DiagnosticInfoBareboneCC(
  enum DiagnosticKind Kind,
  enum DiagnosticSeverity Severity,
  const Function &Fn,
  const Instruction *Instr
) : DiagnosticInfoWithLocationBase(Kind, Severity, Fn, Instr
                                   ? DiagnosticLocation(Instr->getDebugLoc())
                                   : DiagnosticLocation(Fn.getSubprogram())) {
}

DiagnosticInfoBareboneCC DiagnosticInfoBareboneCC::hwRegInvalid(
  enum DiagnosticSeverity Severity,
  const Function &Fn,
  const CallBase *CallInstr,
  StringRef RawValue
) {
  DiagnosticInfoBareboneCC D(DK_BareboneCCHWRegInvalid, Severity, Fn, CallInstr);
  D.CallInstr = CallInstr;
  D.RawValue = RawValue;
  return D;
}

DiagnosticInfoBareboneCC DiagnosticInfoBareboneCC::hwRegAllocFailure(
  enum DiagnosticSeverity Severity,
  const Function &Fn,
  const CallBase *CallInstr,
  StringRef RawValue
) {
  DiagnosticInfoBareboneCC D(DK_BareboneCCHWRegAllocFailure,
                           Severity, Fn, CallInstr);
  D.CallInstr = CallInstr;
  D.RawValue = RawValue;
  return D;
}

DiagnosticInfoBareboneCC DiagnosticInfoBareboneCC::multipartArgUnsupported(
  enum DiagnosticSeverity Severity,
  const Function &Fn,
  const CallBase *CallInstr,
  Type *T
) {
  DiagnosticInfoBareboneCC D(DK_BareboneCCMultipartArgUnsupported,
                           Severity, Fn, CallInstr);
  D.CallInstr = CallInstr;
  D.T = T;
  return D;
}

DiagnosticInfoBareboneCC DiagnosticInfoBareboneCC::noClobberHWRegInvalid(
  enum DiagnosticSeverity Severity,
  const Function &Fn,
  StringRef RawValue
) {
  DiagnosticInfoBareboneCC D(DK_BareboneCCNoClobberHWRegInvalid,
                           Severity, Fn, nullptr);
  D.RawValue = RawValue;
  return D;
}

DiagnosticInfoBareboneCC DiagnosticInfoBareboneCC::framePointerNotAllowed(
  enum DiagnosticSeverity Severity,
  const Function &Fn
) {
  return DiagnosticInfoBareboneCC(DK_BareboneCCFramePointerNotAllowed,
                                Severity, Fn, nullptr);
}

DiagnosticInfoBareboneCC DiagnosticInfoBareboneCC::localAreaSizeInvalid(
  enum DiagnosticSeverity Severity,
  const Function &Fn,
  StringRef RawValue,
  Align Align
) {
  DiagnosticInfoBareboneCC D(DK_BareboneCCLocalAreaSizeInvalid,
                           Severity, Fn, nullptr);
  D.RawValue = RawValue;
  D.A = Align;
  return D;
}

DiagnosticInfoBareboneCC DiagnosticInfoBareboneCC::localAreaSizeAlignNote(
  enum DiagnosticSeverity Severity,
  const Function &Fn,
  Align Align
) {
  DiagnosticInfoBareboneCC D(DK_BareboneCCLocalAreaSizeAlignNote,
                           Severity, Fn, nullptr);
  D.A = Align;
  return D;
}

DiagnosticInfoBareboneCC DiagnosticInfoBareboneCC::localAreaSizeExceeded(
  enum DiagnosticSeverity Severity,
  const Function &Fn,
  int64_t LocalAreaSize,
  int64_t BytesUsed
) {
  DiagnosticInfoBareboneCC D(DK_BareboneCCLocalAreaSizeExceeded,
                           Severity, Fn, nullptr);
  D.LocalAreaSize = LocalAreaSize;
  D.BytesUsed = BytesUsed;
  return D;
}

DiagnosticInfoBareboneCC DiagnosticInfoBareboneCC::returnNotAllowed(
  enum DiagnosticSeverity Severity,
  const Function &Fn,
  const Instruction *ReturnInstr
) {
  return DiagnosticInfoBareboneCC(DK_BareboneCCReturnNotAllowed,
                                Severity, Fn, ReturnInstr);
}

DiagnosticInfoBareboneCC DiagnosticInfoBareboneCC::mustTailCall(
  enum DiagnosticSeverity Severity,
  const Function &Fn,
  const CallBase *CallInstr
) {
  DiagnosticInfoBareboneCC D(DK_BareboneCCMustTailCall,
                           Severity, Fn, CallInstr);
  D.CallInstr = CallInstr;
  return D;
}

DiagnosticInfoBareboneCC DiagnosticInfoBareboneCC::notInTailCallPosition(
  enum DiagnosticSeverity Severity,
  const Function &Fn,
  const CallBase *CallInstr
) {
  DiagnosticInfoBareboneCC D(DK_BareboneCCNotInTailCallPosition,
                           Severity, Fn, CallInstr);
  D.CallInstr = CallInstr;
  return D;
}

DiagnosticInfoBareboneCC DiagnosticInfoBareboneCC::inNonBareboneFunction(
  enum DiagnosticSeverity Severity,
  const Function &Fn,
  const CallBase *CallInstr
) {
  DiagnosticInfoBareboneCC D(DK_BareboneCCInNonBareboneFunction,
                           Severity, Fn, CallInstr);
  D.CallInstr = CallInstr;
  return D;
}

static void PrintCallee(DiagnosticPrinter &DP, const CallBase *Instr) {
  if (!Instr) return;
  auto *F = Instr->getCalledFunction();
  if (F) {
    DP << F->getName();
  } else {
    std::string Str;
    raw_string_ostream OS(Str);
    OS << *Instr->getFunctionType();
    OS.flush();
    DP << Str;
  }
}

void DiagnosticInfoBareboneCC::print(DiagnosticPrinter &DP) const {
  if (isLocationAvailable())
    DP << getLocationStr() << ": ";
  DP << "in function " << getFunction().getName() << ": ";
  switch (getKind()) {
  case DK_BareboneCCHWRegInvalid:
    DP << "register requested by 'hwreg' attribute is unknown "
          "or invalid";
    if (CallInstr) {
      DP << " in a call to ";
      PrintCallee(DP, CallInstr);
    }
    DP << ": " << RawValue;
    break;
  case DK_BareboneCCHWRegAllocFailure:
    DP << "failed to allocate register requested by 'hwreg' attribute";
    if (CallInstr) {
      DP << " in a call to ";
      PrintCallee(DP, CallInstr);
    }
    DP << ": " << RawValue;
    break;
  case DK_BareboneCCMultipartArgUnsupported:
    {
      std::string T;
      raw_string_ostream OS(T);
      OS << *getType();
      OS.flush();
      DP << "argument of type " << T
         << " is passed in multiple registers, incompatible with 'hwreg'";
    }
    if (CallInstr) {
      DP << " in a call to ";
      PrintCallee(DP, CallInstr);
    }
    break;
  case DK_BareboneCCNoClobberHWRegInvalid:
    DP << "unknown register in 'no-clobber-hwreg' attribute: " << RawValue;
    break;
  case DK_BareboneCCFramePointerNotAllowed:
    DP << "frame pointer not allowed";
    break;
  case DK_BareboneCCLocalAreaSizeInvalid:
    DP << "bad value in 'local-area-size' attribute: " << RawValue;
    break;
  case DK_BareboneCCLocalAreaSizeAlignNote:
    DP << "the value in 'local-area-size' attribute must be a multiple of "
       << A.value();
    break;
  case DK_BareboneCCLocalAreaSizeExceeded:
    DP << "stack size limit of " << LocalAreaSize << " exceeded: "
       << BytesUsed << " used";
    break;
  case DK_BareboneCCReturnNotAllowed:
    DP << "must terminate by tail-calling another barebonecc function";
    break;
  case DK_BareboneCCMustTailCall:
    DP << "function ";
    PrintCallee(DP, CallInstr);
    DP << " must be tail-called, use musttail marker";
    break;
  case DK_BareboneCCNotInTailCallPosition:
    DP << "a call to function ";
    PrintCallee(DP, CallInstr);
    DP << " must be in tail-call position";
    break;
  case DK_BareboneCCInNonBareboneFunction:
    DP << "a call to function ";
    PrintCallee(DP, CallInstr);
    DP << " is only allowed in barebonecc functions";
    break;
  default:
    llvm_unreachable("unexpected diagnostic kind");
    break;
  }
}
