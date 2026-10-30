#include "bytecode.h"

// Implementacion de las operaciones basicas del bytecode (FASE 1).

void bytecode_init(Bytecode& bc) {
  bc.code.clear();
  bc.numbers.clear();
  bc.strings.clear();
  bc.num_registers = 0;
  bc.num_locals = 0;
}

int bytecode_emit(Bytecode& bc, const Instruction& instruction) {
  bc.code.push_back(instruction);
  return static_cast<int>(bc.code.size() - 1);
}

void bytecode_free(Bytecode& bc) { bytecode_init(bc); }
