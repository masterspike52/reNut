#include "http.h"

#include <string_view>
#include <type_traits>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#endif

namespace rexfabric::net {
namespace {

constexpr int kTimeoutMs = 15000;
constexpr const char* kUserAgent = "RexFabric/1.0";

}  // namespace

#if defined(_WIN32)

namespace {

std::wstring Wide(std::string_view text) { return std::wstring(text.begin(), text.end()); }

}  // namespace

Http::Http() { session_ = WinHttpOpen(Wide(kUserAgent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0); }

Http::~Http() {
  if (session_) WinHttpCloseHandle(session_);
}

bool Http::Available() { return true; }

bool Http::Get(const std::string& url, std::string& body, std::string& error) {
  constexpr std::string_view kScheme = "https://";
  const size_t path_start = url.starts_with(kScheme) ? url.find('/', kScheme.size()) : std::string::npos;
  if (!session_ || path_start == std::string::npos) {
    error = "Bad request.";
    return false;
  }
  const std::wstring host = Wide(std::string_view(url).substr(kScheme.size(), path_start - kScheme.size()));
  const std::wstring path = Wide(std::string_view(url).substr(path_start));
  HINTERNET connection = WinHttpConnect(session_, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
  HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
  bool ok = request != nullptr;
  if (ok) {
    WinHttpSetTimeouts(request, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
    ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr);
    if (!ok) error = "Could not reach " + std::string(host.begin(), host.end()) + ".";
  }
  if (ok) {
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    char buffer[16384];
    for (DWORD read = 0; WinHttpReadData(request, buffer, sizeof(buffer), &read) && read; read = 0) body.append(buffer, read);
    ok = status >= 200 && status < 300;
    if (!ok) error = "The server answered " + std::to_string(status) + ".";
  }
  if (request) WinHttpCloseHandle(request);
  if (connection) WinHttpCloseHandle(connection);
  return ok;
}

#else

// libcurl's stable ABI numbers, from curl.h.
namespace curl {

constexpr int kOptWriteData = 10001;
constexpr int kOptUrl = 10002;
constexpr int kOptUserAgent = 10018;
constexpr int kOptFollowLocation = 52;
constexpr int kOptMaxRedirs = 68;
constexpr int kOptNoSignal = 99;
constexpr int kOptTimeoutMs = 155;
constexpr int kOptConnectTimeoutMs = 156;
constexpr int kOptWriteFunction = 20011;
constexpr int kInfoResponseCode = 0x200002;
constexpr long kGlobalDefault = 3;

struct Api {
  int (*global_init)(long flags);
  void* (*easy_init)();
  int (*easy_setopt)(void* easy, int option, ...);
  int (*easy_perform)(void* easy);
  int (*easy_getinfo)(void* easy, int info, ...);
  void (*easy_cleanup)(void* easy);
};

// The first libcurl there is; Debian and Ubuntu desktops often have only the GnuTLS build.
const Api* Load() {
  static const Api* const api = []() -> const Api* {
    static Api loaded;
    for (const char* name : {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so", "libcurl.4.dylib"}) {
      void* library = dlopen(name, RTLD_NOW | RTLD_LOCAL);
      if (!library) continue;
      const auto get = [library](auto& fn, const char* symbol) { return (fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(library, symbol))) != nullptr; };
      if (get(loaded.global_init, "curl_global_init") && get(loaded.easy_init, "curl_easy_init") && get(loaded.easy_setopt, "curl_easy_setopt") && get(loaded.easy_perform, "curl_easy_perform") && get(loaded.easy_getinfo, "curl_easy_getinfo") && get(loaded.easy_cleanup, "curl_easy_cleanup") && loaded.global_init(kGlobalDefault) == 0) return &loaded;
      dlclose(library);
    }
    return nullptr;
  }();
  return api;
}

size_t Append(char* data, size_t size, size_t count, void* out) {
  static_cast<std::string*>(out)->append(data, size * count);
  return size * count;
}

}  // namespace curl

Http::Http() {
  if (const curl::Api* api = curl::Load()) session_ = api->easy_init();
}

Http::~Http() {
  if (session_) curl::Load()->easy_cleanup(session_);
}

bool Http::Available() { return curl::Load() != nullptr; }

bool Http::Get(const std::string& url, std::string& body, std::string& error) {
  const curl::Api* api = curl::Load();
  if (!api || !session_) {
    error = "Downloading needs libcurl; install your system's libcurl package.";
    return false;
  }
  api->easy_setopt(session_, curl::kOptUrl, url.c_str());
  api->easy_setopt(session_, curl::kOptUserAgent, kUserAgent);
  api->easy_setopt(session_, curl::kOptNoSignal, 1L);
  api->easy_setopt(session_, curl::kOptConnectTimeoutMs, static_cast<long>(kTimeoutMs));
  api->easy_setopt(session_, curl::kOptTimeoutMs, static_cast<long>(kTimeoutMs * 4));
  api->easy_setopt(session_, curl::kOptFollowLocation, 1L);
  api->easy_setopt(session_, curl::kOptMaxRedirs, 5L);
  api->easy_setopt(session_, curl::kOptWriteFunction, &curl::Append);
  api->easy_setopt(session_, curl::kOptWriteData, &body);
  if (api->easy_perform(session_) != 0) {
    error = "Could not reach the server.";
    return false;
  }
  long status = 0;
  api->easy_getinfo(session_, curl::kInfoResponseCode, &status);
  if (status >= 200 && status < 300) return true;
  error = "The server answered " + std::to_string(status) + ".";
  return false;
}

#endif

}  // namespace rexfabric::net
