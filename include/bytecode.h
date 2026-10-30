#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "value.h"

// Bytecode de bajo nivel para la VM register-based de Jolt.
// Cada instruccion opera sobre registros y se disena para mapear a una
// instruccion nativa en la futura FASE 7.

enum class OpCode : std::uint8_t {
  LOADK,      // R[a] = numbers[b]  (constante numerica: int o float)
  LOADKS,     // R[a] = strings[b]  (constante de cadena)
  LOADNIL,    // R[a] = nil
  LOADBOOL,   // R[a] = (b != 0)
  MOVE,       // R[a] = R[b]

  ADD,        // R[a] = R[b] + R[c]
  SUB,        // R[a] = R[b] - R[c]
  MUL,        // R[a] = R[b] * R[c]
  DIV,        // R[a] = R[b] / R[c]
  MOD,        // R[a] = R[b] % R[c]
  POW,        // R[a] = R[b] ** R[c]
  NEG,        // R[a] = -R[b]
  NOT,        // R[a] = !R[b]

  BAND,       // R[a] = R[b] & R[c]
  BOR,        // R[a] = R[b] | R[c]
  BXOR,       // R[a] = R[b] ^ R[c]
  SHL,        // R[a] = R[b] << R[c]
  SHR,        // R[a] = R[b] >> R[c]
  BNOT,       // R[a] = ~R[b]

  EQ,         // R[a] = R[b] == R[c]
  NEQ,        // R[a] = R[b] != R[c]
  LT,         // R[a] = R[b] <  R[c]
  LTE,        // R[a] = R[b] <= R[c]
  GT,         // R[a] = R[b] >  R[c]
  GTE,        // R[a] = R[b] >= R[c]

  // Control de flujo. `jump` es un indice absoluto dentro de `code`.
  JMP,          // pc = jump
  JMP_IF_FALSE, // si !R[a] entonces pc = jump
  JMP_IF_TRUE,  // si R[a] entonces pc = jump

  // Variables. Los globales se resuelven por nombre (pool de cadenas); los
  // locales por indice de celda en el frame actual.
  SETGLOBAL,  // globales[strings[b]] = R[a]
  GETGLOBAL,  // R[a] = globales[strings[b]]
  DECLLOCAL,  // locals[b] = nueva celda con R[a]
  SETLOCAL,   // locals[b]->value = R[a]
  GETLOCAL,   // R[a] = locals[b]->value

  PRINT,      // imprime R[a]
  HALT,       // termina la ejecucion
};

struct Instruction {
  OpCode op = OpCode::HALT;
  int a = 0;
  int b = 0;
  int c = 0;
  int jump = 0;
};

struct Bytecode {
  std::vector<Instruction> code;
  // Pool de constantes numericas. Usa `Value` para conservar la diferencia
  // entre enteros y flotantes (un `double` convertiria todo a float).
  std::vector<Value> numbers;
  std::vector<std::string> strings;
  int num_registers = 0;
  // Numero de celdas locales que necesita el frame.
  int num_locals = 0;
};

// Reinicia el bytecode conservando la capacidad de sus contenedores.
void bytecode_init(Bytecode& bc);

// Anade una instruccion y devuelve su indice.
int bytecode_emit(Bytecode& bc, const Instruction& instruction);

// Libera el contenido del bytecode. En C++ los contenedores gestionan su
// memoria, pero se mantiene la funcion para simetria con el diseno original.
void bytecode_free(Bytecode& bc);
