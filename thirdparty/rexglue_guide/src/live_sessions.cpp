// Xbox LIVE sessions and invites, backed by RexLiveless.
//
// Titles create, search and join sessions through XGI messages (app 0xFB)
// sent with XMsgStartIORequest. A host's XSESSION_INFO (session id, its
// XNADDR, key) and the contexts/properties the title advertises go to the
// server; searches come back from it. Peers then talk directly, the same way
// as over System Link (net.cpp).

#include "live_sessions.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <rex/logging.h>

#include "guest.h"
#include "http.h"
#include "live.h"
#include "live_stats.h"
#include "net.h"

using namespace rexglue_guide::guest;
using json = nlohmann::json;

namespace rexglue_guide::sessions {
namespace {

constexpr uint32_t kSuccess = 0;
constexpr uint32_t kIoPending = 997;

// A context or property the title set with XUserSetContext/PropertyEx.
struct Attr {
  bool context = false;
  uint8_t type = 0;      // XUSER_DATA type (property id >> 28)
  uint8_t value[8] = {};  // big-endian union value as the title gave it
  std::vector<uint8_t> data;  // wide string / binary payload
};

struct Session {
  bool host = false;
  uint8_t info[60] = {};  // XSESSION_INFO: XNKID, host XNADDR, XNKEY
  uint32_t flags = 0;
  uint32_t slots_public = 0, slots_private = 0;
  int filled_public = 0, filled_private = 0;
};

std::mutex g_mutex;
std::map<uint32_t, Attr> g_attrs;
std::map<uint32_t, Session> g_sessions;  // by session handle
std::string g_hosted_id;                 // hex XNKID of the session we host
std::map<std::string, std::vector<uint8_t>> g_qos;  // XNetQosListen data by hex XNKID

struct Invite {
  bool pending = false;
  uint64_t inviter = 0;
  uint8_t info[60] = {};
};
Invite g_invite;

std::string Hex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789ABCDEF";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += d[p[i] >> 4];
    s += d[p[i] & 15];
  }
  return s;
}

std::vector<uint8_t> Unhex(const std::string& s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(uint8_t(std::stoul(s.substr(i, 2), nullptr, 16)));
  return out;
}

std::string TitleId() {
  char t[9];
  std::snprintf(t, sizeof(t), "%08X", REX_KERNEL_STATE()->title_id());
  return t;
}

http::Response Api(const std::string& method, const std::string& path, const json& body = nullptr) {
  const LiveSnapshot s = live::Snapshot();
  if (!s.signed_in || s.token.empty()) return {};
  http::Endpoint endpoint{s.server_host, s.server_port, s.server_tls};
  return http::Request(endpoint, method, path, body.is_null() ? "" : body.dump(), s.token, "application/json", 5000);
}

json AttrsJson(bool contexts) {
  json list = json::array();
  for (const auto& [id, a] : g_attrs) {
    if (a.context != contexts) continue;
    if (contexts) {
      list.push_back({{"id", id}, {"value", Load32(a.value)}});
    } else {
      list.push_back({{"id", id}, {"type", a.type}, {"value", Hex(a.value, 8)}, {"data", Hex(a.data.data(), a.data.size())}});
    }
  }
  return list;
}

// Registers (or refreshes) the hosted session with the server. Call with
// g_mutex held.
void PublishLocked(const Session& s) {
  json body = {{"id", Hex(s.info, 8)},
               {"title_id", TitleId()},
               {"info", Hex(s.info, 60)},
               {"flags", s.flags},
               {"slots_public", s.slots_public},
               {"slots_private", s.slots_private},
               {"filled_public", s.filled_public},
               {"filled_private", s.filled_private},
               {"properties", AttrsJson(false)},
               {"contexts", AttrsJson(true)}};
  const auto r = Api("POST", "/api/sessions", body);
  if (r.status != 200) REXKRNL_WARN("RexLiveless: couldn't publish session ({} {})", r.status, r.error);
}

void Complete(PPCContext& ctx, uint32_t overlapped, uint32_t result) {
  if (overlapped) {
    REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped, result);
    return Return(ctx, kIoPending);
  }
  Return(ctx, result);
}

// ----------------------------------------------------------- XGI handlers --

