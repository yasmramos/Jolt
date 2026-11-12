#pragma once

#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

template <class... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

inline std::string trim(const std::string& s) {
  size_t begin = 0;
  size_t end = s.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
  return s.substr(begin, end - begin);
}

inline bool startsWith(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Directory part of a path ("a/b/c.jolt" -> "a/b", "c.jolt" -> "."). Used to
// resolve an import relative to the file that declares it.
inline std::string directoryOf(const std::string& path) {
  size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos) return ".";
  if (slash == 0) return "/";
  return path.substr(0, slash);
}

inline bool readSourceFile(const std::string& path, std::string& out) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return false;
  std::ostringstream buffer;
  buffer << file.rdbuf();
  out = buffer.str();
  // Skip a UTF-8 BOM if present.
  if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
      static_cast<unsigned char>(out[1]) == 0xBB &&
      static_cast<unsigned char>(out[2]) == 0xBF) {
    out.erase(0, 3);
  }
  return true;
}

// Writes raw bytes to `path`, creating or truncating it. Used by `jolt build`
// to drop a generated executable; `data` is already the complete image.
inline bool writeBinaryFile(const std::string& path,
                            const std::vector<std::uint8_t>& data) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) return false;
  file.write(reinterpret_cast<const char*>(data.data()),
             static_cast<std::streamsize>(data.size()));
  return static_cast<bool>(file);
}

// The path without its extension ("a/b.jolt" -> "a/b", "a/b" -> "a/b"). A dot
// in a directory component is not an extension, so only the last component is
// inspected.
inline std::string stripExtension(const std::string& path) {
  size_t slash = path.find_last_of("/\\");
  size_t start = slash == std::string::npos ? 0 : slash + 1;
  size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || dot < start) return path;
  return path.substr(0, dot);
}
