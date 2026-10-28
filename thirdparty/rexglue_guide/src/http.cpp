#include "http.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <type_traits>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
using socket_t = SOCKET;
constexpr socket_t kBadSocket = INVALID_SOCKET;
static void CloseSocket(socket_t s) {
  closesocket(s);
}
#else
#include <dlfcn.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kBadSocket = -1;
static void CloseSocket(socket_t s) {
  close(s);
}
#endif

namespace rexglue_guide::http {
namespace {

void EnsureSockets() {
#if defined(_WIN32)
  static const bool started = [] {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  (void)started;
#endif
}

void SetTimeouts(socket_t s, int timeout_ms) {
#if defined(_WIN32)
  DWORD t = DWORD(timeout_ms);
#else
  timeval t{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
#endif
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof(t));
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

std::string DecodeChunked(const std::string& raw) {
  std::string out;
  size_t pos = 0;
  while (pos < raw.size()) {
    const size_t line_end = raw.find("\r\n", pos);
    if (line_end == std::string::npos) break;
    const size_t size = std::strtoul(raw.substr(pos, line_end - pos).c_str(), nullptr, 16);
    if (size == 0) break;
    pos = line_end + 2;
    out.append(raw, pos, std::min(size, raw.size() - pos));
    pos += size + 2;
  }
  return out;
}

#if defined(_WIN32)
std::wstring Wide(const std::string& s) {
  return std::wstring(s.begin(), s.end());  // hosts, methods, paths and headers are ASCII
}

// One request over HTTPS. WinHTTP checks the certificate against the system
// store and fails the request if it doesn't match the host.
Response RequestTls(const Endpoint& endpoint, const std::string& method, const std::string& path,
                    const std::string& body, const std::string& token, const std::string& content_type,
                    int timeout_ms) {
  static const HINTERNET session = WinHttpOpen(L"RexglueGuide/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  Response response;
  if (!session) {
    response.error = "Can't open a network connection.";
    return response;
  }
  HINTERNET connection = WinHttpConnect(session, Wide(endpoint.host).c_str(), endpoint.port, 0);
  HINTERNET request = connection ? WinHttpOpenRequest(connection, Wide(method).c_str(), Wide(path).c_str(), nullptr,
                                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                      WINHTTP_FLAG_SECURE)
                                 : nullptr;
  const auto finish = [&](const char* error) {
    if (error) response.error = error;
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    return response;
  };
  if (!request) return finish("Can't find the RexLiveless server.");
  WinHttpSetTimeouts(request, timeout_ms, timeout_ms, timeout_ms, timeout_ms);

  std::string headers;
  if (!token.empty()) headers += "Authorization: Bearer " + token + "\r\n";
  if (!body.empty() || method == "POST" || method == "PUT") headers += "Content-Type: " + content_type + "\r\n";
  const std::wstring wide_headers = Wide(headers);
  if (!WinHttpSendRequest(request, wide_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : wide_headers.c_str(),
                          DWORD(wide_headers.size()), const_cast<char*>(body.data()), DWORD(body.size()),
                          DWORD(body.size()), 0) ||
      !WinHttpReceiveResponse(request, nullptr)) {
    return finish(GetLastError() == ERROR_WINHTTP_SECURE_FAILURE
                      ? "The RexLiveless server's certificate isn't valid."
                      : "Can't connect to the RexLiveless server.");
  }
  DWORD status = 0, size = sizeof(status);
  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                      &status, &size, WINHTTP_NO_HEADER_INDEX);
  char buffer[16384];
  for (DWORD n = 0; WinHttpReadData(request, buffer, sizeof(buffer), &n) && n; n = 0) response.body.append(buffer, n);
  response.status = int(status);
  return finish(nullptr);
}
#else
// HTTPS through the system's libcurl, loaded at runtime so the build needs no
// curl headers or library. The option and error numbers are libcurl's stable
// ABI (curl.h).
namespace curl {

using Handle = void;
struct Slist;

constexpr int kOptWriteData = 10001;
constexpr int kOptUrl = 10002;
constexpr int kOptPostFields = 10015;
constexpr int kOptUserAgent = 10018;
constexpr int kOptHttpHeader = 10023;
constexpr int kOptCustomRequest = 10036;
constexpr int kOptPost = 47;
constexpr int kOptPostFieldSize = 60;
constexpr int kOptNoSignal = 99;
constexpr int kOptTimeoutMs = 155;
constexpr int kOptConnectTimeoutMs = 156;
constexpr int kOptWriteFunction = 20011;
constexpr int kInfoResponseCode = 0x200002;
constexpr long kGlobalDefault = 3;
constexpr int kErrorCouldntResolveHost = 6;
constexpr int kErrorSslConnect = 35;
constexpr int kErrorPeerFailedVerification = 60;
constexpr int kErrorSslCaCert = 77;

struct Api {
  int (*global_init)(long flags);
  Handle* (*easy_init)();
  int (*easy_setopt)(Handle* easy, int option, ...);
  int (*easy_perform)(Handle* easy);
  int (*easy_getinfo)(Handle* easy, int info, ...);
  void (*easy_cleanup)(Handle* easy);
  Slist* (*slist_append)(Slist* list, const char* text);
  void (*slist_free_all)(Slist* list);
};

// The first libcurl there is (Debian and Ubuntu desktops often have only the
// GnuTLS build), or nullptr.
const Api* Load() {
  static const Api* const api = []() -> const Api* {
    static Api loaded;
    for (const char* name : {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so"}) {
      void* lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
      if (!lib) continue;
      const auto get = [lib](auto& fn, const char* symbol) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, symbol));
        return fn != nullptr;
      };
      if (get(loaded.global_init, "curl_global_init") && get(loaded.easy_init, "curl_easy_init") &&
          get(loaded.easy_setopt, "curl_easy_setopt") && get(loaded.easy_perform, "curl_easy_perform") &&
          get(loaded.easy_getinfo, "curl_easy_getinfo") && get(loaded.easy_cleanup, "curl_easy_cleanup") &&
          get(loaded.slist_append, "curl_slist_append") && get(loaded.slist_free_all, "curl_slist_free_all") &&
          loaded.global_init(kGlobalDefault) == 0) {
        return &loaded;
      }
      dlclose(lib);
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

// One request over HTTPS. libcurl checks the certificate against the system's
// trusted certificates and fails the request if it doesn't match the host.
Response RequestTls(const Endpoint& endpoint, const std::string& method, const std::string& path,
                    const std::string& body, const std::string& token, const std::string& content_type,
                    int timeout_ms) {
  Response response;
  const curl::Api* api = curl::Load();
  if (!api) {
    response.error = "HTTPS needs libcurl: install your distribution's libcurl package.";
    return response;
  }
  curl::Handle* easy = api->easy_init();
  if (!easy) {
    response.error = "Can't open a network connection.";
    return response;
  }
  const std::string url = "https://" + endpoint.host + ":" + std::to_string(endpoint.port) + path;
  const bool has_body = !body.empty() || method == "POST" || method == "PUT";
  curl::Slist* headers = nullptr;
  if (!token.empty()) headers = api->slist_append(headers, ("Authorization: Bearer " + token).c_str());
  if (has_body) headers = api->slist_append(headers, ("Content-Type: " + content_type).c_str());
  headers = api->slist_append(headers, "Expect:");  // no 100-continue round trip
  api->easy_setopt(easy, curl::kOptUrl, url.c_str());
  api->easy_setopt(easy, curl::kOptUserAgent, "RexglueGuide/1.0");
  api->easy_setopt(easy, curl::kOptHttpHeader, headers);
  api->easy_setopt(easy, curl::kOptNoSignal, 1L);  // runs on the worker thread
  api->easy_setopt(easy, curl::kOptConnectTimeoutMs, long(timeout_ms));
  api->easy_setopt(easy, curl::kOptTimeoutMs, long(timeout_ms));
  api->easy_setopt(easy, curl::kOptWriteFunction, &curl::Append);
  api->easy_setopt(easy, curl::kOptWriteData, &response.body);
  if (method == "POST") {
    api->easy_setopt(easy, curl::kOptPost, 1L);
  } else if (method != "GET") {
    api->easy_setopt(easy, curl::kOptCustomRequest, method.c_str());
  }
  if (has_body) {
    api->easy_setopt(easy, curl::kOptPostFields, body.data());
    api->easy_setopt(easy, curl::kOptPostFieldSize, long(body.size()));
  }
  const int code = api->easy_perform(easy);
  if (code == 0) {
    long status = 0;
    api->easy_getinfo(easy, curl::kInfoResponseCode, &status);
    response.status = int(status);
  } else {
    response.body.clear();
    response.error = code == curl::kErrorCouldntResolveHost ? "Can't find the RexLiveless server."
                     : code == curl::kErrorSslConnect || code == curl::kErrorPeerFailedVerification ||
                             code == curl::kErrorSslCaCert
                         ? "The RexLiveless server's certificate isn't valid."
                         : "Can't connect to the RexLiveless server.";
  }
  api->slist_free_all(headers);
  api->easy_cleanup(easy);
  return response;
}
#endif

}  // namespace

bool ParseUrl(const std::string& url, Endpoint* out) {
  const bool tls = url.rfind("https://", 0) == 0;
  if (!tls && url.rfind("http://", 0) != 0) return false;
  std::string rest = url.substr(tls ? 8 : 7);
  rest = rest.substr(0, rest.find('/'));
  const size_t colon = rest.rfind(':');
  out->host = colon == std::string::npos ? rest : rest.substr(0, colon);
  out->port = colon == std::string::npos ? (tls ? 443 : 80) : uint16_t(std::atoi(rest.c_str() + colon + 1));
  out->tls = tls;
  return !out->host.empty();
}

Response Request(const Endpoint& endpoint, const std::string& method, const std::string& path,
                 const std::string& body, const std::string& token, const std::string& content_type,
                 int timeout_ms) {
  if (endpoint.tls) return RequestTls(endpoint, method, path, body, token, content_type, timeout_ms);
  EnsureSockets();
  Response response;
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* found = nullptr;
  if (getaddrinfo(endpoint.host.c_str(), std::to_string(endpoint.port).c_str(), &hints, &found) != 0 || !found) {
    response.error = "Can't find the RexLiveless server.";
    return response;
  }
  socket_t s = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
  if (s == kBadSocket) {
    freeaddrinfo(found);
    response.error = "Can't open a network connection.";
    return response;
  }
  SetTimeouts(s, timeout_ms);
  const bool connected = connect(s, found->ai_addr, int(found->ai_addrlen)) == 0;
  freeaddrinfo(found);
  if (!connected) {
    CloseSocket(s);
    response.error = "Can't connect to the RexLiveless server.";
    return response;
  }

  std::string request = method + " " + path + " HTTP/1.1\r\nHost: " + endpoint.host + "\r\n" +
                        "Connection: close\r\nUser-Agent: RexglueGuide/1.0\r\n";
  if (!token.empty()) request += "Authorization: Bearer " + token + "\r\n";
  if (!body.empty() || method == "POST" || method == "PUT") {
    request += "Content-Type: " + content_type + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  }
  request += "\r\n";
  request += body;
  size_t sent = 0;
  while (sent < request.size()) {
    const int n = send(s, request.data() + sent, int(request.size() - sent), 0);
    if (n <= 0) {
      CloseSocket(s);
      response.error = "The connection to the RexLiveless server dropped.";
      return response;
    }
    sent += size_t(n);
  }

  std::string raw;
  char buffer[16384];
  for (;;) {
    const int n = recv(s, buffer, int(sizeof(buffer)), 0);
    if (n <= 0) break;
    raw.append(buffer, size_t(n));
  }
  CloseSocket(s);

  const size_t header_end = raw.find("\r\n\r\n");
  if (raw.rfind("HTTP/1.", 0) != 0 || header_end == std::string::npos) {
    response.error = "The RexLiveless server didn't answer.";
    return response;
  }
  response.status = std::atoi(raw.c_str() + raw.find(' ') + 1);
  const std::string headers = Lower(raw.substr(0, header_end));
  response.body = raw.substr(header_end + 4);
  if (headers.find("transfer-encoding: chunked") != std::string::npos) {
    response.body = DecodeChunked(response.body);
  }
  return response;
}

Worker::Worker() : thread_([this] { Run(); }) {}

Worker::~Worker() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  cv_.notify_all();
  thread_.join();
}

void Worker::Post(Endpoint endpoint, std::string method, std::string path, std::string body,
                  std::string token, Done done, std::string content_type) {
  {
    std::lock_guard lock(mutex_);
    jobs_.push_back({std::move(endpoint), std::move(method), std::move(path), std::move(body), std::move(token),
                     std::move(content_type), std::move(done)});
    ++in_flight_;
  }
  cv_.notify_one();
}

void Worker::Pump() {
  std::deque<std::pair<Done, Response>> done;
  {
    std::lock_guard lock(mutex_);
    done.swap(done_);
  }
  for (auto& [callback, response] : done) {
    if (callback) callback(response);
  }
}

size_t Worker::pending() const {
  std::lock_guard lock(mutex_);
  return in_flight_;
}

void Worker::Run() {
  for (;;) {
    Job job;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
      if (stop_) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    Response response = Request(job.endpoint, job.method, job.path, job.body, job.token, job.content_type);
    std::lock_guard lock(mutex_);
    done_.emplace_back(std::move(job.done), std::move(response));
    --in_flight_;
  }
}

}  // namespace rexglue_guide::http
