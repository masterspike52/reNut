/**
 * @file    online/friends.cpp
 * @brief   reNut's friends list over EOS, and the game's friends enumerator.
 *
 * N&B's friends menu (sub_826CAB08, sub_824C4A68) calls XFriendsCreateEnumerator
 * and XEnumerate, and keeps entries that are online, playing N&B (title ID) and
 * not pending friend requests; joining one searches for its session by ID
 * (sessions.cpp). XONLINE_FRIEND, 196 bytes, big-endian: xuid, gamertag[16],
 * state, sessionID, title ID, ftUserTime, xnkidInvite, gameinviteTime,
 * cchRichPresence, wszRichPresence[64].
 *
 * Friend messages (EOS channel 1): u8 type, u8 version, u8 gamertag length,
 * the gamertag, then for presence: u8 session length and the session (u8
 * id[8], u8 key[16], u8 public slots, u8 filled slots, u8 joinable, u8 QoS
 * data size, the QoS data, then u8 co-op and u32 co-op town scene, which
 * older builds leave unread; none while not hosting). Requests, answers and
 * removals go reliably; presence every couple of seconds, unreliably, and at
 * once to a friend whose connection just opened or who was just heard from
 * for the first time (so coming online shows straight away on both sides). A
 * friend whose connection closes is offline at once.
 *
 * online_friends.txt: a line per friend ("friend", online ID, gamertag) and
 * per request sent and not answered yet ("sent", online ID), tab-separated.
 */
#include "renut_engine/online/friends.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xobject.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#include "renut_engine/app/app_paths.h"
#include "renut_engine/online/eos.h"
#include "renut_engine/online/online.h"
#include "renut_engine/online/online_log.h"
#include "renut_engine/online/xnaddr.h"

REX_EXTERN(__imp__rex_XFriendsCreateEnumerator);

// The SDK's X_ERROR_* macros name these unqualified.
using rex::X_RESULT;
using rex::X_STATUS;

