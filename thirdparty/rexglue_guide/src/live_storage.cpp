// Xbox LIVE storage (XStorage*), backed by RexLiveless: game clips (the file a
// leaderboard row carries, such as Nuts & Bolts' challenge replays) and a
// title's own and per-player files.
//
// Server paths are the Guide's own, made by XStorageBuildServerPath:
//   rxl/<facility>/<title id>/<owner XUID, or ->/<location info as hex, or ->/<item name>
// The server keeps one file per path, written only by its owner. A game clip's
// location info is its leaderboard's id, so a row's replay is
// rxl/1/<title>/<player>/<view id>/ (and the server reports its size in the
// row's attachment column, 0xFFFA).
//
// XStorageUploadFromMemory, XStorageDownloadToMemory and XStorageEnumerate are
// XDK code compiled into the title, which hands XAM its requests in LIVE's own
// wire format (XLiveBase async tasks); the host hooks them at their entry
// (guide.h) and they come here with their arguments.

#include "live_storage.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xio.h>
#include <rex/system/xthread.h>

#include <rexglue_guide/guide.h>

#include "guest.h"
#include "http.h"
#include "live.h"

using namespace rexglue_guide::guest;
using json = nlohmann::json;

namespace rexglue_guide::storage {
namespace {

// XSTORAGE_FACILITY_PER_TITLE: the title's own files, no owner. Game clips
// (1) and per-player files (3) belong to a player.
constexpr uint32_t kFacilityPerTitle = 2;
constexpr const char* kGameClipPrefix = "rxl/1/";
// XStorageBuildServerPathByXuid's user index.
constexpr uint32_t kUserByXuid = 0xFE;

constexpr uint32_t kOk = 0;
constexpr uint32_t kInvalidArg = 0x80070057;
constexpr uint32_t kTooSmall = 0x8007007A;
constexpr uint32_t kFail = 0x80004005;
constexpr uint32_t kNotLoggedOn = 0x80151802;     // X_ONLINE_E_LOGON_NOT_LOGGED_ON
constexpr uint32_t kFileNotFound = 0x8015C004;    // X_ONLINE_E_STORAGE_FILE_NOT_FOUND

// Win32 results.
constexpr uint32_t kIoPending = 997;
constexpr uint32_t kFunctionFailed = 1627;
constexpr uint32_t kInsufficientBuffer = 122;

constexpr int kTimeoutMs = 30000;
constexpr size_t kMaxPath = 512;
constexpr uint32_t kDownloadResultsSize = 20;  // XSTORAGE_DOWNLOAD_TO_MEMORY_RESULTS
constexpr uint32_t kEnumerateHeaderSize = 12;  // XSTORAGE_ENUMERATE_RESULTS
constexpr uint32_t kFileInfoSize = 65;         // XSTORAGE_FILE_INFO, packed
constexpr uint32_t kMaxEnumerate = 100;

// Guest strings are big-endian UTF-16. Paths and item names are ASCII.
std::string ReadWide(uint32_t address) {
  std::string s;
  const uint8_t* p = At(address);
  for (size_t i = 0; p && i < kMaxPath; ++i) {
    const uint16_t c = Load16(p + i * 2);
    if (!c) break;
    s.push_back(c < 0x80 ? char(c) : '_');
  }
  return s;
}

void WriteWide(uint8_t* p, const std::string& s) {
  for (size_t i = 0; i < s.size(); ++i) Store16(p + i * 2, uint8_t(s[i]));
  Store16(p + s.size() * 2, 0);
}

std::string Hex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789ABCDEF";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += d[p[i] >> 4];
    s += d[p[i] & 15];
  }
  return s;
}

std::string HexId(uint64_t v, int digits) {
  char t[17];
  std::snprintf(t, sizeof(t), "%0*llX", digits, static_cast<unsigned long long>(v));
  return t;
}

std::string UrlEncode(const std::string& s) {
  static const char* d = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += char(c);
    } else {
      out += '%';
      out += d[c >> 4];
      out += d[c & 15];
    }
  }
  return out;
}

// FILETIME from Unix seconds.
uint64_t FileTime(int64_t unix_seconds) {
  return uint64_t(unix_seconds) * 10000000ull + 116444736000000000ull;
}

void StoreFileTime(uint8_t* p, int64_t unix_seconds) {
  const uint64_t ft = FileTime(unix_seconds);
  Store32(p, uint32_t(ft));  // dwLowDateTime
  Store32(p + 4, uint32_t(ft >> 32));
}

// The player a call is made as (user index 0-3); player 1 for any other.
LiveSnapshot Player(uint32_t user) {
  return live::Snapshot(user < uint32_t(live::kMaxUsers) ? int(user) : 0);
}

http::Response Call(const LiveSnapshot& s, const std::string& method, const std::string& path, std::string body,
                    const std::string& content_type) {
  if (!s.signed_in || s.token.empty()) return {};
  const http::Endpoint endpoint{s.server_host, s.server_port, s.server_tls};
  return http::Request(endpoint, method, path, body, s.token, content_type, kTimeoutMs);
}

