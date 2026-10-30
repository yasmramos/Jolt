#pragma once

#include <string>
#include <vector>

struct Frame {
  std::string function;
  int line = 0;
  int col = 0;
};

// Raised by the lexer and the parser.
struct CompileError {
  std::string message;
  int line = 1;
  int col = 1;
};

// Raised while a program is running.
struct RuntimeError {
  std::string message;
  int line = 0;
  int col = 0;
  std::vector<Frame> stack;
};