namespace renut::online::friends {

namespace {

using Clock = std::chrono::steady_clock;

enum MessageType : uint8_t {
  kRequest = 1,
  kAccept = 2,
  kDecline = 3,
  kRemove = 4,
  kPresence = 5,
};
constexpr uint8_t kVersion = 1;
// A friend is online while its presence keeps coming.
constexpr auto kOnlineFor = std::chrono::seconds(10);
// Requests not answered yet are sent again every few ticks (the other player
// may have been offline).
constexpr uint32_t kRequestResendTicks = 3;
constexpr uint16_t kEstimatedRttMs = 50;
constexpr size_t kMaxRequests = 32;

struct Stored {
  std::string id;
  std::string gamertag;
};

struct Live {
  std::string gamertag;
  Session session;
  Clock::time_point heard_at;
};

std::mutex g_mutex;
bool g_loaded = false;
std::vector<Stored> g_friends;
std::vector<Request> g_sent;
std::vector<Request> g_requests;
std::unordered_map<std::string, Live> g_live;
Session g_hosted;
bool g_coop_hosting = false;
uint32_t g_coop_town = 0;
// XNetQosListen's data, by session: the hosted session's is told to friends.
struct QosEntry {
  SessionId id{};
  std::array<uint8_t, 32> data{};
  uint8_t size = 0;
};
std::vector<QosEntry> g_qos;
uint32_t g_ticks = 0;

std::filesystem::path FriendsFile() {
  return renut::paths::WritableFolder() / "online_friends.txt";
}

// Requires g_mutex.
void LoadLocked() {
  if (g_loaded)
    return;
  g_loaded = true;
  std::ifstream in(FriendsFile());
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream fields(line);
    std::string kind, id, gamertag;
    std::getline(fields, kind, '\t');
    std::getline(fields, id, '\t');
    std::getline(fields, gamertag);
    id = eos::ParseId(id);
    if (id.empty())
      continue;
    if (kind == "friend")
      g_friends.push_back({id, gamertag});
    else if (kind == "sent")
      g_sent.push_back({id, gamertag});
  }
  for (const Stored &f : g_friends)
    eos::Remember(f.id);
}

// Requires g_mutex.
void SaveLocked() {
  std::ofstream out(FriendsFile(), std::ios::trunc);
  for (const Stored &f : g_friends)
    out << "friend\t" << f.id << '\t' << f.gamertag << '\n';
  for (const Request &s : g_sent)
    out << "sent\t" << s.id << '\t' << s.gamertag << '\n';
}

template <typename T> auto FindById(std::vector<T> &list, const std::string &id) {
  return std::find_if(list.begin(), list.end(), [&](const T &e) { return e.id == id; });
}

// Printable ASCII, at most 15 characters.
std::string CleanName(std::string_view name) {
  std::string out;
  for (char c : name) {
    if (out.size() >= 15)
      break;
    if (c >= 0x20 && c < 0x7F)
      out += c;
  }
  return out;
}

std::vector<uint8_t> EncodeSession(const Session &s) {
  std::vector<uint8_t> blob;
  if (!s.Valid())
    return blob;
  blob.insert(blob.end(), s.id.begin(), s.id.end());
  blob.insert(blob.end(), s.key.begin(), s.key.end());
  blob.push_back(s.public_slots);
  blob.push_back(s.filled_slots);
  blob.push_back(s.joinable ? 1 : 0);
  blob.push_back(s.qos_size);
  blob.insert(blob.end(), s.qos_data.begin(), s.qos_data.begin() + s.qos_size);
  blob.push_back(s.coop ? 1 : 0);
  for (int shift = 24; shift >= 0; shift -= 8)
    blob.push_back(uint8_t(s.coop_town >> shift));
  return blob;
}

std::optional<Session> DecodeSession(const uint8_t *data, size_t size, const std::string &id) {
  if (size < 28)
    return std::nullopt;
  Session s;
  std::memcpy(s.id.data(), data, 8);
  std::memcpy(s.key.data(), data + 8, 16);
  s.public_slots = data[24];
  s.filled_slots = data[25];
  s.joinable = data[26] != 0;
  s.qos_size = std::min<uint8_t>(data[27], uint8_t(s.qos_data.size()));
  if (size < size_t(28) + s.qos_size)
    return std::nullopt;
  std::memcpy(s.qos_data.data(), data + 28, s.qos_size);
  const size_t coop_at = size_t(28) + s.qos_size;
  if (size >= coop_at + 5) {
    s.coop = data[coop_at] != 0;
    s.coop_town = uint32_t(data[coop_at + 1]) << 24 | uint32_t(data[coop_at + 2]) << 16 |
                  uint32_t(data[coop_at + 3]) << 8 | data[coop_at + 4];
  }
  s.host_address = xnaddr::Make(id);
  return s.Valid() ? std::optional<Session>(s) : std::nullopt;
}

// A message of 'type' with player 1's gamertag (and, for presence, its
// hosted session). Requires g_mutex (reads the hosted session).
std::vector<uint8_t> MessageLocked(MessageType type) {
  const std::string name = CleanName(PlayerName());
  std::vector<uint8_t> m = {type, kVersion, uint8_t(name.size())};
  m.insert(m.end(), name.begin(), name.end());
  if (type == kPresence) {
    Session s = g_hosted;
    s.coop = g_coop_hosting;
    s.coop_town = g_coop_hosting ? g_coop_town : 0;
    for (const QosEntry &q : g_qos) {
      if (q.id == s.id) {
        s.qos_data = q.data;
        s.qos_size = q.size;
      }
    }
    const std::vector<uint8_t> blob = EncodeSession(s);
    m.push_back(uint8_t(blob.size()));
    m.insert(m.end(), blob.begin(), blob.end());
  }
  return m;
}

// Requires g_mutex.
void SendLocked(const std::string &id, MessageType type) {
  const std::vector<uint8_t> m = MessageLocked(type);
  eos::Send(id, eos::kChannelFriends, m.data(), m.size(), type != kPresence);
}

// Requires g_mutex.
void BroadcastPresenceLocked() {
  for (const Stored &f : g_friends)
    SendLocked(f.id, kPresence);
}

// Requires g_mutex. 'id' becomes a friend (or its gamertag is updated).
void AddFriendLocked(const std::string &id, const std::string &gamertag) {
  if (auto it = FindById(g_friends, id); it != g_friends.end()) {
    if (!gamertag.empty())
      it->gamertag = gamertag;
  } else {
    g_friends.push_back({id, gamertag});
    RONL_INFO("Friends: {} ({}) is a friend now", gamertag.empty() ? "?" : gamertag, id);
  }
  eos::Remember(id);
  std::erase_if(g_sent, [&](const Request &s) { return s.id == id; });
  std::erase_if(g_requests, [&](const Request &r) { return r.id == id; });
  SaveLocked();
}

// A friend message, on the EOS thread.
void OnMessage(const std::string &id, const uint8_t *data, size_t size) {
  if (size < 3 || data[1] != kVersion)
    return;
  const size_t name_size = std::min<size_t>(data[2], size - 3);
  const std::string gamertag = CleanName(std::string_view(reinterpret_cast<const char *>(data + 3), name_size));
  const uint8_t *rest = data + 3 + name_size;
  const size_t rest_size = size - 3 - name_size;

  std::lock_guard lock(g_mutex);
  LoadLocked();
  const bool is_friend = FindById(g_friends, id) != g_friends.end();
  switch (data[0]) {
  case kRequest:
    if (is_friend || FindById(g_sent, id) != g_sent.end()) {
      // A friend that lost us, or both asked: friends, and it is told so.
      AddFriendLocked(id, gamertag);
      SendLocked(id, kAccept);
    } else if (FindById(g_requests, id) == g_requests.end() && g_requests.size() < kMaxRequests) {
      g_requests.push_back({id, gamertag});
      RONL_INFO("Friends: friend request from {} ({})", gamertag, id);
    }
    break;
  case kAccept:
    if (is_friend || FindById(g_sent, id) != g_sent.end())
      AddFriendLocked(id, gamertag);
    break;
  case kDecline:
    if (FindById(g_sent, id) != g_sent.end()) {
      std::erase_if(g_sent, [&](const Request &s) { return s.id == id; });
      SaveLocked();
      RONL_INFO("Friends: {} ({}) declined the friend request", gamertag, id);
    }
    break;
  case kRemove:
    if (is_friend) {
      std::erase_if(g_friends, [&](const Stored &f) { return f.id == id; });
      g_live.erase(id);
      SaveLocked();
      RONL_INFO("Friends: {} ({}) removed you as a friend", gamertag, id);
    }
    break;
  case kPresence:
    if (is_friend) {
      auto known = g_live.find(id);
      const bool was_online = known != g_live.end() && Clock::now() - known->second.heard_at < kOnlineFor;
      // Just came online: it hears from us now, not at our next tick.
      if (!was_online)
        SendLocked(id, kPresence);
      Live &live = g_live[id];
      live.gamertag = gamertag;
      live.heard_at = Clock::now();
      live.session = {};
      if (rest_size >= 1) {
        const size_t blob_size = std::min<size_t>(rest[0], rest_size - 1);
        if (auto session = DecodeSession(rest + 1, blob_size, id))
          live.session = *session;
      }
      auto stored = FindById(g_friends, id);
      if (!gamertag.empty() && stored->gamertag != gamertag) {
        stored->gamertag = gamertag;
        SaveLocked();
      }
    }
    break;
  default:
    break;
  }
}

// Every couple of seconds, on the EOS thread: presence to every friend, and
// the unanswered requests again now and then.
void Tick() {
  std::lock_guard lock(g_mutex);
  LoadLocked();
  BroadcastPresenceLocked();
  if (g_ticks++ % kRequestResendTicks == 0) {
    for (const Request &s : g_sent)
      SendLocked(s.id, kRequest);
  }
}

// The connection to a player opened or closed, on the EOS thread. A friend
// reached is told our presence at once (so each sees the other online within
// a round trip, not a tick or two later); one whose connection closed is
// offline now, not once its presence has been missing for kOnlineFor.
void OnPeer(const std::string &id, bool connected) {
  std::lock_guard lock(g_mutex);
  LoadLocked();
  if (FindById(g_friends, id) == g_friends.end())
    return;
  if (connected) {
    SendLocked(id, kPresence);
  } else if (g_live.erase(id)) {
    RONL_INFO("Friends: lost the connection to {}: offline", id);
  }
}

[[maybe_unused]] const bool g_registered = [] {
  eos::SetReceiver(eos::kChannelFriends, &OnMessage);
  eos::AddTicker(&Tick);
  eos::AddPeerListener(&OnPeer);
  return true;
}();

} // namespace