// Ours to answer: a RexLiveless account is signed in, and the path is one the
// Guide built.
bool Ours(const LiveSnapshot& s, const std::string& path) {
  return s.signed_in && path.rfind("rxl/", 0) == 0;
}

// An overlapped call is under way: pending until Complete.
void Start(uint32_t overlapped) {
  uint8_t* ov = At(overlapped);
  if (!ov) return;
  rex::system::XOverlappedSetResult(ov, kIoPending);
  rex::system::XOverlappedSetExtendedError(ov, 0);
  rex::system::XOverlappedSetContext(ov, rex::system::XThread::GetCurrentThreadHandle());
  if (const auto event = rex::system::XOverlappedGetEvent(ov)) {
    if (auto ev = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XEvent>(event)) ev->Reset();
  }
}

struct Outcome {
  uint32_t result = kOk;  // Win32
  uint32_t extended = 0;  // HRESULT
  uint32_t length = 0;
};

Outcome Failed(uint32_t hresult) {
  return {hresult == kTooSmall ? kInsufficientBuffer : kFunctionFailed, hresult, 0};
}

// Runs a call: on its own thread with an overlapped (completed when done),
// otherwise now. Returns what the title's call returns.
template <typename Work>
uint32_t Run(uint32_t overlapped, Work work) {
  if (!overlapped) return work().result;
  Start(overlapped);
  std::thread([overlapped, work = std::move(work)]() mutable {
    const Outcome o = work();
    REX_KERNEL_STATE()->CompleteOverlappedEx(overlapped, o.result, o.extended, o.length);
  }).detach();
  return kIoPending;
}

}  // namespace

// {user index (0xFE: by XUID), padding, XUID (+8), facility (+0x10), location
//  info* (+0x14), its size (+0x18), item name* (+0x1C), server path out
//  (+0x20), DWORD* its length (+0x24, in: room in characters)}.
uint32_t BuildServerPath(uint32_t args_ptr) {
  const uint8_t* a = At(args_ptr);
  if (!a) return kInvalidArg;
  const uint32_t user = Load32(a);
  uint64_t xuid = Load64(a + 8);
  const uint32_t facility = Load32(a + 0x10);
  const uint8_t* info = At(Load32(a + 0x14));
  const uint32_t info_size = std::min<uint32_t>(Load32(a + 0x18), 64);
  const std::string name = ReadWide(Load32(a + 0x1C));
  uint8_t* out = At(Load32(a + 0x20));
  uint8_t* length = At(Load32(a + 0x24));
  if (!out || !length) return kInvalidArg;
  if (user != kUserByXuid) {
    if (user >= uint32_t(live::kMaxUsers)) return kInvalidArg;
    const LiveSnapshot s = live::Snapshot(int(user));
    if (!s.signed_in) return kNotLoggedOn;
    xuid = s.me.xuid;
  }
  const std::string owner = facility == kFacilityPerTitle ? "-" : HexId(xuid, 16);
  const std::string location = info && info_size ? Hex(info, info_size) : "-";
  const std::string path = "rxl/" + std::to_string(facility) + "/" + HexId(REX_KERNEL_STATE()->title_id(), 8) + "/" + owner +
                           "/" + location + "/" + name;
  const uint32_t room = Load32(length);
  Store32(length, uint32_t(path.size() + 1));
  if (path.size() + 1 > room) return kTooSmall;
  WriteWide(out, path);
  return kOk;
}

}  // namespace rexglue_guide::storage

