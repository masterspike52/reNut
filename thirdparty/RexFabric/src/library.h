// A dynamically loaded library: LoadLibrary on Windows, dlopen elsewhere.

#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace rexfabric {

class Library {
 public:
  // Null on failure, with the reason in `error`.
  static std::unique_ptr<Library> Open(const std::filesystem::path& path, std::string& error);
  ~Library();

  Library(const Library&) = delete;
  Library& operator=(const Library&) = delete;

  void* Symbol(const char* name) const;

  template <typename T>
  T Function(const char* name) const { return reinterpret_cast<T>(Symbol(name)); }

 private:
  explicit Library(void* handle) : handle_(handle) {}

  void* handle_;
};

}  // namespace rexfabric