uint32_t SessionCreate(const uint8_t* buf) {
  const uint32_t handle = Load32(buf + 0);
  const uint32_t flags = Load32(buf + 4);
  Session s;
  s.flags = flags;
  s.slots_public = Load32(buf + 8);
  s.slots_private = Load32(buf + 0xC);
  uint8_t* info = At(Load32(buf + 0x14));
  uint8_t* nonce = At(Load32(buf + 0x18));
  std::random_device rd;
  std::mt19937_64 rng(rd());
  if (flags & 1) {  // XSESSION_CREATE_HOST
    s.host = true;
    const uint64_t kid = rng(), key0 = rng(), key1 = rng();
    std::memcpy(s.info, &kid, 8);
    s.info[0] = uint8_t((s.info[0] & 0x0F) | 0x80);  // online peer session id
    net::LocalXnAddr().Write(s.info + 8);
    std::memcpy(s.info + 0x2C, &key0, 8);
    std::memcpy(s.info + 0x34, &key1, 8);
    if (info) std::memcpy(info, s.info, 60);
    if (nonce) Store64(nonce, rng());
  } else if (info) {
    std::memcpy(s.info, info, 60);
  }
  std::lock_guard lock(g_mutex);
  if (s.host) {
    g_hosted_id = Hex(s.info, 8);
    PublishLocked(s);
  }
  g_sessions[handle] = s;
  REXKRNL_INFO("RexLiveless: XSessionCreate {} flags {:08X} session {}", s.host ? "host" : "join", flags, Hex(s.info, 8));
  return kSuccess;
}

uint32_t SessionModify(const uint8_t* buf) {
  std::lock_guard lock(g_mutex);
  auto it = g_sessions.find(Load32(buf));
  if (it == g_sessions.end()) return kSuccess;
  it->second.flags = Load32(buf + 4);
  it->second.slots_public = Load32(buf + 8);
  it->second.slots_private = Load32(buf + 0xC) & 0xFFFF;
  if (it->second.host) PublishLocked(it->second);
  return kSuccess;
}

uint32_t SessionMembers(const uint8_t* buf, int sign) {
  std::lock_guard lock(g_mutex);
  auto it = g_sessions.find(Load32(buf));
  if (it == g_sessions.end()) return kSuccess;
  const int count = int(Load32(buf + 4));
  it->second.filled_public = std::max(0, it->second.filled_public + sign * count);
  if (it->second.host) PublishLocked(it->second);
  return kSuccess;
}

uint32_t SessionDelete(const uint8_t* buf) {
  std::lock_guard lock(g_mutex);
  auto it = g_sessions.find(Load32(buf));
  if (it == g_sessions.end()) return kSuccess;
  if (it->second.host) {
    Api("DELETE", "/api/sessions/" + Hex(it->second.info, 8));
    if (g_hosted_id == Hex(it->second.info, 8)) g_hosted_id.clear();
  }
  g_sessions.erase(it);
  return kSuccess;
}

void CaptureContext(const uint8_t* buf) {
  Attr a;
  a.context = true;
  Store32(a.value, Load32(buf + 20));
  std::lock_guard lock(g_mutex);
  g_attrs[Load32(buf + 16)] = a;
}

void CaptureProperty(const uint8_t* buf) {
  const uint32_t id = Load32(buf + 16);
  const uint32_t size = Load32(buf + 20);
  const uint8_t* value = At(Load32(buf + 24));
  Attr a;
  a.type = uint8_t(id >> 28);
  if (value) {
    if (a.type == 4 || a.type == 6) a.data.assign(value, value + std::min<uint32_t>(size, 0x400));
    else std::memcpy(a.value, value, std::min<uint32_t>(size, 8));
  }
  std::lock_guard lock(g_mutex);
  g_attrs[id] = a;
}

// XUserGetContext: this title passes XUSER_CONTEXT* at +0x14.
uint32_t GetContext(const uint8_t* buf) {
  uint8_t* context = At(Load32(buf + 0x14));
  if (!context) return kSuccess;
  std::lock_guard lock(g_mutex);
  auto it = g_attrs.find(Load32(context));
  Store32(context + 4, it != g_attrs.end() && it->second.context ? Load32(it->second.value) : 0);
  return kSuccess;
}

// Guest memory for search results' properties, contexts and strings: after
// the results in the title's buffer while it lasts, then the system heap.
struct ResultSpace {
  uint32_t next, end;
  uint32_t Alloc(uint32_t size) {
    size = (size + 7) & ~7u;
    if (next + size <= end) {
      const uint32_t p = next;
      next += size;
      return p;
    }
    return REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
  }
};