namespace rexglue_guide {

using namespace storage;

// XStorageUploadFromMemory(user, server path, size, buffer, overlapped)
bool StorageUploadFromMemory(uint32_t user, uint32_t server_path, uint32_t size, uint32_t buffer,
                             uint32_t overlapped, uint32_t* result) {
  const std::string path = ReadWide(server_path);
  const LiveSnapshot s = Player(user);
  if (!Ours(s, path)) return false;
  const uint8_t* data = At(buffer);
  if (!data && size) {
    *result = kFunctionFailed;
    return true;
  }
  // A leaderboard replay goes with its score: both fail then.
  if (path.rfind(kGameClipPrefix, 0) == 0) {
    if (const std::string why = live::LeaderboardBlocked(); !why.empty()) {
      REXKRNL_INFO("RexLiveless: {} refused ({})", path, why);
      *result = Run(overlapped, [] { return Failed(kFail); });
      return true;
    }
  }
  std::string body(reinterpret_cast<const char*>(data), size);
  *result = Run(overlapped, [s, path, body = std::move(body)] {
    const auto r = Call(s, "PUT", "/api/storage/file?path=" + UrlEncode(path) + "&build=" + UrlEncode(live::Build()), body,
                        "application/octet-stream");
    if (r.status != 200) {
      REXKRNL_WARN("RexLiveless: couldn't upload {} ({} {}{})", path, r.status, r.error, r.status ? " " + r.body : "");
      return Failed(kFail);
    }
    REXKRNL_INFO("RexLiveless: uploaded {} ({} bytes)", path, body.size());
    return Outcome{kOk, 0, uint32_t(body.size())};
  });
  return true;
}

// XStorageDownloadToMemory(user, server path, size, buffer, results size,
// XSTORAGE_DOWNLOAD_TO_MEMORY_RESULTS*, overlapped). The results: bytes, owner
// XUID (unaligned at +4), created (FILETIME at +12). The server's answer: owner
// XUID and created (Unix seconds), 8 bytes each, then the file.
bool StorageDownloadToMemory(uint32_t user, uint32_t server_path, uint32_t size, uint32_t buffer, uint32_t results_size,
                             uint32_t results, uint32_t overlapped, uint32_t* result) {
  const std::string path = ReadWide(server_path);
  const LiveSnapshot s = Player(user);
  if (!Ours(s, path)) return false;
  *result = Run(overlapped, [s, path, size, buffer, results_size, results] {
    const auto r = Call(s, "GET", "/api/storage/file?path=" + UrlEncode(path), "", "application/json");
    if (r.status == 404) return Failed(kFileNotFound);
    if (r.status != 200 || r.body.size() < 16) {
      REXKRNL_WARN("RexLiveless: couldn't download {} ({} {})", path, r.status, r.error);
      return Failed(kFail);
    }
    const auto* head = reinterpret_cast<const uint8_t*>(r.body.data());
    const uint64_t owner = Load64(head);
    const int64_t created = int64_t(Load64(head + 8));
    const uint32_t bytes = uint32_t(r.body.size() - 16);
    if (uint8_t* out = At(results); out && results_size >= kDownloadResultsSize) {
      Store32(out, bytes);
      Store64(out + 4, owner);
      StoreFileTime(out + 12, created);
    }
    if (bytes > size) return Failed(kTooSmall);
    if (uint8_t* dst = At(buffer)) std::memcpy(dst, r.body.data() + 16, bytes);
    REXKRNL_INFO("RexLiveless: downloaded {} ({} bytes)", path, bytes);
    return Outcome{kOk, 0, bytes};
  });
  return true;
}

// XStorageEnumerate(user, server path, first, max, results size,
// XSTORAGE_ENUMERATE_RESULTS*, overlapped). The results: total, returned,
// XSTORAGE_FILE_INFO* (packed, 65 bytes each), then the items and their
// paths. A path's item name may have '*' wildcards.
bool StorageEnumerate(uint32_t user, uint32_t server_path, uint32_t start, uint32_t max, uint32_t results_size,
                      uint32_t results, uint32_t overlapped, uint32_t* result) {
  const std::string path = ReadWide(server_path);
  const LiveSnapshot s = Player(user);
  if (!Ours(s, path)) return false;
  if (!At(results) || results_size < kEnumerateHeaderSize) {
    *result = kInsufficientBuffer;
    return true;
  }
  *result = Run(overlapped, [s, path, start, max, results_size, results] {
    const json query = {{"path", path}, {"start", start}, {"max", std::min(max, kMaxEnumerate)}};
    const auto r = Call(s, "POST", "/api/storage/enumerate", query.dump(), "application/json");
    json answer;
    try {
      if (r.status == 200) answer = json::parse(r.body);
    } catch (...) {
    }
    if (!answer.is_object()) {
      REXKRNL_WARN("RexLiveless: couldn't list {} ({} {})", path, r.status, r.error);
      return Failed(kFail);
    }
    const json items = answer.value("items", json::array());
    uint8_t* out = At(results);
    const uint32_t items_ptr = results + kEnumerateHeaderSize;
    uint32_t next = items_ptr + uint32_t(items.size()) * kFileInfoSize;
    const uint32_t end = results + results_size;
    uint32_t returned = 0;
    for (const json& item : items) {
      const std::string name = item.value("path", std::string());
      const uint32_t name_bytes = uint32_t(name.size() + 1) * 2;
      if (items_ptr + (returned + 1) * kFileInfoSize > end || next + name_bytes > end) break;
      uint8_t* info = At(items_ptr + returned * kFileInfoSize);
      std::memset(info, 0, kFileInfoSize);
      Store32(info + 0, REX_KERNEL_STATE()->title_id());
      Store64(info + 8, std::strtoull(item.value("owner", std::string("0")).c_str(), nullptr, 16));
      Store32(info + 29, item.value("size", 0u));  // dwStorageSize
      Store32(info + 33, item.value("size", 0u));  // dwInstalledSize
      StoreFileTime(info + 37, item.value("created", int64_t(0)));
      StoreFileTime(info + 45, item.value("updated", int64_t(0)));
      Store16(info + 55, uint16_t(name.size()));
      Store32(info + 57, next);
      WriteWide(At(next), name);
      next += name_bytes;
      ++returned;
    }
    Store32(out, answer.value("total", 0u));
    Store32(out + 4, returned);
    Store32(out + 8, returned ? items_ptr : 0);
    return Outcome{kOk, 0, 0};
  });
  return true;
}

}  // namespace rexglue_guide