bool Session::Valid() const {
  return std::any_of(id.begin(), id.end(), [](uint8_t b) { return b != 0; });
}

bool EnsureStarted() {
  return Enabled();
}

std::vector<Friend> Friends() {
  std::vector<Friend> out;
  std::lock_guard lock(g_mutex);
  LoadLocked();
  const auto now = Clock::now();
  for (const Stored &f : g_friends) {
    Friend entry;
    entry.id = f.id;
    entry.xuid = eos::XuidOf(f.id);
    entry.gamertag = f.gamertag;
    entry.title_id = kTitleId;
    auto live = g_live.find(f.id);
    if (live != g_live.end() && now - live->second.heard_at < kOnlineFor) {
      entry.online = true;
      if (!live->second.gamertag.empty())
        entry.gamertag = live->second.gamertag;
      entry.state = kStateOnline | kStatePlaying;
      entry.session = live->second.session;
      if (entry.session.Valid() && entry.session.joinable)
        entry.state |= kStateJoinable;
    }
    out.push_back(std::move(entry));
  }
  return out;
}

std::vector<Request> Requests() {
  std::lock_guard lock(g_mutex);
  LoadLocked();
  return g_requests;
}

std::vector<Request> SentRequests() {
  std::lock_guard lock(g_mutex);
  LoadLocked();
  return g_sent;
}

