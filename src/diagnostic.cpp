#include "diagnostic.h"

#include <algorithm>
#include <algorithm>
#include <string>

#include "platform.h"
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
                  const std::string& kind, const std::string& message, Platform& platform) {
  std::string text = lineText(source, line);
  size_t gutter = std::to_string(std::max(line, 1)).size();
  size_t byteColumn = col > 0 ? static_cast<size_t>(col - 1) : 0;
  size_t caret = displayColumns(text, std::min(byteColumn, text.size()));

  std::string out;
  out += sourceName + ':' + std::to_string(line) + ':' + std::to_string(col) + ": " + kind + ": " + message + '\n';
  out += std::string(gutter + 2, ' ') + "|\n";
  out += std::to_string(line) + ' ' + '|' + ' ' + text + '\n';
  out += std::string(gutter + 2, ' ') + '|' + ' ' + std::string(caret, ' ') + "^\n";
  platform.write(Platform::kStdout, out.data(), out.size());
  platform.flush(Platform::kStdout);
}

}  // namespace

void printCompileError(const std::string& sourceName, const std::string& source,
                       const CompileError& error, Platform& platform) {
  printSnippet(sourceName, source, error.line, error.col, "error", error.message, platform);
}

void printRuntimeError(const std::string& sourceName, const std::string& source,
                       const RuntimeError& error, Platform& platform) {
  printSnippet(sourceName, source, error.line, error.col, "runtime error", error.message, platform);

  if (!error.stack.empty()) {
    std::string out = "stack trace:\n";
    for (auto it = error.stack.rbegin(); it != error.stack.rend(); ++it) {
      out += "  at " + it->function + " (" + sourceName + ':' + std::to_string(it->line) + ':'
             + std::to_string(it->col) + ")\n";
    }
    platform.write(Platform::kStdout, out.data(), out.size());
    platform.flush(Platform::kStdout);
  }
}
