#include "module_resolver.h"

#include <fstream>

#include "util.h"

namespace {

bool fileExists(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return static_cast<bool>(file);
}

std::string joinPath(const std::string& dir, const std::string& relative) {
  if (dir.empty()) return relative;
  char last = dir.back();
  if (last == '/' || last == '\\') return dir + relative;
  return dir + "/" + relative;
}

}  // namespace

std::string modulePathToFile(const std::string& path) {
  std::string result;
  result.reserve(path.size() + 5);
  for (char c : path) result += (c == '.') ? '/' : c;
  result += ".jolt";
  return result;
}

bool ModuleResolver::isReservedStd(const std::string& path) {
  return path == "std" || startsWith(path, "std.");
}

bool ModuleResolver::resolve(const std::string& path, std::string& file,
                             std::string& message) const {
  message = "module '" + path + "' not found";

  std::string relative = modulePathToFile(path);

  if (isReservedStd(path)) {
    std::string rest = path == "std" ? std::string() : path.substr(4);
    if (!rest.empty() && !stdlibDir_.empty()) {
      std::string candidate = joinPath(stdlibDir_, modulePathToFile(rest));
      if (fileExists(candidate)) {
        file = std::move(candidate);
        return true;
      }
    }
    // `std.` is reserved for the standard library: a user file that happens to
    // sit at the same path (std/foo.jolt) is an error, not a valid module.
    if (fileExists(joinPath(projectDir_, relative)) ||
        (!packagesDir_.empty() && fileExists(joinPath(packagesDir_, relative)))) {
      message = "module '" + path + "' is reserved for the standard library";
    }
    return false;
  }

  std::string projectCandidate = joinPath(projectDir_, relative);
  if (fileExists(projectCandidate)) {
    file = std::move(projectCandidate);
    return true;
  }
  if (!packagesDir_.empty()) {
    std::string packageCandidate = joinPath(packagesDir_, relative);
    if (fileExists(packageCandidate)) {
      file = std::move(packageCandidate);
      return true;
    }
  }
  return false;
}