void SendRequest(const std::string &id) {
  const std::string parsed = eos::ParseId(id);
  if (parsed.empty() || parsed == eos::OwnId())
    return;
  std::lock_guard lock(g_mutex);
  LoadLocked();
  if (FindById(g_friends, parsed) != g_friends.end())
    return;
  if (FindById(g_requests, parsed) != g_requests.end()) {
    // It asked us already: this answers it.
    const std::string gamertag = FindById(g_requests, parsed)->gamertag;
    AddFriendLocked(parsed, gamertag);
    SendLocked(parsed, kAccept);
    return;
  }
  if (FindById(g_sent, parsed) == g_sent.end()) {
    g_sent.push_back({parsed, {}});
    SaveLocked();
  }
  eos::Remember(parsed);
  SendLocked(parsed, kRequest);
  RONL_INFO("Friends: friend request sent to {}", parsed);
}

void CancelRequest(const std::string &id) {
  std::lock_guard lock(g_mutex);
  LoadLocked();
  std::erase_if(g_sent, [&](const Request &s) { return s.id == id; });
  SaveLocked();
}

void Accept(const std::string &id) {
  std::lock_guard lock(g_mutex);
  LoadLocked();
  auto it = FindById(g_requests, id);
  if (it == g_requests.end())
    return;
  const std::string gamertag = it->gamertag;
  AddFriendLocked(id, gamertag);
  SendLocked(id, kAccept);
  SendLocked(id, kPresence);
}

void Decline(const std::string &id) {
  std::lock_guard lock(g_mutex);
  LoadLocked();
  if (FindById(g_requests, id) == g_requests.end())
    return;
  std::erase_if(g_requests, [&](const Request &r) { return r.id == id; });
  SendLocked(id, kDecline);
}

void Remove(const std::string &id) {
  std::lock_guard lock(g_mutex);
  LoadLocked();
  if (FindById(g_friends, id) == g_friends.end())
    return;
  std::erase_if(g_friends, [&](const Stored &f) { return f.id == id; });
  g_live.erase(id);
  SaveLocked();
  SendLocked(id, kRemove);
}

void SetHostedSession(const Session &session) {
  std::lock_guard lock(g_mutex);
  LoadLocked();
  g_hosted = session;
  BroadcastPresenceLocked();
}

Session HostedSession() {
  std::lock_guard lock(g_mutex);
  return g_hosted;
}

void SetQosData(const SessionId &id, const uint8_t *data, size_t size) {
  std::lock_guard lock(g_mutex);
  LoadLocked();
  auto it = std::find_if(g_qos.begin(), g_qos.end(), [&](const QosEntry &q) { return q.id == id; });
  if (!size || !data) {
    if (it != g_qos.end())
      g_qos.erase(it);
  } else {
    if (it == g_qos.end()) {
      g_qos.push_back({id});
      it = g_qos.end() - 1;
    }
    it->size = uint8_t(std::min(size, it->data.size()));
    it->data = {};
    std::memcpy(it->data.data(), data, it->size);
  }
  if (g_hosted.Valid() && g_hosted.id == id)
    BroadcastPresenceLocked();
}

void SetCoopHosting(bool coop, uint32_t town) {
  std::lock_guard lock(g_mutex);
  LoadLocked();
  g_coop_hosting = coop;
  g_coop_town = town;
  BroadcastPresenceLocked();
}

std::optional<Session> FindFriendSession(const SessionId &id) {
  for (const Friend &f : Friends()) {
    if (f.online && f.session.Valid() && f.session.id == id) {
      Session session = f.session;
      session.rtt_ms = kEstimatedRttMs;
      return session;
    }
  }
  return std::nullopt;
}

} // namespace renut::online::friends