// One XSESSION_SEARCHRESULT (0x5C) from a server session, with its properties
// and contexts, and any the search filtered on that the host didn't set.
void WriteSearchResult(const json& s, uint32_t res_ptr, ResultSpace& space, const uint8_t* filter_props,
                       uint32_t num_props, const uint8_t* filter_ctx, uint32_t num_ctx);

// Lays results out in the title's buffer: header {count, results}, the
// XSESSION_SEARCHRESULTs (0x5C each), then their properties and contexts.
uint32_t SessionSearch(const uint8_t* buf) {
  const uint32_t max_results = Load32(buf + 8);
  const uint32_t num_props = Load16(buf + 12), num_ctx = Load16(buf + 14);
  const uint8_t* filter_props = At(Load32(buf + 16));
  const uint8_t* filter_ctx = At(Load32(buf + 20));
  const uint32_t out_ptr = Load32(buf + 28);
  const uint32_t out_size = Load32(buf + 24);
  uint8_t* out = At(out_ptr);
  if (!out) return kSuccess;
  Store32(out, 0);
  Store32(out + 4, 0);

  const auto r = Api("GET", "/api/sessions?title_id=" + TitleId());
  json sessions = json::array();
  if (r.status == 200) {
    try {
      sessions = json::parse(r.body).value("sessions", json::array());
    } catch (...) {
    }
  }
  const uint32_t count = std::min<uint32_t>(uint32_t(sessions.size()), max_results);
  const uint32_t results_ptr = out_ptr + 8;
  ResultSpace space{results_ptr + count * 0x5C, out_ptr + out_size};
  for (uint32_t i = 0; i < count; ++i) {
    WriteSearchResult(sessions[i], results_ptr + i * 0x5C, space, filter_props, num_props, filter_ctx, num_ctx);
  }
  Store32(out, count);
  Store32(out + 4, count ? results_ptr : 0);
  REXKRNL_INFO("RexLiveless: XSessionSearchEx -> {} session(s)", count);
  return kSuccess;
}

// XSessionSearchByID: {user, XNKID (8, unaligned at +4), results buffer size,
// XSESSION_SEARCHRESULT_HEADER*}. Joining a friend's game (Nuts & Bolts finds
// the session its presence named this way): the session from the server, or
// no results when it has ended.
uint32_t SessionSearchById(const uint8_t* buf) {
  const std::string id = Hex(buf + 4, 8);
  const uint32_t out_size = Load32(buf + 12);
  const uint32_t out_ptr = Load32(buf + 16);
  uint8_t* out = At(out_ptr);
  if (!out) return kSuccess;
  Store32(out, 0);
  Store32(out + 4, 0);

  json session;
  const auto r = Api("GET", "/api/sessions/" + id);
  if (r.status == 200) {
    try {
      session = json::parse(r.body).at("session");
    } catch (...) {
    }
  }
  if (!session.is_object()) {
    REXKRNL_INFO("RexLiveless: XSessionSearchByID {} -> not found ({} {})", id, r.status, r.error);
    return kSuccess;
  }
  const uint32_t results_ptr = out_ptr + 8;
  ResultSpace space{results_ptr + 0x5C, out_ptr + out_size};
  WriteSearchResult(session, results_ptr, space, nullptr, 0, nullptr, 0);
  Store32(out, 1);
  Store32(out + 4, results_ptr);
  REXKRNL_INFO("RexLiveless: XSessionSearchByID {} -> found", id);
  return kSuccess;
}

