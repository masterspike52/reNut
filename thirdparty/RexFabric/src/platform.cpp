#include "platform.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <shellapi.h>
#else
#include <unistd.h>
#endif

namespace rexfabric {
namespace {

#if defined(_WIN32)

void Open(const std::wstring& target) { ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

#else

void Open(const std::string& target) {
#if defined(__APPLE__)
  const char* opener = "open";
#else
  const char* opener = "xdg-open";
#endif
  if (fork() == 0) {
    execlp(opener, opener, target.c_str(), static_cast<char*>(nullptr));
    _exit(1);
  }
}

#endif

}  // namespace

#if defined(_WIN32)
void OpenUrl(const std::string& url) { Open(std::wstring(url.begin(), url.end())); }
void OpenFolder(const std::filesystem::path& folder) { Open(folder.wstring()); }
#else
void OpenUrl(const std::string& url) { Open(url); }
void OpenFolder(const std::filesystem::path& folder) { Open(folder.string()); }
#endif

}  // namespace rexfabric
