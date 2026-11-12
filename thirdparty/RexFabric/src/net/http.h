// HTTPS GET for the Thunderstore client: WinHTTP on Windows, the system's libcurl elsewhere, loaded at run
// time so the build needs neither its headers nor its library.

#pragma once

#include <string>

namespace rexfabric::net {

// One session per thread that makes requests; it keeps connections open between them.
class Http {
 public:
  Http();
  ~Http();
  Http(const Http&) = delete;
  Http& operator=(const Http&) = delete;

  // Follows redirects. False with `error` set when the request fails or answers other than 2xx.
  bool Get(const std::string& url, std::string& body, std::string& error);

  static bool Available();

 private:
  void* session_ = nullptr;
};

}  // namespace rexfabric::net