void WriteSearchResult(const json& s, uint32_t res_ptr, ResultSpace& space, const uint8_t* filter_props,
                       uint32_t num_props, const uint8_t* filter_ctx, uint32_t num_ctx) {
  auto alloc = [&](uint32_t size) { return space.Alloc(size); };
  {
    uint8_t* res = At(res_ptr);
    std::memset(res, 0, 0x5C);
    const auto info = Unhex(s.value("info", ""));
    std::memcpy(res, info.data(), std::min<size_t>(info.size(), 60));
    const uint32_t pub = s.value("slots_public", 0u), priv = s.value("slots_private", 0u);
    const uint32_t fpub = s.value("filled_public", 0u), fpriv = s.value("filled_private", 0u);
    Store32(res + 0x3C, pub > fpub ? pub - fpub : 0);
    Store32(res + 0x40, priv > fpriv ? priv - fpriv : 0);
    Store32(res + 0x44, fpub);
    Store32(res + 0x48, fpriv);

    // Properties: the host's, then the system ones titles expect, then any
    // the search filtered on that the host didn't set.
    struct Prop {
      uint32_t id;
      uint8_t type;
      uint8_t value[8];
      std::vector<uint8_t> data;
    };
    std::vector<Prop> props;
    auto has = [&](uint32_t id) {
      for (auto& p : props) if (p.id == id) return true;
      return false;
    };
    for (const json& p : s.value("properties", json::array())) {
      Prop prop{p.value("id", 0u), uint8_t(p.value("type", 0)), {}, Unhex(p.value("data", ""))};
      const auto v = Unhex(p.value("value", ""));
      std::memcpy(prop.value, v.data(), std::min<size_t>(v.size(), 8));
      props.push_back(std::move(prop));
    }
    const json host = s.value("host", json::object());
    if (!has(0x40008109)) {  // GAMER_HOSTNAME
      Prop p{0x40008109, 4, {}, {}};
      for (char c : host.value("gamertag", std::string("Player"))) {
        p.data.push_back(0);
        p.data.push_back(uint8_t(c));
      }
      p.data.push_back(0);
      p.data.push_back(0);
      props.push_back(p);
    }
    if (!has(0x20008107)) {  // GAMER_PUID
      Prop p{0x20008107, 2, {}, {}};
      Store64(p.value, std::strtoull(host.value("xuid", std::string("0")).c_str(), nullptr, 16));
      props.push_back(p);
    }
    for (uint32_t f = 0; filter_props && f < num_props && f < 32; ++f) {
      const uint8_t* fp = filter_props + f * 0x18;
      const uint32_t id = Load32(fp);
      if (!id || has(id)) continue;
      Prop p{id, fp[8], {}, {}};
      std::memcpy(p.value, fp + 0x10, 8);
      props.push_back(p);
    }
    // Titles read these without checking they exist.
    for (uint32_t id : {0x10000010u, 0x20000002u, 0x10000003u}) {
      if (!has(id)) props.push_back({id, uint8_t(id >> 28), {}, {}});
    }
    const uint32_t props_ptr = alloc(uint32_t(props.size()) * 0x18);
    for (size_t p = 0; p < props.size(); ++p) {
      uint8_t* dst = At(props_ptr + uint32_t(p) * 0x18);
      std::memset(dst, 0, 0x18);
      Store32(dst, props[p].id);
      dst[8] = props[p].type;
      if (props[p].type == 4 || props[p].type == 6) {
        const uint32_t data_ptr = alloc(std::max<uint32_t>(2, uint32_t(props[p].data.size())));
        std::memcpy(At(data_ptr), props[p].data.data(), props[p].data.size());
        Store32(dst + 0x10, uint32_t(props[p].data.size()));
        Store32(dst + 0x14, data_ptr);
      } else {
        std::memcpy(dst + 0x10, props[p].value, 8);
      }
    }
    Store32(res + 0x4C, uint32_t(props.size()));
    Store32(res + 0x54, props_ptr);

    std::vector<std::pair<uint32_t, uint32_t>> contexts;
    for (const json& c : s.value("contexts", json::array())) contexts.push_back({c.value("id", 0u), c.value("value", 0u)});
    for (uint32_t f = 0; filter_ctx && f < num_ctx && f < 32; ++f) {
      const uint32_t id = Load32(filter_ctx + f * 8);
      bool found = false;
      for (auto& c : contexts) found |= c.first == id;
      if (!found) contexts.push_back({id, Load32(filter_ctx + f * 8 + 4)});
    }
    const uint32_t ctx_ptr = alloc(uint32_t(contexts.size()) * 8);
    for (size_t c = 0; c < contexts.size(); ++c) {
      Store32(At(ctx_ptr + uint32_t(c) * 8), contexts[c].first);
      Store32(At(ctx_ptr + uint32_t(c) * 8 + 4), contexts[c].second);
    }
    Store32(res + 0x50, uint32_t(contexts.size()));
    Store32(res + 0x58, ctx_ptr);
  }
}

}  // namespace

std::string HostedSessionId() {
  std::lock_guard lock(g_mutex);
  return g_hosted_id;
}