// ---------------------------------------------------------------------------
// XFriendsCreateEnumerator
// ---------------------------------------------------------------------------

namespace {

using namespace renut::online;

constexpr uint32_t kFriendSize = 196;
constexpr uint32_t kRichPresenceChars = 64;

// The friends a handle returns, written as XONLINE_FRIEND. An enumerator of
// reNut's own: the SDK's static enumerator's vtable is not exported from its DLL.
class FriendsEnumerator : public rex::system::XEnumerator {
public:
  FriendsEnumerator(rex::system::KernelState *kernel_state, size_t per_call,
                    std::vector<uint8_t> items)
      : XEnumerator(kernel_state, per_call, kFriendSize), items_(std::move(items)) {}

  uint32_t WriteItems(uint32_t, uint8_t *buffer, uint32_t *written_count) override {
    const size_t total = items_.size() / kFriendSize;
    const size_t count = std::min(total - next_, items_per_enumerate());
    if (!count)
      return X_ERROR_NO_MORE_FILES;
    std::memcpy(buffer, items_.data() + next_ * kFriendSize, count * kFriendSize);
    next_ += count;
    if (written_count)
      *written_count = uint32_t(count);
    return X_ERROR_SUCCESS;
  }

private:
  std::vector<uint8_t> items_;
  size_t next_ = 0;
};

void StoreBe32(uint8_t *p, uint32_t v) {
  *reinterpret_cast<uint32_t *>(p) = rex::byte_swap(v);
}

void StoreBe64(uint8_t *p, uint64_t v) {
  *reinterpret_cast<uint64_t *>(p) = rex::byte_swap(v);
}

void WriteFriend(uint8_t *item, const friends::Friend &f) {
  std::memset(item, 0, kFriendSize);
  StoreBe64(item + 0, f.xuid);
  std::memcpy(item + 8, f.gamertag.data(), std::min<size_t>(f.gamertag.size(), 15));
  StoreBe32(item + 24, f.state);
  std::memcpy(item + 28, f.session.id.data(), f.session.id.size());
  StoreBe32(item + 36, f.title_id);
  static constexpr char kPresence[] = "Playing Nuts & Bolts on reNut";
  const uint32_t length = std::min<uint32_t>(sizeof(kPresence) - 1, kRichPresenceChars - 1);
  StoreBe32(item + 64, length + 1);
  for (uint32_t i = 0; i < length; ++i)
    *reinterpret_cast<uint16_t *>(item + 68 + 2 * i) = rex::byte_swap(uint16_t(kPresence[i]));
}

} // namespace

// DWORD XFriendsCreateEnumerator(DWORD user, DWORD start, DWORD count,
//                                DWORD *buffer_size, HANDLE *enumerator)
REX_HOOK_RAW(rex_XFriendsCreateEnumerator) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t start = ctx.r4.u32;
  const uint32_t count = ctx.r5.u32;
  const uint32_t buffer_size = ctx.r6.u32;
  const uint32_t handle_out = ctx.r7.u32;
  if (!Enabled() || user != 0) {
    __imp__rex_XFriendsCreateEnumerator(ctx, base);
    return;
  }
  if (!count || !buffer_size || !handle_out) {
    ctx.r3.u64 = X_ERROR_INVALID_PARAMETER;
    return;
  }
  // The friends online: the game's menu lists those playing N&B.
  std::vector<friends::Friend> list = friends::Friends();
  std::erase_if(list, [](const friends::Friend &f) { return !f.online; });
  std::vector<uint8_t> items;
  for (size_t i = start; i < list.size() && i < size_t(start) + count; ++i) {
    items.resize(items.size() + kFriendSize);
    WriteFriend(items.data() + items.size() - kFriendSize, list[i]);
  }

  auto enumerator = rex::system::object_ref<FriendsEnumerator>(
      new FriendsEnumerator(REX_KERNEL_STATE(), count, std::move(items)));
  // XAM's friends app.
  if (XFAILED(enumerator->Initialize(user, 0xFA, 0x58004, 0x58005, 0))) {
    ctx.r3.u64 = X_ERROR_FUNCTION_FAILED;
    return;
  }
  *reinterpret_cast<uint32_t *>(base + buffer_size) = rex::byte_swap(count * kFriendSize);
  *reinterpret_cast<uint32_t *>(base + handle_out) = rex::byte_swap(uint32_t(enumerator->handle()));
  ctx.r3.u64 = X_ERROR_SUCCESS;
}
