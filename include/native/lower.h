#pragma once

#include <cstdint>
#include <vector>

#include "bytecode.h"
#include "native/codegen.h"
#include "native/target.h"

// Lowers a Bytecode proto to x86-64 machine code.
//
// PHASE 7b: only the opcodes that need no runtime are lowered inline
// (LOADK, MOVE, ADD-SUB-MUL int fast path, UNOP, BAND-SHR, EQ-GTE int fast
// path, JMP, JMP_IF_FALSE, JMP_IF_TRUE, HALT).
//
// Opcodes that need runtime support (DIV, MOD, POW, GETGLOBAL, SETGLOBAL,
// DECLLOCAL, GETLOCAL, SETLOCAL, CALL, RETURN, CLOSURE, GETUPVAL, SETUPVAL,
// NEWTABLE, GETTABLE, SETTABLE, IN, ITER, PRINT) throw a LoweringError.
//
// The first 12 registers (R0-R11) are mapped to x86-64 registers:
//   R0=rax, R1=rcx, R2=rdx, R3=rsi, R4=rdi, R5=r8,
//   R6=r9, R7=r10, R8=r11, R9=r12, R10=r13, R11=r14.
//
// Protos that use more than 12 registers throw a LoweringError.
struct LoweringError : std::exception {
  explicit LoweringError(std::string message) : message(std::move(message)) {}
  std::string message;
  const char* what() const noexcept override { return message.c_str(); }
};

// Lowers `proto` to x86-64 machine code. The result is a flat byte vector
// that can be placed in a .text section.
//
// The lowering assumes the System V AMD64 ABI (Linux x86-64). Windows x86-64
// support is deferred to PHASE 7e.
std::vector<std::uint8_t> lowerToX64(const Target& target,
                                      const Bytecode& proto,
                                      const CodeGen& codegen);