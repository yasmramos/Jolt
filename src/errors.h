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
  std::string message;
  int line = 1;
  int col = 1;

  const char* what() const noexcept override { return message.c_str(); }
};

// Raised while a program is running.
struct RuntimeError : std::exception {
  std::string message;
  int line = 0;
  int col = 0;
  std::vector<Frame> stack;

  const char* what() const noexcept override { return message.c_str(); }
};
