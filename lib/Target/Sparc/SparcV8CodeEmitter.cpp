//===-- SparcV8CodeEmitter.cpp - JIT Code Emitter for SparcV8 -----*- C++ -*-=//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//
//===----------------------------------------------------------------------===//

#include "SparcV8.h"
#include "SparcV8TargetMachine.h"
#include "llvm/Module.h"
#include "llvm/CodeGen/MachineCodeEmitter.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/Support/Debug.h"
#include <cstdlib>
#include <map>
#include <vector>
using namespace llvm;

namespace {
  class SparcV8CodeEmitter : public MachineFunctionPass {
    TargetMachine &TM;
    MachineCodeEmitter &MCE;

    /// getMachineOpValue - evaluates the MachineOperand of a given MachineInstr
    ///
    int64_t getMachineOpValue(MachineInstr &MI, MachineOperand &MO);

    // Tracks which instruction references which BasicBlock
    std::vector<std::pair<const BasicBlock*,
                          std::pair<unsigned*,MachineInstr*> > > BBRefs;
    // Tracks where each BasicBlock starts
    std::map<const BasicBlock*, long> BBLocations;

  public:
    SparcV8CodeEmitter(TargetMachine &T, MachineCodeEmitter &M)
      : TM(T), MCE(M) {}

    const char *getPassName() const { return "SparcV8 Machine Code Emitter"; }

    /// runOnMachineFunction - emits the given MachineFunction to memory
    ///
    bool runOnMachineFunction(MachineFunction &MF);

    /// emitBasicBlock - emits the given MachineBasicBlock to memory
    ///
    void emitBasicBlock(MachineBasicBlock &MBB);

    /// emitWord - write a 32-bit word to memory at the current PC
    ///
    void emitWord(unsigned w) { MCE.emitWord(w); }

    /// getValueBit - return the particular bit of Val
    ///
    unsigned getValueBit(int64_t Val, unsigned bit) { return (Val >> bit) & 1; }

    /// getBinaryCodeForInstr - This function, generated by the
    /// CodeEmitterGenerator using TableGen, produces the binary encoding for
    /// machine instructions.
    ///
    unsigned getBinaryCodeForInstr(MachineInstr &MI);
  };
}

/// addPassesToEmitMachineCode - Add passes to the specified pass manager to get
/// machine code emitted.  This uses a MachineCodeEmitter object to handle
/// actually outputting the machine code and resolving things like the address
/// of functions.  This method should returns true if machine code emission is
/// not supported.
///
bool SparcV8TargetMachine::addPassesToEmitMachineCode(FunctionPassManager &PM,
                                                      MachineCodeEmitter &MCE) {
  // Keep as `true' until this is a functional JIT to allow llvm-gcc to build
  return true;

  // Machine code emitter pass for SparcV8
  PM.add(new SparcV8CodeEmitter(*this, MCE));
  // Delete machine code for this function after emitting it
  PM.add(createMachineCodeDeleter());
  return false;
}

bool SparcV8CodeEmitter::runOnMachineFunction(MachineFunction &MF) {
  MCE.startFunction(MF);
  MCE.emitConstantPool(MF.getConstantPool());
  for (MachineFunction::iterator I = MF.begin(), E = MF.end(); I != E; ++I)
    emitBasicBlock(*I);
  MCE.finishFunction(MF);

  // Resolve branches to BasicBlocks for the entire function
  for (unsigned i = 0, e = BBRefs.size(); i != e; ++i) {
    long Location = BBLocations[BBRefs[i].first];
    unsigned *Ref = BBRefs[i].second.first;
    MachineInstr *MI = BBRefs[i].second.second;
    DEBUG(std::cerr << "Fixup @ " << std::hex << Ref << " to 0x" << Location
                    << " in instr: " << std::dec << *MI);
    for (unsigned ii = 0, ee = MI->getNumOperands(); ii != ee; ++ii) {
      MachineOperand &op = MI->getOperand(ii);
      if (op.isPCRelativeDisp()) {
        // the instruction's branch target is made such that it branches to
        // PC + (branchTarget * 4), so undo that arithmetic here:
        // Location is the target of the branch
        // Ref is the location of the instruction, and hence the PC
        int64_t branchTarget = (Location - (long)Ref) >> 2;
        MI->SetMachineOperandConst(ii, MachineOperand::MO_SignExtendedImmed,
                                   branchTarget);
        unsigned fixedInstr = SparcV8CodeEmitter::getBinaryCodeForInstr(*MI);
        MCE.emitWordAt(fixedInstr, Ref);
        break;
      }
    }
  }
  BBRefs.clear();
  BBLocations.clear();

  return false;
}

void SparcV8CodeEmitter::emitBasicBlock(MachineBasicBlock &MBB) {
  for (MachineBasicBlock::iterator I = MBB.begin(), E = MBB.end(); I != E; ++I)
    emitWord(getBinaryCodeForInstr(*I));
}

int64_t SparcV8CodeEmitter::getMachineOpValue(MachineInstr &MI,
                                            MachineOperand &MO) {
  int64_t rv = 0; // Return value; defaults to 0 for unhandled cases
                  // or things that get fixed up later by the JIT.
  if (MO.isPCRelativeDisp()) {
    std::cerr << "SparcV8CodeEmitter: PC-relative disp unhandled\n";
    abort();
  } else if (MO.isRegister()) {
    rv = MO.getReg();
  } else if (MO.isImmediate()) {
    rv = MO.getImmedValue();
  } else if (MO.isGlobalAddress()) {
    GlobalValue *GV = MO.getGlobal();
    std::cerr << "Unhandled global value: " << GV << "\n";
    abort();
  } else if (MO.isMachineBasicBlock()) {
    const BasicBlock *BB = MO.getMachineBasicBlock()->getBasicBlock();
    unsigned* CurrPC = (unsigned*)(intptr_t)MCE.getCurrentPCValue();
    BBRefs.push_back(std::make_pair(BB, std::make_pair(CurrPC, &MI)));
  } else if (MO.isExternalSymbol()) {
  } else if (MO.isConstantPoolIndex()) {
    unsigned index = MO.getConstantPoolIndex();
    rv = MCE.getConstantPoolEntryAddress(index);
  } else if (MO.isFrameIndex()) {
    std::cerr << "SparcV8CodeEmitter: error: Frame index unhandled!\n";
    abort();
  } else {
    std::cerr << "ERROR: Unknown type of MachineOperand: " << MO << "\n";
    abort();
  }

  // Adjust for special meaning of operands in some instructions
  unsigned Opcode = MI.getOpcode();
  if (Opcode == V8::SETHIi && !MO.isRegister() && !MO.isImmediate()) {
    rv &= 0x03ff;
  } else if (Opcode == V8::ORri &&!MO.isRegister() &&!MO.isImmediate()) {
    rv = (rv >> 10) & 0x03fffff;
  }

  return rv;
}

void *SparcV8JITInfo::getJITStubForFunction(Function *F,
                                            MachineCodeEmitter &MCE) {
  std::cerr << "SparcV8JITInfo::getJITStubForFunction not implemented!\n";
  abort();
  return 0;
}

void SparcV8JITInfo::replaceMachineCodeForFunction(void *Old, void *New) {
  std::cerr << "SparcV8JITInfo::replaceMachineCodeForFunction not implemented!";
  abort();
}

#include "SparcV8GenCodeEmitter.inc"
