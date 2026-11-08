#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "ast.h"
#include "builtins.h"
#include "compiler.h"
#include "diagnostic.h"
#include "lexer.h"
#include "module_resolver.h"
#include "parser.h"
#include "util.h"
#include "vm.h"

namespace {

const char *kVersion = "0.1.0";

const char *kUsage =
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

// Joins a directory and a relative path without doubling the separator.
std::string joinPath(const std::string &dir, const std::string &relative) {
  if (dir.empty()) return relative;
  char last = dir.back();
  if (last == '/' || last == '\\') return dir + relative;
  return dir + "/" + relative;
}

// Absolute path of the running executable, so `standardLibraryDir` can locate
// the shipped `std/` even when `jolt` was invoked through `PATH` and `argv[0]`
// has no directory part. Falls back to `argv0` when it cannot be determined.
std::string executablePath(const std::string &argv0) {
  if (argv0.find('/') != std::string::npos ||
      argv0.find('\\') != std::string::npos) {
    return argv0;
  }
#ifdef _WIN32
  char buffer[MAX_PATH];
  DWORD n = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
  if (n > 0 && n < MAX_PATH) return std::string(buffer, n);
#elif defined(__linux__)
  char buffer[4096];
  ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (n > 0) return std::string(buffer, static_cast<size_t>(n));
#endif
  // Last resort: search `PATH` for the bare name.
  if (const char *path = std::getenv("PATH")) {
    std::string dirs(path);
    size_t start = 0;
    while (start <= dirs.size()) {
      size_t end = dirs.find(':', start);
      std::string dir = dirs.substr(start, end == std::string::npos
                                               ? std::string::npos
                                               : end - start);
      if (!dir.empty()) {
        std::string candidate = joinPath(dir, argv0);
        std::ifstream probe(candidate, std::ios::binary);
        if (probe.good()) return candidate;
      }
      if (end == std::string::npos) break;
      start = end + 1;
    }
  }
  return argv0;
}

// Directory of the standard library shipped with the toolchain. `std.*`
// imports resolve here (see AGENT.md §18: `std.` is reserved).
//
// `$JOLT_STDLIB` overrides the search. Otherwise the standard library is
// looked for in the working directory and, walking up from the executable's
// directory, in `<dir>/std`. The upward walk is what lets an import resolve
// whether `jolt` sits in the source tree, in a single-config build
// (`<root>/build`), in a multi-config build (`<root>/build/<config>`, MSVC) or
// in an install prefix; the `share/jolt/std` location covers the install
// layout explicitly.
std::string standardLibraryDir(const std::string &argv0) {
  if (const char *env = std::getenv("JOLT_STDLIB")) return env;

  auto hasStd = [](const std::string &dir) {
    std::string ignored;
    return readSourceFile(joinPath(dir, "math.jolt"), ignored);
  };

  if (hasStd("std")) return "std";

  std::string dir = directoryOf(executablePath(argv0));
  for (int depth = 0; depth < 4; ++depth) {
    std::string candidate = joinPath(dir, "std");
    if (hasStd(candidate)) return candidate;
    dir = joinPath(dir, "..");
  }

  std::string installed = joinPath(directoryOf(executablePath(argv0)), "../share/jolt/std");
  if (hasStd(installed)) return installed;

  return "std";  // Nothing found: imports report "module not found".
}

// Installed-packages root: `$JOLT_PACKAGES`, else `$HOME/.jolt/packages`.
// Empty when the home directory cannot be determined, which disables the
// package lookup step.
std::string packagesDir() {
  if (const char *explicitDir = std::getenv("JOLT_PACKAGES")) return explicitDir;
  const char *home = std::getenv("HOME");
#ifdef _WIN32
  if (home == nullptr || *home == '\0') home = std::getenv("USERPROFILE");
#endif
  if (home == nullptr || *home == '\0') return std::string();
  return std::string(home) + "/.jolt/packages";
}

// Resolver for a program named `name`. Imports resolve from the project root:
// the directory of the file being compiled, or the current directory for
// `-e`/stdin. A nested import resolves from the same root, so `modules.util`
// names `<root>/modules/util.jolt` no matter which module declares it.
ModuleResolver resolverFor(const std::string &name, const std::string &argv0) {
  bool isFile = !name.empty() && name[0] != '<';
  std::string projectDir = isFile ? directoryOf(name) : std::string(".");
  return ModuleResolver(projectDir, standardLibraryDir(argv0), packagesDir());
}

// Builds the module loader the compiler uses for `import` declarations. It
// resolves the dotted path to a file, reads and parses it, and returns the
// program. A path that cannot be resolved becomes the compile error
// "module 'X' not found"; a parse error inside the module is reported with the
// module's own file name in the message.
Compiler::ImportLoader makeImportLoader(ModuleResolver resolver) {
  // The loader owns its resolver: it outlives the statement that built it.
  return [resolver](const std::string &path, int line, int col) -> StmtPtr {
    std::string file;
    std::string message;
    if (!resolver.resolve(path, file, message)) {
      throw CompileError{message, line, col};
    }
    std::string moduleSource;
    if (!readSourceFile(file, moduleSource)) {
      throw CompileError{"cannot read module '" + path + "' from '" + file + "'", line, col};
    }
    try {
      Parser parser(lex(moduleSource), file);
      return parser.parseProgram();
    } catch (const CompileError &error) {
      throw CompileError{"in module '" + path + "': " + error.message, error.line, error.col};
    }
  };
}

// Compiles `source` (resolving any imports) and, unless `checkOnly`, runs it.
// Returns 0 on success, 65 for compile errors, 70 for runtime errors.
int runSource(const std::string &source, const std::string &name,
              const std::vector<std::string> &scriptArgs, bool checkOnly,
              const std::string &argv0) {
  std::vector<Token> tokens;
  try {
    tokens = lex(source);
  } catch (const CompileError &error) {
    printCompileError(name, source, error, platformPosix());
    return 65;
  }

  StmtPtr program;
  try {
    Parser parser(std::move(tokens), name);
    program = parser.parseProgram();
  } catch (const CompileError &error) {
    printCompileError(name, source, error, platformPosix());
    return 65;
  }

  // Imports are resolved during compilation, so `--check` validates them too.
  Bytecode bytecode;
  Compiler compiler;
  try {
    compiler.setImportLoader(makeImportLoader(resolverFor(name, argv0)));
    bytecode = compiler.compile(program);
  } catch (const CompileError &error) {
    printCompileError(name, source, error, platformPosix());
    return 65;
  }
  for (const Warning &warning : compiler.warnings()) {
    printCompileWarning(name, source, warning, platformPosix());
  }

  if (checkOnly) return 0;

  VM vm;
  vm.sourceName = name;
  vm.scriptArgs = scriptArgs;
  registerBuiltins(vm);
  try {
    vm.run(bytecode);
  } catch (const RuntimeError &error) {
    printRuntimeError(name, source, error, platformPosix());
    return 70;
  }
  return 0;
}

bool isStatementStart(const std::string &text) {
  // An attribute (`@MustUse`) always begins a declaration, never an expression.
  if (!text.empty() && text[0] == '@') return true;
  size_t end = 0;
  while (end < text.size() &&
         (std::isalnum(static_cast<unsigned char>(text[end])) ||
          text[end] == '_')) {
    ++end;
  }
  std::string word = text.substr(0, end);
  return word == "let" || word == "var" || word == "const" || word == "fn" ||
         word == "if" || word == "while" || word == "for" || word == "return" ||
         word == "break" || word == "continue" || word == "else" ||
         word == "import" || word == "switch" || word == "struct" ||
         word == "native" || word == "extern";
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
bool handleCommand(VM &vm, Compiler &compiler, const std::string &command,
                   const std::string &argv0) {
  if (command == ":quit" || command == ":q" || command == ":exit")
    return true;

  if (command == ":help" || command == ":?") {
    printReplHelp();
    return false;
  }

  if (command == ":clear") {
    vm.clearGlobals();
    registerBuiltins(vm);
    // The session state (types, structs, imports) belongs to the definitions
    // just forgotten, so it is dropped with them; otherwise a later entry
    // would still see a type or a module the user no longer has.
    compiler.clearSession();
    std::cout << "all definitions cleared\n";
    return false;
  }

  if (command == ":vars") {
    std::vector<std::pair<std::string, std::string>> entries;
    for (const auto &entry : vm.globals()) {
      const Value &value = entry.second;
      if (value.isFunc() && value.func().isNative())
        continue;
      // Internal globals (the `$module$...` tables that keep imported modules
      // alive) are not user definitions, so they stay out of the listing.
      if (!entry.first.empty() && entry.first[0] == '$')
        continue;
      entries.emplace_back(entry.first, value.typeName());
    }
    std::sort(entries.begin(), entries.end());
    if (entries.empty()) {
      std::cout << "(no user definitions)\n";
    }
    for (const auto &entry : entries) {
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
    // `:load` runs the file *in the current session*: it sees the session's
    // structs, types and imports, and its own definitions persist for later
    // entries. Its imports still resolve relative to the file, so the file's
    // loader is used only for this call.
    try {
      Parser parser(lex(source), path);
      parser.seedStructNames(compiler.structNames());
      Bytecode bytecode = compiler.compileLoaded(
          parser.parseProgram(), makeImportLoader(resolverFor(path, argv0)), path);
      for (const Warning &warning : compiler.warnings())
        printCompileWarning(path, source, warning, platformPosix());
      vm.run(bytecode);
    } catch (const CompileError &error) {
      printCompileError(path, source, error, platformPosix());
    } catch (const RuntimeError &error) {
      printRuntimeError(path, source, error, platformPosix());
    }
    return false;
  }

  std::cout << "unknown command '" << command << "'; try :help\n";
  return false;
}

// Reports the value of a REPL expression and keeps it in `_`, like the AST
// interpreter did.
void publishReplValue(VM &vm, const Value &value) {
  vm.defineGlobal("_", value, false);
  if (!value.isNil())
    std::cout << value.repr() << '\n';
}

void repl(const std::vector<std::string> &scriptArgs, const std::string &argv0) {
  VM vm;
  vm.sourceName = "<repl>";
  vm.scriptArgs = scriptArgs;
  registerBuiltins(vm);

  // One compiler for the whole session so a module imported in an earlier
  // entry is compiled once and reused by later entries.
  Compiler compiler;
  compiler.setImportLoader(makeImportLoader(resolverFor("<repl>", argv0)));

  std::cout << "Jolt " << kVersion
            << " - a small scripting language written in C++\n";
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
      if (handleCommand(vm, compiler, trimmed, argv0))
        break;
      continue;
    }
    if (buffer.empty() && trimmed.empty())
      continue;

    buffer += line;
    buffer += '\n';

    std::vector<Token> tokens;
    try {
      tokens = lex(buffer);
    } catch (const CompileError &error) {
      if (error.message.find("unterminated") != std::string::npos)
        continue;
      printCompileError("<repl>", buffer, error, platformPosix());
      buffer.clear();
      continue;
    }

    // A new REPL entry that begins with a statement keyword is parsed as a
    // program; everything else is first tried as a bare expression.
    size_t firstNewline = buffer.find('\n');
    std::string firstLineText = firstNewline == std::string::npos
                                    ? buffer
                                    : buffer.substr(0, firstNewline);

    if (!isStatementStart(trim(firstLineText))) {
      try {
        Parser expressionParser(tokens, "<repl>");
        expressionParser.seedStructNames(compiler.structNames());
        auto expr = expressionParser.parseFullExpression();
        Bytecode bytecode = compiler.compileExpression(expr);
        Value value = vm.run(bytecode);
        publishReplValue(vm, value);
        buffer.clear();
        continue;
      } catch (const CompileError &) {
        // Fall through and try to parse it as a program.
      } catch (const RuntimeError &error) {
        printRuntimeError("<repl>", buffer, error, platformPosix());
        buffer.clear();
        continue;
      }
    }

    StmtPtr program;
    try {
      Parser parser(tokens, "<repl>");
      parser.seedStructNames(compiler.structNames());
      program = parser.parseProgram();
    } catch (const CompileError &error) {
      if (error.message.find("end of input") != std::string::npos) {
        // Convenience: accept a missing ';' at the end of a REPL entry.
        try {
          Parser parser(lex(buffer + ";"), "<repl>");
          parser.seedStructNames(compiler.structNames());
          program = parser.parseProgram();
        } catch (const CompileError &retry) {
          bool incomplete =
              retry.message.find("end of input") != std::string::npos ||
              retry.message.find("unterminated") != std::string::npos;
          if (incomplete)
            continue;
          printCompileError("<repl>", buffer, retry, platformPosix());
          buffer.clear();
          continue;
        }
      } else {
        printCompileError("<repl>", buffer, error, platformPosix());
        buffer.clear();
        continue;
      }
    }

    try {
      const auto &block = std::get<BlockStmt>(program->node);
      if (block.stmts.empty()) {
        buffer.clear();
        continue;
      }

      bool endsWithExpression = false;
      const ExprPtr *tailExpression = nullptr;
      if (const auto *last = std::get_if<ExprStmt>(&block.stmts.back()->node)) {
        endsWithExpression = true;
        tailExpression = &last->expr;
      }

      Bytecode bytecode = compiler.compileRepl(program);
      for (const Warning &warning : compiler.warnings())
        printCompileWarning("<repl>", buffer, warning, platformPosix());
      Value value = vm.run(bytecode);
      if (endsWithExpression && tailExpression)
        publishReplValue(vm, value);
      buffer.clear();
    } catch (const CompileError &error) {
      printCompileError("<repl>", buffer, error, platformPosix());
      buffer.clear();
    } catch (const RuntimeError &error) {
      printRuntimeError("<repl>", buffer, error, platformPosix());
      buffer.clear();
    }
  }
}

} // namespace