bool AcceptInvite(const std::string& session_id, uint64_t inviter, std::string* error) {
  const auto r = Api("GET", "/api/sessions/" + session_id);
  if (r.status != 200) {
    if (error) *error = r.status == 404 ? "That game has ended." : "Couldn't reach RexLiveless.";
    return false;
  }
  try {
    const auto info = Unhex(json::parse(r.body).at("session").value("info", ""));
    std::lock_guard lock(g_mutex);
    g_invite = {};
    g_invite.pending = true;
    g_invite.inviter = inviter;
    std::memcpy(g_invite.info, info.data(), std::min<size_t>(info.size(), 60));
  } catch (...) {
    if (error) *error = "The invite was damaged.";
    return false;
  }
  // XN_LIVE_INVITE_ACCEPTED for user 0; the title then asks for the details.
  REX_KERNEL_STATE()->BroadcastNotification(0x02000002, 0);
  return true;
}

// XInviteGetAcceptedInfo: arg list entry 1 points at the title's XINVITE_INFO.
bool FillAcceptedInvite(uint32_t args_ptr) {
  const uint8_t* args = At(args_ptr);
  if (!args) return false;
  uint8_t* out = At(uint32_t(Load64(args + 16 + 8)));
  std::lock_guard lock(g_mutex);
  if (!out || !g_invite.pending) return false;
  std::memset(out, 0, 0x54);
  Store64(out + 0x00, live::Snapshot().me.xuid);
  Store64(out + 0x08, g_invite.inviter);
  Store32(out + 0x10, REX_KERNEL_STATE()->title_id());
  std::memcpy(out + 0x14, g_invite.info, 60);
  Store32(out + 0x50, 1);  // fFromGameInvite
  return true;
}

void PublishQos(const uint8_t kid[8], std::vector<uint8_t> data) {
  const std::string id = Hex(kid, 8);
  {
    // A copy here, so a joiner in this same process (tests) finds it at once.
    std::lock_guard lock(g_mutex);
    g_qos[id] = data;
    // Members of a session listen too; only the host's data is published.
    if (id != g_hosted_id) return;
  }
  std::thread([id, data = std::move(data)] {
    const auto r = Api("POST", "/api/sessions/" + id + "/qos", {{"data", Hex(data.data(), data.size())}});
    if (r.status != 200) REXKRNL_WARN("RexLiveless: couldn't publish QoS data for {} ({} {})", id, r.status, r.error);
  }).detach();
}

bool FetchQos(const uint8_t kid[8], std::vector<uint8_t>* data) {
  const std::string id = Hex(kid, 8);
  {
    std::lock_guard lock(g_mutex);
    if (auto it = g_qos.find(id); it != g_qos.end()) {
      *data = it->second;
      return true;
    }
  }
  const auto r = Api("GET", "/api/sessions/" + id);
  if (r.status != 200) return false;
  try {
    *data = Unhex(json::parse(r.body).at("session").value("qos", ""));
  } catch (...) {
    return false;
  }
  return !data->empty();
}

namespace {

// XSESSION_CREATE flags that decide whether friends join through presence.
constexpr uint32_t kCreateUsesPresence = 0x00000002;
constexpr uint32_t kCreateJoinViaPresenceDisabled = 0x00000200;
constexpr auto kJoinableRefresh = std::chrono::seconds(5);

std::mutex g_joinable_mutex;
std::map<uint64_t, std::array<uint8_t, 8>> g_joinable;  // host XUID -> XNKID
std::map<uint64_t, std::string> g_host_gamertags;       // host XUID -> gamertag
std::chrono::steady_clock::time_point g_joinable_at;
bool g_joinable_listing = false;

void RefreshJoinable() {
  std::map<uint64_t, std::array<uint8_t, 8>> joinable;
  std::map<uint64_t, std::string> gamertags;
  const auto r = Api("GET", "/api/sessions?title_id=" + TitleId());
  if (r.status == 200) {
    try {
      for (const json& s : json::parse(r.body).value("sessions", json::array())) {
        const json host = s.value("host", json::object());
        const uint64_t xuid = std::strtoull(host.value("xuid", std::string("0")).c_str(), nullptr, 16);
        if (xuid && host.contains("gamertag")) gamertags[xuid] = host.value("gamertag", std::string());
        // A server that leaves the flags out: every listed session counts.
        const uint32_t flags = s.value("flags", kCreateUsesPresence);
        if (!(flags & kCreateUsesPresence) || (flags & kCreateJoinViaPresenceDisabled)) continue;
        const auto info = Unhex(s.value("info", ""));
        if (!xuid || info.size() < 8) continue;
        std::array<uint8_t, 8> kid{};
        std::memcpy(kid.data(), info.data(), kid.size());
        joinable[xuid] = kid;
      }
    } catch (...) {
    }
  }
  std::lock_guard lock(g_joinable_mutex);
  g_joinable = std::move(joinable);
  g_host_gamertags = std::move(gamertags);
  g_joinable_at = std::chrono::steady_clock::now();
  g_joinable_listing = false;
}

// With g_joinable_mutex held.
void RefreshJoinableIfStaleLocked() {
  if (!g_joinable_listing && std::chrono::steady_clock::now() - g_joinable_at >= kJoinableRefresh) {
    g_joinable_listing = true;
    std::thread(RefreshJoinable).detach();
  }
}

}  // namespace

