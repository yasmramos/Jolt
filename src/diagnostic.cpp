#include "diagnostic.h"

#include <algorithm>
#include <iostream>
#include <string>

#include "value.h"

namespace {

std::string lineText(const std::string& source, int line) {
  size_t pos = 0;
  int current = 1;
  while (current < line) {
    size_t newline = source.find('\n', pos);
    if (newline == std::string::npos) {
      pos = source.size();
      break;
    }
    pos = newline + 1;
    ++current;
  }
  size_t end = source.find('\n', pos);
  if (end == std::string::npos) end = source.size();
  std::string text = source.substr(pos, end - pos);
  if (!text.empty() && text.back() == '\r') text.pop_back();
  return text;
}

size_t displayColumns(const std::string& text, size_t byteCount) {
  size_t columns = 0;
  for (size_t i = 0; i < text.size() && i < byteCount; ++i) {
    if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) ++columns;
  }
  return columns;
}

void printSnippet(const std::string& sourceName, const std::string& source, int line, int col,
                  const std::string& kind, const std::string& message) {
  std::string text = lineText(source, line);
  size_t gutter = std::to_string(std::max(line, 1)).size();
  size_t byteColumn = col > 0 ? static_cast<size_t>(col - 1) : 0;
  size_t caret = displayColumns(text, std::min(byteColumn, text.size()));

  std::cout << sourceName << ':' << line << ':' << col << ": " << kind << ": " << message << '\n';
  std::cout << std::string(gutter + 2, ' ') << "|\n";
  std::cout << line << ' ' << '|' << ' ' << text << '\n';
  std::cout << std::string(gutter + 2, ' ') << '|' << ' ' << std::string(caret, ' ') << "^\n";
}

}  // namespace

void printCompileError(const std::string& sourceName, const std::string& source,
                       const CompileError& error) {
  printSnippet(sourceName, source, error.line, error.col, "error", error.message);
}

void printRuntimeError(const std::string& sourceName, const std::string& source,
                       const RuntimeError& error) {
  printSnippet(sourceName, source, error.line, error.col, "runtime error", error.message);

  if (!error.stack.empty()) {
    std::cout << "stack trace:\n";
    for (auto it = error.stack.rbegin(); it != error.stack.rend(); ++it) {
      std::cout << "  at " << it->function << " (" << sourceName << ':' << it->line << ':'
                << it->col << ")\n";
    }
  }
}