int main(int argc, char **argv) {
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
    const std::string &arg = arguments[i];
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
      scriptArgs.assign(arguments.begin() + static_cast<std::ptrdiff_t>(i + 1),
                        arguments.end());
      break;
    }
    if (arg == "-c" || arg == "--check") {
      checkOnly = true;
      continue;
    }
    if (arg == "--") {
      if (i + 1 < arguments.size()) {
        file = arguments[i + 1];
        scriptArgs.assign(arguments.begin() +
                              static_cast<std::ptrdiff_t>(i + 2),
                          arguments.end());
      }
      break;
    }
    if (!arg.empty() && arg[0] == '-') {
      std::cerr << "jolt: unknown option '" << arg << "'\n";
      return 64;
    }
    file = arg;
    scriptArgs.assign(arguments.begin() + static_cast<std::ptrdiff_t>(i + 1),
                      arguments.end());
    break;
  }

  if (hasEval)
    return runSource(evalCode, "<eval>", scriptArgs, checkOnly, argv[0]);

  if (!file.empty()) {
    std::string source;
    if (!readSourceFile(file, source)) {
      std::cerr << "jolt: cannot read file '" << file << "'\n";
      return 66;
    }
    return runSource(source, file, scriptArgs, checkOnly, argv[0]);
  }

  if (checkOnly) {
    std::cerr << "jolt: --check requires a file\n";
    return 64;
  }

  repl(scriptArgs, argv[0]);
  return 0;
}