void RefreshFriendSessions() {
  std::lock_guard lock(g_joinable_mutex);
  RefreshJoinableIfStaleLocked();
}

std::string HostGamertag(uint64_t xuid) {
  std::lock_guard lock(g_joinable_mutex);
  RefreshJoinableIfStaleLocked();
  auto it = g_host_gamertags.find(xuid);
  return it == g_host_gamertags.end() ? std::string() : it->second;
}

bool FriendJoinableSession(uint64_t xuid, uint8_t kid[8]) {
  std::lock_guard lock(g_joinable_mutex);
  RefreshJoinableIfStaleLocked();
  auto it = g_joinable.find(xuid);
  if (it == g_joinable.end()) return false;
  std::memcpy(kid, it->second.data(), it->second.size());
  return true;
}

}  // namespace rexglue_guide::sessions

using namespace rexglue_guide::sessions;

// XMsgStartIORequest(app, message, overlapped, buffer, length)
extern "C" REX_FUNC(__imp__XMsgStartIORequest) {
  const uint32_t app = ctx.r3.u32, message = ctx.r4.u32, overlapped = ctx.r5.u32;
  const uint8_t* buf = At(ctx.r6.u32);
  const bool signed_in = rexglue_guide::live::Snapshot().signed_in;
  if (app == 0xFB && buf) {
    switch (message) {
      case 0x000B0006:  // XUserSetContextEx: remember for advertising
        CaptureContext(buf);
        break;
      case 0x000B0007:  // XUserSetPropertyEx
        CaptureProperty(buf);
        break;
      case 0x000B0041:  // XUserGetContext
        return Complete(ctx, overlapped, GetContext(buf));
      case 0x000B0010:
        if (signed_in) return Complete(ctx, overlapped, SessionCreate(buf));
        break;
      case 0x000B0011:
        if (signed_in) return Complete(ctx, overlapped, SessionDelete(buf));
        break;
      case 0x000B0012:
        if (signed_in) return Complete(ctx, overlapped, SessionMembers(buf, 1));
        break;
      case 0x000B0013:
        if (signed_in) return Complete(ctx, overlapped, SessionMembers(buf, -1));
        break;
      case 0x000B0018:
        if (signed_in) return Complete(ctx, overlapped, SessionModify(buf));
        break;
      case 0x000B0014:  // start
      case 0x000B0015:  // end
      case 0x000B001E:  // migrate host
        if (signed_in) return Complete(ctx, overlapped, 0);
        break;
      case 0x000B001C:
        if (signed_in) return Complete(ctx, overlapped, SessionSearch(buf));
        break;
      case 0x000B001B:  // XSessionSearchByID
        if (signed_in) return Complete(ctx, overlapped, SessionSearchById(buf));
        break;
      case 0x000B0021:  // XUserReadStats
        if (signed_in) return Complete(ctx, overlapped, rexglue_guide::stats::ReadStats(buf));
        break;
      case 0x000B0025:  // XSessionWriteStats
        if (signed_in) return Complete(ctx, overlapped, rexglue_guide::stats::WriteStats(buf));
        break;
      default:
        break;
    }
  }
  if (app == 0xFC && message == 0x00050002 && signed_in) {
    // XInviteSend from the title. The marshaled arguments aren't decoded yet;
    // invites go out from the Guide instead.
    REXKRNL_INFO("RexLiveless: XInviteSend buffer {:08X} length {}", ctx.r6.u32, ctx.r7.u32);
    return Complete(ctx, overlapped, 0);
  }
  CallOriginal("__imp__XMsgStartIORequest", ctx, base);
}
