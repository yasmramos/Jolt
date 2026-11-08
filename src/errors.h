#pragma once

#include <exception>
#include <string>
#include <vector>

struct Frame {
  std::string function;
  int line = 0;
  int col = 0;
};

// Raised by the lexer and the parser.
struct CompileError : std::exception {
  CompileError() = default;
  CompileError(std::string errorMessage, int errorLine, int errorCol)
      : message(std::move(errorMessage)), line(errorLine), col(errorCol) {}

  std::string message;
  int line = 1;
  int col = 1;

  const char* what() const noexcept override { return message.c_str(); }
};

// A non-fatal diagnostic raised while compiling (PHASE 6.7.5). Warnings never
// stop the compilation: they are collected so the CLI can print them and the
// program still runs. A hard error would violate the §4.6 principle that
// inference never rejects a program that would have run.
struct Warning {
  std::string message;
  int line = 1;
  int col = 1;
};

// Raised while a program is running.
struct RuntimeError : std::exception {
  RuntimeError() = default;
  explicit RuntimeError(std::string errorMessage) : message(std::move(errorMessage)) {}

  std::string message;
  int line = 1;
  int col = 1;
  std::vector<Frame> stack;

  const char* what() const noexcept override { return message.c_str(); }
};
