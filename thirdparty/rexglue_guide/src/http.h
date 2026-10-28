#pragma once
// Minimal blocking HTTP/1.1 client and a worker thread that runs requests off
// the game's threads. https:// goes through WinHTTP on Windows and the
// system's libcurl (loaded at runtime) elsewhere; http:// uses plain sockets.

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace rexglue_guide::http {

struct Response {
  int status = 0;  // 0 = no response (connection failed)
  std::string body;
  std::string error;  // transport error, when status == 0
};

struct Endpoint {
  std::string host = "127.0.0.1";
  uint16_t port = 36100;
  bool tls = false;
};

// Parses "http://host:port" or "https://host:port"; false if it isn't one.
bool ParseUrl(const std::string& url, Endpoint* out);

Response Request(const Endpoint& endpoint, const std::string& method, const std::string& path,
                 const std::string& body, const std::string& token,
                 const std::string& content_type = "application/json", int timeout_ms = 8000);

// Runs requests one at a time on its own thread. Completions are queued and
// delivered by Pump() on the caller's thread.
class Worker {
 public:
  using Done = std::function<void(const Response&)>;
  Worker();
  ~Worker();
  void Post(Endpoint endpoint, std::string method, std::string path, std::string body,
            std::string token, Done done, std::string content_type = "application/json");
  void Pump();
  size_t pending() const;

 private:
  struct Job {
    Endpoint endpoint;
    std::string method, path, body, token, content_type;
    Done done;
  };
  void Run();
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Job> jobs_;
  std::deque<std::pair<Done, Response>> done_;
  size_t in_flight_ = 0;
  bool stop_ = false;
  std::thread thread_;
};

}  // namespace rexglue_guide::http
