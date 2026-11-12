#pragma once

#include <string>

// Resolves an import path to a source file on disk.
//
// Oskel's import grammar is Java-style (`import std.math;`, `import myapp.utils.*;`),
// so a module is named by a dotted path, not by a file path. The resolver maps
// that path to a file:
//
//   - `std.*`  -> the standard library: `<stdlibDir>/<rest>.oskl`. `std` is
//                reserved, so the caller rejects a user module named `std.*`.
//   - anything -> the project directory (the directory of the file being
//                compiled), then the installed-packages directory:
//                `<projectDir>/<dotted/rest>.oskl` and
//                `<packagesDir>/<dotted/rest>.oskl`.
//
// A path not found anywhere is a compile error: "module 'X' not found".
//
// The resolver is pure path computation plus a file-existence probe, so it
// carries no compiler or VM dependency and can be unit tested on its own.
class ModuleResolver {
 public:
  // `projectDir` is the directory of the file that declares the import;
  // `stdlibDir` holds the standard library; `packagesDir` is the install root
  // for user packages (usually `<home>/.oskel/packages`). Empty roots disable
  // that lookup step.
  ModuleResolver(std::string projectDir, std::string stdlibDir, std::string packagesDir)
      : projectDir_(std::move(projectDir)),
        stdlibDir_(std::move(stdlibDir)),
        packagesDir_(std::move(packagesDir)) {}

  // Resolves `path` (a dotted module path) to an absolute or relative file
  // path. On failure returns false and sets `message` to the exact compile
  // error text: "module 'X' not found".
  bool resolve(const std::string& path, std::string& file, std::string& message) const;

  // True when `path` names the reserved standard-library prefix (`std` or
  // `std.something`). A user module declared under that prefix is an error.
  static bool isReservedStd(const std::string& path);

 private:
  std::string projectDir_;
  std::string stdlibDir_;
  std::string packagesDir_;
};

// Turns a dotted module path into a relative file path: "myapp.utils" ->
// "myapp/utils.oskl". The first segment is kept as-is; later segments become
// directory separators.
std::string modulePathToFile(const std::string& path);
