#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include "diagnostic.h"
#include "interpreter.h"
#include "lexer.h"
#include "parser.h"
#include "util.h"

namespace {

const char* kVersion = "0.1.0";

const char* kUsage =
    "usage:\n"
    "  jolt                      start the interactive REPL\n"
    "  jolt <file.jolt> [args]   run a script\n"
    "  jolt -e \"<code>\" [args]   run inline code\n"
    "  jolt -c <file.jolt>       parse a script without running it\n"
    "\n"
    "options:\n"
    "  -h, --help                show this help\n"
    "  -v, --version             show the version\n"
    "  -e, --eval <code>         evaluate code from the command line\n"
    "  -c, --check               only check the script for errors\n";

// Returns 0 on success, 65 for compile errors, 70 for runtime errors.
int execute(Interpreter& interp, const std::string& source, const std::string& name) {
  std::vector<Token> tokens;
  try {
    tokens = lex(source);
  } catch (const CompileError& error) {
    printCompileError(name, source, error);
    return 65;
  }

  StmtPtr program;
  try {
    Parser parser(std::move(tokens), name);
    program = parser.parseProgram();
  } catch (const CompileError& error) {
    printCompileError(name, source, error);
    return 65;
  }

  try {
    interp.run(program);
  } catch (const RuntimeError& error) {
    printRuntimeError(name, source, error);
    return 70;
  }
  return 0;
}

int runSource(const std::string& source, const std::string& name,
              const std::vector<std::string>& scriptArgs, bool checkOnly) {
  if (checkOnly) {
    try {
      Parser parser(lex(source), name);
      parser.parseProgram();
    } catch (const CompileError& error) {
      printCompileError(name, source, error);
      return 65;
    }
    return 0;
  }

  Interpreter interp(name);
  interp.scriptArgs = scriptArgs;
  return execute(interp, source, name);
}

bool isStatementStart(const std::string& text) {
  size_t end = 0;
  while (end < text.size() && (std::isalnum(static_cast<unsigned char>(text[end])) || text[end] == '_')) {
    ++end;
  }
  std::string word = text.substr(0, end);
  return word == "let" || word == "var" || word == "fn" || word == "if" || word == "while" ||
         word == "for" || word == "return" || word == "break" || word == "continue" ||
         word == "else";
}

void printReplHelp() {
  std::cout << "commands:\n"
               "  :help            show this help\n"
               "  :quit            exit the REPL\n"
               "  :clear           forget all user definitions\n"
               "  :load <file>     run a script in this session\n"
               "  :vars            list global variables\n";
}

// Returns true when the REPL should exit.
bool handleCommand(Interpreter& interp, const std::string& command,
                   const std::vector<std::string>& scriptArgs) {
  if (command == ":quit" || command == ":q" || command == ":exit") return true;

  if (command == ":help" || command == ":?") {
    printReplHelp();
    return false;
  }

  if (command == ":clear") {
    interp = Interpreter("<repl>");
    interp.scriptArgs = scriptArgs;
    std::cout << "all definitions cleared\n";
    return false;
  }

  if (command == ":vars") {
    std::vector<std::pair<std::string, std::string>> entries;
    for (const auto& entry : interp.globals->vars) {
      const Value& value = entry.second.value;
      if (value.isFunc() && value.func().isNative()) continue;
      entries.emplace_back(entry.first, value.typeName());
    }
    std::sort(entries.begin(), entries.end());
    if (entries.empty()) {
      std::cout << "(no user definitions)\n";
    }
    for (const auto& entry : entries) {
      std::cout << "  " << entry.first << ": " << entry.second << '\n';
    }
    return false;
  }

  if (startsWith(command, ":load")) {
    std::string path = trim(command.substr(5));
    if (path.empty()) {
      std::cout << "usage: :load <file.jolt>\n";
      return false;
    }
    std::string source;
    if (!readSourceFile(path, source)) {
      std::cout << "cannot read file '" << path << "'\n";
      return false;
    }
    execute(interp, source, path);
    return false;
  }

  std::cout << "unknown command '" << command << "'; try :help\n";
  return false;
}

void repl(const std::vector<std::string>& scriptArgs) {
  Interpreter interp("<repl>");
  interp.scriptArgs = scriptArgs;

  std::cout << "Jolt " << kVersion << " - a small scripting language written in C++\n";
  std::cout << "Type :help for help, :quit to exit.\n";

  std::string buffer;
  while (true) {
    std::cout << (buffer.empty() ? "jolt> " : "...> ") << std::flush;

    std::string line;
    if (!std::getline(std::cin, line)) {
      std::cout << '\n';
      break;
    }

    std::string trimmed = trim(line);
    if (!buffer.empty() && trimmed.empty()) {
      buffer.clear();
      continue;
    }
    if (buffer.empty() && !trimmed.empty() && trimmed[0] == ':') {
      if (handleCommand(interp, trimmed, scriptArgs)) break;
      continue;
    }
    if (buffer.empty() && trimmed.empty()) continue;

    buffer += line;
    buffer += '\n';

    std::vector<Token> tokens;
    try {
      tokens = lex(buffer);
    } catch (const CompileError& error) {
      if (error.message.find("unterminated") != std::string::npos) continue;
      printCompileError("<repl>", buffer, error);
      buffer.clear();
      continue;
    }

    // A new REPL entry that begins with a statement keyword is parsed as a
    // program; everything else is first tried as a bare expression.
    size_t firstNewline = buffer.find('\n');
    std::string firstLineText =
        firstNewline == std::string::npos ? buffer : buffer.substr(0, firstNewline);

    if (!isStatementStart(trim(firstLineText))) {
      try {
        Parser expressionParser(tokens, "<repl>");
        auto expr = expressionParser.parseFullExpression();
        Value value = interp.evalRepl(expr);
        interp.defineGlobal("_", value, false);
        if (!value.isNil()) std::cout << value.repr() << '\n';
        buffer.clear();
        continue;
      } catch (const CompileError&) {
        // Fall through and try to parse it as a program.
      } catch (const RuntimeError& error) {
        printRuntimeError("<repl>", buffer, error);
        buffer.clear();
        continue;
      }
    }

    StmtPtr program;
    try {
      Parser parser(tokens, "<repl>");
      program = parser.parseProgram();
    } catch (const CompileError& error) {
      if (error.message.find("end of input") != std::string::npos) {
        // Convenience: accept a missing ';' at the end of a REPL entry.
        try {
          Parser parser(lex(buffer + ";"), "<repl>");
          program = parser.parseProgram();
        } catch (const CompileError& retry) {
          bool incomplete = retry.message.find("end of input") != std::string::npos ||
                            retry.message.find("unterminated") != std::string::npos;
          if (incomplete) continue;
          printCompileError("<repl>", buffer, retry);
          buffer.clear();
          continue;
        }
      } else {
        printCompileError("<repl>", buffer, error);
        buffer.clear();
        continue;
      }
    }

    try {
      const auto& block = std::get<BlockStmt>(program->node);
      if (block.stmts.empty()) {
        buffer.clear();
        continue;
      }

      for (size_t i = 0; i + 1 < block.stmts.size(); ++i) {
        interp.runStatement(*block.stmts[i]);
      }

      const auto& last = block.stmts.back();
      if (auto* expression = std::get_if<ExprStmt>(&last->node)) {
        Value value = interp.evalRepl(expression->expr);
        interp.defineGlobal("_", value, false);
        if (!value.isNil()) std::cout << value.repr() << '\n';
      } else {
        interp.runStatement(*last);
      }
      buffer.clear();
    } catch (const RuntimeError& error) {
      printRuntimeError("<repl>", buffer, error);
      buffer.clear();
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif

  std::vector<std::string> arguments(argv + 1, argv + argc);
  std::string file;
  std::string evalCode;
  bool hasEval = false;
  bool checkOnly = false;
  std::vector<std::string> scriptArgs;

  for (size_t i = 0; i < arguments.size(); ++i) {
    const std::string& arg = arguments[i];
    if (arg == "-h" || arg == "--help") {
      std::cout << "Jolt " << kVersion << "\n\n" << kUsage;
      return 0;
    }
    if (arg == "-v" || arg == "--version") {
      std::cout << "Jolt " << kVersion << '\n';
      return 0;
    }
    if (arg == "-e" || arg == "--eval") {
      if (i + 1 >= arguments.size()) {
        std::cerr << "jolt: option '" << arg << "' needs an argument\n";
        return 64;
      }
      evalCode = arguments[++i];
      hasEval = true;
      scriptArgs.assign(arguments.begin() + static_cast<std::ptrdiff_t>(i + 1), arguments.end());
      break;
    }
    if (arg == "-c" || arg == "--check") {
      checkOnly = true;
      continue;
    }
    if (arg == "--") {
      if (i + 1 < arguments.size()) {
        file = arguments[i + 1];
        scriptArgs.assign(arguments.begin() + static_cast<std::ptrdiff_t>(i + 2), arguments.end());
      }
      break;
    }
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << "jolt: unknown option '" << arg << "'\n";
      return 64;
    }
    file = arg;
    scriptArgs.assign(arguments.begin() + static_cast<std::ptrdiff_t>(i + 1), arguments.end());
    break;
  }

  if (hasEval) return runSource(evalCode, "<eval>", scriptArgs, checkOnly);

  if (!file.empty()) {
    std::string source;
    if (!readSourceFile(file, source)) {
      std::cerr << "jolt: cannot read file '" << file << "'\n";
      return 66;
    }
    return runSource(source, file, scriptArgs, checkOnly);
  }

  if (checkOnly) {
    std::cerr << "jolt: --check requires a file\n";
    return 64;
  }

  repl(scriptArgs);
  return 0;
}
