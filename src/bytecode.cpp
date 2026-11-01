#include "bytecode.h"

// Implementation of the basic bytecode operations (PHASE 1).

void bytecodeInit(Bytecode& bc) {
  bc.code.clear();
  bc.constants.clear();
  bc.numRegisters = 0;
  bc.numLocals = 0;
}

int bytecodeEmit(Bytecode& bc, const Instruction& instruction) {
  bc.code.push_back(instruction);
  return static_cast<int>(bc.code.size() - 1);
}

void bytecodeFree(Bytecode& bc) { bytecodeInit(bc); }
